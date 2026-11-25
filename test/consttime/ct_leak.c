/*
 * Length-dependent constant-time leakage test.
 *
 * For every length, two classes are measured:
 *   - first byte differs after zero matching bytes
 *   - last byte differs after len - 1 matching bytes
 *
 * A constant-time implementation has the same cost distribution for both
 * classes.  A short-circuiting memcmp returns as soon as the first mismatch
 * is found, so its timing is visibly correlated with the matching prefix
 * length.  Welch's t-test is used over interleaved samples; |t| above the
 * configured threshold is reported as leakage.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sodium.h"

int ct_fixture_libc_compare(const unsigned char *expected_key,
                            const unsigned char *received_key, size_t key_len);
int ct_fixture_leaky_compare(const unsigned char *expected_key,
                             const unsigned char *received_key, size_t key_len);
int ct_fixture_constant_time_compare(const unsigned char *expected_key,
                                     const unsigned char *received_key,
                                     size_t key_len);

typedef int (*compare_fn)(const unsigned char *, const unsigned char *, size_t);

typedef struct {
    const char *name;
    compare_fn  fn;
} implementation;

typedef struct {
    double mean[2];
    double t;
} timing_result;

static uint64_t rng_state = UINT64_C(0x9e3779b97f4a7c15);

static uint64_t
next_random(void)
{
    uint64_t x = rng_state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * UINT64_C(0x2545f4914f6cdd1d);
}

static uint64_t
now_ns(void)
{
    struct timespec ts;

#ifdef CLOCK_MONOTONIC_RAW
    const clockid_t clock_id = CLOCK_MONOTONIC_RAW;
#else
    const clockid_t clock_id = CLOCK_MONOTONIC;
#endif
    if (clock_gettime(clock_id, &ts) != 0) {
        fprintf(stderr, "clock_gettime() failed: %s\n", strerror(errno));
        exit(2);
    }
    return (uint64_t) ts.tv_sec * UINT64_C(1000000000) +
           (uint64_t) ts.tv_nsec;
}

static double
sample_mean(const uint64_t *samples, size_t count)
{
    uint64_t sum = 0U;
    size_t   i;

    for (i = 0U; i < count; i++) {
        sum += samples[i];
    }
    return (double) sum / (double) count;
}

static double
sample_variance(const uint64_t *samples, size_t count, double mean)
{
    double sum = 0.0;
    size_t i;

    for (i = 0U; i < count; i++) {
        const double delta = (double) samples[i] - mean;
        sum += delta * delta;
    }
    if (count < 2U) {
        return 0.0;
    }
    return sum / (double) (count - 1U);
}

static timing_result
welch_t_test(const uint64_t *first, const uint64_t *second, size_t count)
{
    timing_result result;
    double        var_first;
    double        var_second;
    double        denominator;

    result.mean[0] = sample_mean(first, count);
    result.mean[1] = sample_mean(second, count);
    var_first = sample_variance(first, count, result.mean[0]);
    var_second = sample_variance(second, count, result.mean[1]);
    denominator = sqrt(var_first / (double) count +
                       var_second / (double) count);
    if (denominator == 0.0) {
        result.t = result.mean[0] == result.mean[1] ? 0.0 : INFINITY;
    } else {
        result.t = (result.mean[0] - result.mean[1]) / denominator;
    }
    return result;
}

static size_t
choose_batch_size(size_t len)
{
    size_t batch = (size_t) 8192U / len;

    if (batch < 16U) {
        return 16U;
    }
    if (batch > 512U) {
        return 512U;
    }
    return batch;
}

static timing_result
measure_implementation(compare_fn fn, unsigned char *a, unsigned char *b,
                       const unsigned char *base, size_t len, size_t samples)
{
    uint64_t *first = (uint64_t *) calloc(samples, sizeof *first);
    uint64_t *last  = (uint64_t *) calloc(samples, sizeof *last);
    size_t    batch = choose_batch_size(len);
    size_t    i;
    size_t    j;
    int       sink = 0;
    timing_result result;

    if (first == NULL || last == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }

    for (j = 0U; j < 64U; j++) {
        memcpy(a, base, len);
        memcpy(b, base, len);
        b[0] ^= 0x80U;
        for (i = 0U; i < batch; i++) {
            sink ^= fn(a, b, len);
        }
    }
    for (i = 0U; i < samples; i++) {
        const uint64_t order = next_random();
        const bool     run_second_first = (order & 1U) != 0U;
        unsigned int   pass;

        for (pass = 0U; pass < 2U; pass++) {
            const bool second = run_second_first ? (pass == 0U)
                                                 : (pass == 1U);
            uint64_t   start;
            uint64_t   elapsed;

            memcpy(a, base, len);
            memcpy(b, base, len);
            if (second) {
                b[len - 1U] ^= 0x80U;
            } else {
                b[0] ^= 0x80U;
            }
            start = now_ns();
            for (j = 0U; j < batch; j++) {
                sink ^= fn(a, b, len);
            }
            elapsed = now_ns() - start;
            if (second) {
                last[i] = elapsed;
            } else {
                first[i] = elapsed;
            }
        }
    }
    __asm__ __volatile__("" : "+r"(sink) : : "memory");

    result = welch_t_test(first, last, samples);
    free(first);
    free(last);
    return result;
}

static const size_t default_lengths[] = { 16U, 32U, 64U, 256U, 1024U, 4096U };

int
main(void)
{
    static const implementation implementations[] = {
        { "libc-memcmp", ct_fixture_libc_compare },
        { "leaky-loop", ct_fixture_leaky_compare },
        { "sodium_memcmp", ct_fixture_constant_time_compare },
    };
    const size_t length_count =
        sizeof default_lengths / sizeof default_lengths[0];
    const char *samples_env = getenv("CONSTTIME_SAMPLES");
    const char *threshold_env = getenv("CONSTTIME_T_THRESHOLD");
    const char *lengths_env = getenv("CONSTTIME_LENGTHS");
    size_t        samples = 200U;
    double        threshold = 6.0;
    const size_t *lengths = default_lengths;
    size_t        requested_lengths = length_count;
    size_t       *parsed_lengths = NULL;
    size_t        ylen = 0U;
    bool        failed = false;
    bool        leaky_detected = false;
    size_t      impl_index;
    size_t      i;

    if (sodium_init() < 0) {
        fprintf(stderr, "sodium_init() failed\n");
        return 2;
    }
    if (samples_env != NULL) {
        long value = strtol(samples_env, NULL, 10);
        if (value > 0) {
            samples = (size_t) value;
        }
    }
    if (threshold_env != NULL) {
        double value = strtod(threshold_env, NULL);
        if (value > 0.0) {
            threshold = value;
        }
    }
    if (lengths_env != NULL) {
        char  *copy = strdup(lengths_env);
        char  *token;
        char  *saveptr = NULL;

        if (copy == NULL) {
            fprintf(stderr, "out of memory\n");
            return 2;
        }
        parsed_lengths = (size_t *) calloc(strlen(lengths_env) + 1U,
                                           sizeof *parsed_lengths);
        if (parsed_lengths == NULL) {
            free(copy);
            fprintf(stderr, "out of memory\n");
            return 2;
        }
        lengths = parsed_lengths;
        token = strtok_r(copy, ",", &saveptr);
        while (token != NULL) {
            long value = strtol(token, NULL, 10);
            if (value > 1 && (size_t) value <= SIZE_MAX / 2U) {
                parsed_lengths[ylen++] = (size_t) value;
            }
            token = strtok_r(NULL, ",", &saveptr);
        }
        free(copy);
        requested_lengths = ylen;
        if (requested_lengths == 0U) {
            fprintf(stderr, "CONSTTIME_LENGTHS did not contain a valid length\n");
            free(parsed_lengths);
            return 2;
        }
    }

    {
        size_t max_len = lengths[0];
        for (i = 1U; i < requested_lengths; i++) {
            if (lengths[i] > max_len) {
                max_len = lengths[i];
            }
        }
        {
            unsigned char *a = (unsigned char *) sodium_malloc(max_len);
            unsigned char *b = (unsigned char *) sodium_malloc(max_len);
            unsigned char *base = (unsigned char *) sodium_malloc(max_len);

            if (a == NULL || b == NULL || base == NULL) {
                fprintf(stderr, "sodium_malloc() failed\n");
                sodium_free(a);
                sodium_free(b);
                sodium_free(base);
                free(parsed_lengths);
                return 2;
            }
            for (i = 0U; i < max_len; i++) {
                base[i] = (unsigned char) (next_random() & 0xffU);
            }

            printf("Welch t-test, |t| threshold %.1f, %zu samples per class\n",
                   threshold, samples);
            printf("%-8s %-24s %-10s %-10s %s\n",
                   "length", "implementation", "first-ns", "last-ns", "|t|");
            printf("%-8s %-24s %-10s %-10s %s\n",
                   "------", "--------------", "--------", "-------", "---");

            for (i = 0U; i < requested_lengths; i++) {
                const size_t len = lengths[i];
                size_t       tindex;

                for (tindex = 0U;
                     tindex < sizeof implementations / sizeof implementations[0];
                     tindex++) {
                    const implementation *impl = &implementations[tindex];
                    timing_result result =
                        measure_implementation(impl->fn, a, b, base, len,
                                               samples);
                    const double abs_t = fabs(result.t);

                    printf("%-8zu %-24s %-10.1f %-10.1f %.2f\n", len,
                           impl->name, result.mean[0], result.mean[1], abs_t);
                    if (strcmp(impl->name, "sodium_memcmp") == 0 &&
                        !(abs_t <= threshold)) {
                        fprintf(stderr,
                                "FAIL: sodium_memcmp leaked at length %zu (|t|=%.2f)\n",
                                len, abs_t);
                        failed = true;
                    }
                    if (strcmp(impl->name, "leaky-loop") == 0 &&
                        abs_t > threshold) {
                        leaky_detected = true;
                    }
                }
            }
            sodium_free(a);
            sodium_free(b);
            sodium_free(base);
        }
    }
    free(parsed_lengths);

    if (!leaky_detected) {
        fprintf(stderr,
                "FAIL: the timing test did not detect the leaky-loop control\n");
        failed = true;
    }
    if (failed) {
        return 1;
    }
    printf("PASS: long matching prefixes leaked in the control and "
           "sodium_memcmp stayed within the threshold\n");
    return 0;
}
