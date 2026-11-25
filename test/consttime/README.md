# Constant-time comparison acceptance

This acceptance suite covers the audit finding that key comparisons must not
use content-dependent, short-circuiting routines such as `memcmp`.

## What is checked

1. `consttime_check.py` statically scans a C source tree and reports
   `memcmp` and `bcmp` calls as `file:line:column`; `--strings` additionally
   reports `strcmp`, `strncmp`, `strcasecmp` and `strncasecmp`.  It masks
   comments and strings, recognizes
   `sodium_memcmp()`, `crypto_verify_*()`, `sodium_compare()` and
   `sodium_is_zero()` as safe, and supports `consttime: public-data` on a
   call or the preceding line for intentional non-secret comparisons.
2. The same scanner is run against
   `fixtures/bad_compare.c`; the acceptance run must print the exact
   fixture path, line and column of its `memcmp()` call.
3. `ct_leak.c` sweeps 16, 32, 64, 256, 1024 and 4096 byte inputs.  For each
   length it measures two classes: the first byte differs (zero matching
   prefix bytes), and the last byte differs (`len - 1` matching prefix
   bytes).  Samples are interleaved and compared with Welch's t-test.
   A short-circuiting control must be detected and `sodium_memcmp()` must
   stay under the threshold.
4. `test/default/consttime.c` is part of `make check` and verifies the
   existing API contract: `0` for equal inputs and `-1` for unequal inputs,
   including zero-length and fixed sizes used by `crypto_verify_16/32/64`.

## Run

After building libsodium:

```sh
./test/consttime/run.sh
# or
make consttime-check
```

The runner uses `src/libsodium/.libs/libsodium.a` (or a shared library in
that directory) by default.  Set `LIBSODIUM_LIB` to test another build.

Useful environment variables:

```sh
CONSTTIME_LENGTHS=16,64,1024 CONSTTIME_SAMPLES=800 ./test/consttime/run.sh
CONSTTIME_T_THRESHOLD=4.5 ./test/consttime/run.sh
```

The statistical check is intentionally a negative/positive control pair:
the leaky loop demonstrates that the detector notices matching-prefix length
leakage, while the shipped `sodium_memcmp()` demonstrates that the production
comparison does not leak after the fixes.

## Production fixes

The audit found four remaining `memcmp()` calls in production code:

- `src/libsodium/sodium/codecs.c`
- `src/libsodium/crypto_ipcrypt/ipcrypt_soft.c`
- `src/libsodium/crypto_ipcrypt/ipcrypt_armcrypto.c`
- `src/libsodium/crypto_ipcrypt/ipcrypt_aesni.c`

They now use `sodium_memcmp()` with the same `== 0` / boolean result.  The
static scan over `src/libsodium` therefore has no findings.  Existing public
signatures and return values are unchanged; `sodium_memcmp()` remains the
arbitrary-length constant-time API and `crypto_verify_16/32/64` retain their
fixed-size `0` / `-1` semantics.
