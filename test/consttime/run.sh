#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="${CONSTTIME_WORK:-/tmp/libsodium-consttime}"
CC="${CC:-cc}"
SAMPLES="${CONSTTIME_SAMPLES:-400}"
THRESHOLD="${CONSTTIME_T_THRESHOLD:-6.0}"
LENGTHS="${CONSTTIME_LENGTHS:-16,32,64,256,1024,4096}"

rm -rf "$WORK"
mkdir -p "$WORK"

SCANNER="$ROOT/test/consttime/consttime_check.py"
HARNESS="$ROOT/test/consttime/ct_leak.c"
FIXTURE="$ROOT/test/consttime/fixtures/bad_compare.c"
INCLUDE="$ROOT/src/libsodium/include"
LIBDIR="$ROOT/src/libsodium/.libs"

if [ ! -f "$HARNESS" ] || [ ! -f "$SCANNER" ] || [ ! -f "$FIXTURE" ]; then
    echo "consttime acceptance files are missing under $ROOT/test/consttime" >&2
    exit 2
fi

if [ -n "${LIBSODIUM_BUILD:-}" ] &&
   [ -f "$LIBSODIUM_BUILD/src/libsodium/include/sodium/version.h" ]; then
    BUILD_INCLUDE="$LIBSODIUM_BUILD/src/libsodium/include"
elif [ -f "$ROOT/src/libsodium/include/sodium/version.h" ]; then
    BUILD_INCLUDE="$ROOT/src/libsodium/include"
else
    BUILD_INCLUDE="$(find "$ROOT" -maxdepth 8 \
        -path '*/src/libsodium/include/sodium/version.h' \
        -not -path '*/.git/*' -print -quit 2>/dev/null || true)"
    if [ -n "$BUILD_INCLUDE" ]; then
        BUILD_INCLUDE="${BUILD_INCLUDE%/sodium/version.h}"
    fi
fi
if [ -z "${BUILD_INCLUDE:-}" ]; then
    echo "generated sodium/version.h not found; set LIBSODIUM_BUILD" >&2
    exit 2
fi
INCLUDES=(-I"$BUILD_INCLUDE" -I"$BUILD_INCLUDE/sodium" -I"$INCLUDE" \
          -I"$INCLUDE/sodium")

if [ ! -f "$LIBDIR/libsodium.a" ] &&
   ! compgen -G "$LIBDIR/libsodium.so*" >/dev/null; then
    CANDIDATE="${BUILD_INCLUDE%/include}/.libs"
    if [ -f "$CANDIDATE/libsodium.a" ] ||
       compgen -G "$CANDIDATE/libsodium.so*" >/dev/null; then
        LIBDIR="$CANDIDATE"
    fi
fi

if [ -n "${LIBSODIUM_LIB:-}" ]; then
    LIB="$LIBSODIUM_LIB"
    LIB_LINK="$LIB"
elif [ -f "$LIBDIR/libsodium.a" ]; then
    LIB="$LIBDIR/libsodium.a"
    LIB_LINK="$LIB"
elif compgen -G "$LIBDIR/libsodium.so*" >/dev/null; then
    LIB="$(compgen -G "$LIBDIR/libsodium.so*" | head -n 1)"
    LIB_LINK="-L$LIBDIR -Wl,-rpath,$LIBDIR -lsodium"
else
    echo "built libsodium not found under $LIBDIR" >&2
    echo "run ./configure && make first, or set LIBSODIUM_LIB" >&2
    exit 2
fi

echo "== static comparison check =="
set +e
python3 "$SCANNER" "$ROOT/src/libsodium" >"$WORK/source-scan.txt" 2>&1
SOURCE_RC=$?
set -e
if [ "$SOURCE_RC" -ne 0 ]; then
    cat "$WORK/source-scan.txt"
    echo "FAIL: production source still contains variable-time comparisons" >&2
    exit 1
fi
cat "$WORK/source-scan.txt"

echo
echo "== locate a deliberately bad comparison in test code =="
python3 "$SCANNER" --json --no-fail "$FIXTURE" >"$WORK/fixture-scan.json"
python3 - "$WORK/fixture-scan.json" "$FIXTURE" <<'PY'
import json
import sys
from pathlib import Path

findings = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
fixture = Path(sys.argv[2]).resolve()
matches = [
    finding
    for finding in findings
    if Path(finding["path"]).resolve() == fixture
    and finding["call"] == "memcmp"
    and finding["function"] == "ct_fixture_libc_compare"
]
if not matches:
    raise SystemExit("FAIL: test fixture memcmp was not located")
finding = matches[0]
print(
    f"located {finding['path']}:{finding['line']}:{finding['column']} "
    f"{finding['call']}() in {finding['function']}()"
)
print(f"source line: {finding['source']}")
PY

echo
echo "== length-dependent timing check =="
"$CC" -O2 -fno-builtin-memcmp -fno-builtin "${INCLUDES[@]}" \
    "$HARNESS" "$FIXTURE" $LIB_LINK -lm -o "$WORK/ct_leak"
CONSTTIME_SAMPLES="$SAMPLES" \
CONSTTIME_T_THRESHOLD="$THRESHOLD" \
CONSTTIME_LENGTHS="$LENGTHS" \
    "$WORK/ct_leak"

echo
echo "== sodium_memcmp compatibility regression =="
if [ -x "$ROOT/test/default/consttime" ]; then
    (cd "$ROOT/test/default" && ./consttime)
elif [ -f "$ROOT/test/default/Makefile" ]; then
    make -C "$ROOT/test/default" consttime >/dev/null
    (cd "$ROOT/test/default" && ./consttime)
else
    "$CC" -O2 "${INCLUDES[@]}" -I"$ROOT/test/default" -I"$ROOT/test/quirks" \
        "$ROOT/test/default/consttime.c" $LIB_LINK -o "$WORK/consttime-unit"
    (cd "$ROOT/test/default" && "$WORK/consttime-unit")
    rm -f "$ROOT/test/default/consttime.res"
fi

echo
echo "consttime acceptance: PASS"
