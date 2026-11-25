#!/usr/bin/env python3
"""Locate variable-time comparisons in C-family source trees.

The checker masks comments and string literals before looking for calls to
known short-circuiting comparison routines.  ``sodium_memcmp()``,
``crypto_verify_*()``, ``sodium_compare()`` and ``sodium_is_zero()`` are
recognized as the constant-time APIs shipped by libsodium.

An intentional variable-time comparison can be documented on the call line
or on the preceding line with ``consttime: public-data`` (the words
``intentional`` and ``safe`` are also accepted).
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Iterator

VARIABLE_TIME_MEMORY_CALLS = ("memcmp", "bcmp")
VARIABLE_TIME_STRING_CALLS = ("strcmp", "strncmp", "strcasecmp", "strncasecmp")

_SUPPRESS_RE = re.compile(
    r"consttime\s*:\s*(public-data|public|intentional|safe)",
    re.IGNORECASE,
)
_SIGNATURE_RE = re.compile(
    r"(?m)^\s*(?:[A-Za-z_][\w \t\*]*\s+)?"
    r"([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{"
)
_DEFAULT_EXTENSIONS = {".c", ".h", ".cc", ".cpp", ".cxx"}
_NON_FUNCTION_KEYWORDS = {
    "if",
    "for",
    "while",
    "switch",
    "do",
    "else",
    "return",
    "sizeof",
}


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    column: int
    call: str
    function: str | None
    source: str


def _mask_comments_and_strings(text: str) -> str:
    """Replace comments/strings with spaces while preserving newlines."""

    out: list[str] = []
    i = 0
    state = "code"
    quote = ""
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if ch == "/" and nxt == "/":
                out.extend((" ", " "))
                i += 2
                state = "line_comment"
                continue
            if ch == "/" and nxt == "*":
                out.extend((" ", " "))
                i += 2
                state = "block_comment"
                continue
            if ch in ("'", '"'):
                out.append(" ")
                i += 1
                state = "string"
                quote = ch
                continue
            out.append(ch)
            i += 1
            continue
        if state == "line_comment":
            if ch == "\n":
                out.append("\n")
                i += 1
                state = "code"
            else:
                out.append(" ")
                i += 1
            continue
        if state == "block_comment":
            if ch == "*" and nxt == "/":
                out.extend((" ", " "))
                i += 2
                state = "code"
            elif ch == "\n":
                out.append("\n")
                i += 1
            else:
                out.append(" ")
                i += 1
            continue
        # String literal.
        if ch == "\\" and i + 1 < len(text):
            out.append(" ")
            out.append("\n" if nxt == "\n" else " ")
            i += 2
            continue
        if ch == quote:
            out.append(" ")
            i += 1
            state = "code"
            continue
        out.append("\n" if ch == "\n" else " ")
        i += 1
    return "".join(out)


def _enclosing_function(masked: str, position: int) -> str | None:
    prefix = masked[max(0, position - 4096):position]
    match = None
    for candidate in _SIGNATURE_RE.finditer(prefix):
        if candidate.group(1) not in _NON_FUNCTION_KEYWORDS:
            match = candidate
    return match.group(1) if match is not None else None


def _line_is_suppressed(lines: list[str], line_no: int) -> bool:
    index = line_no - 1
    if 0 <= index < len(lines) and _SUPPRESS_RE.search(lines[index]):
        return True
    return index > 0 and bool(_SUPPRESS_RE.search(lines[index - 1]))


def scan_text(path: str, text: str, calls: Iterable[str]) -> Iterator[Finding]:
    var_time_re = re.compile(
        r"(?<![\w])(" + "|".join(calls) + r")\s*\("
    )
    masked = _mask_comments_and_strings(text)
    lines = text.splitlines()
    for match in var_time_re.finditer(masked):
        line_no = masked.count("\n", 0, match.start()) + 1
        line_start = masked.rfind("\n", 0, match.start()) + 1
        if _line_is_suppressed(lines, line_no):
            continue
        source = lines[line_no - 1].strip() if line_no <= len(lines) else ""
        yield Finding(
            path=path,
            line=line_no,
            column=match.start() - line_start + 1,
            call=match.group(1),
            function=_enclosing_function(masked, match.start()),
            source=source,
        )


def iter_source_files(paths: Iterable[Path], extensions: set[str]) -> Iterator[Path]:
    for root in paths:
        if root.is_file():
            if root.suffix in extensions:
                yield root
            continue
        for path in sorted(root.rglob("*")):
            if not path.is_file() or path.suffix not in extensions:
                continue
            if path.name.startswith("."):
                continue
            if any(part in {".git", ".enc"} for part in path.parts):
                continue
            yield path


def format_finding(finding: Finding) -> str:
    scope = f" in {finding.function}()" if finding.function else ""
    return (
        f"{finding.path}:{finding.line}:{finding.column}: "
        f"variable-time comparison {finding.call}(){scope}"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "paths",
        nargs="*",
        default=["src"],
        help="files or directories to scan (default: src)",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="emit machine-readable findings",
    )
    parser.add_argument(
        "--no-fail",
        action="store_true",
        help="always exit successfully, even when findings are present",
    )
    parser.add_argument(
        "--extensions",
        default=",".join(sorted(_DEFAULT_EXTENSIONS)),
        help="comma-separated source extensions",
    )
    parser.add_argument(
        "--strings",
        action="store_true",
        help="also report strcmp/strncmp/strcasecmp/strncasecmp",
    )
    args = parser.parse_args(argv)

    extensions = {
        ext if ext.startswith(".") else f".{ext}"
        for ext in args.extensions.split(",")
        if ext
    }
    calls = VARIABLE_TIME_MEMORY_CALLS
    if args.strings:
        calls += VARIABLE_TIME_STRING_CALLS
    findings: list[Finding] = []
    for path in iter_source_files([Path(p) for p in args.paths], extensions):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            print(f"{path}: cannot read: {exc}", file=sys.stderr)
            return 2
        findings.extend(scan_text(str(path), text, calls))

    if args.json:
        print(json.dumps([asdict(finding) for finding in findings], indent=2))
    else:
        for finding in findings:
            print(format_finding(finding))
        if findings:
            print(
                f"{len(findings)} variable-time comparison(s) found in "
                f"{len({finding.path for finding in findings})} file(s)",
                file=sys.stderr,
            )
        else:
            print("consttime-check: no variable-time comparison calls found")

    if findings and not args.no_fail:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
