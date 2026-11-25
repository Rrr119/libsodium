/*
 * Deliberately unsafe comparison implementations used by the acceptance
 * checks.  They are compiled into the timing harness as a negative control
 * and scanned by consttime_check.py.
 */

#include <stddef.h>
#include <string.h>

#include "sodium.h"

int
ct_fixture_libc_compare(const unsigned char *expected_key,
                        const unsigned char *received_key, size_t key_len)
{
    return memcmp(expected_key, received_key, key_len);
}

int
ct_fixture_leaky_compare(const unsigned char *expected_key,
                         const unsigned char *received_key, size_t key_len)
{
    const volatile unsigned char *a = expected_key;
    const volatile unsigned char *b = received_key;
    size_t                        i;

    for (i = 0U; i < key_len; i++) {
        if (a[i] != b[i]) {
            return -1;
        }
    }
    return 0;
}

int
ct_fixture_constant_time_compare(const unsigned char *expected_key,
                                 const unsigned char *received_key,
                                 size_t             key_len)
{
    return sodium_memcmp(expected_key, received_key, key_len);
}
