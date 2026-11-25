#define TEST_NAME "consttime"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sodium.h"

#include "cmptest.h"

static void
check_verify_fixed_size(void)
{
    unsigned char a16[16];
    unsigned char b16[16];
    unsigned char a32[32];
    unsigned char b32[32];
    unsigned char a64[64];
    unsigned char b64[64];
    size_t        i;

    for (i = 0U; i < sizeof a64; i++) {
        a64[i] = (unsigned char) (i * 37U + 11U);
        b64[i] = a64[i];
        if (i < sizeof a32) {
            a32[i] = a64[i];
            b32[i] = a64[i];
        }
        if (i < sizeof a16) {
            a16[i] = a64[i];
            b16[i] = a64[i];
        }
    }
    assert(crypto_verify_16(a16, b16) == 0);
    assert(crypto_verify_32(a32, b32) == 0);
    assert(crypto_verify_64(a64, b64) == 0);
    assert(sodium_memcmp(a16, b16, sizeof a16) == 0);
    assert(sodium_memcmp(a32, b32, sizeof a32) == 0);
    assert(sodium_memcmp(a64, b64, sizeof a64) == 0);

    b16[0] ^= 0x01U;
    b32[sizeof b32 / 2U] ^= 0x80U;
    b64[sizeof b64 - 1U] ^= 0x01U;
    assert(crypto_verify_16(a16, b16) == -1);
    assert(crypto_verify_32(a32, b32) == -1);
    assert(crypto_verify_64(a64, b64) == -1);
    assert(sodium_memcmp(a16, b16, sizeof a16) == -1);
    assert(sodium_memcmp(a32, b32, sizeof a32) == -1);
    assert(sodium_memcmp(a64, b64, sizeof a64) == -1);
}

static void
check_variable_size(void)
{
    static const size_t lengths[] = {
        0U, 1U, 15U, 16U, 31U, 32U, 63U, 64U, 255U, 256U, 1024U
    };
    unsigned char a[1024];
    unsigned char b[1024];
    size_t        i;
    size_t        length_index;

    for (i = 0U; i < sizeof a; i++) {
        a[i] = (unsigned char) (i * 29U + 7U);
    }
    for (length_index = 0U;
         length_index < sizeof lengths / sizeof lengths[0];
         length_index++) {
        const size_t len = lengths[length_index];

        if (len == 0U) {
            assert(sodium_memcmp(NULL, NULL, 0U) == 0);
            continue;
        }
        memcpy(b, a, len);
        assert(sodium_memcmp(a, b, len) == 0);

        b[0] ^= 0x01U;
        assert(sodium_memcmp(a, b, len) == -1);
        b[0] ^= 0x01U;

        b[len / 2U] ^= 0x80U;
        assert(sodium_memcmp(a, b, len) == -1);
        b[len / 2U] ^= 0x80U;

        b[len - 1U] ^= 0x01U;
        assert(sodium_memcmp(a, b, len) == -1);
    }
}

int
main(void)
{
    if (sodium_init() < 0) {
        return 1;
    }
    check_verify_fixed_size();
    check_variable_size();
    printf("consttime: ok\n");
    return 0;
}
