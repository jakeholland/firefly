/**
 * fuzz_smoke_main.c — portable, deterministic driver linked against each
 * fuzz_*.c harness (which supplies LLVMFuzzerTestOneInput and nothing
 * else) to make a `fuzz_<name>_smoke` executable that ctest can run on
 * any compiler, no libFuzzer/clang required. See tests/fuzz/README.md.
 *
 * Deliberately NOT a real fuzzer: fixed seed, bounded iteration count,
 * bounded input length. Its only job is to keep the fuzz targets exercised
 * in every normal `ctest` run (docs/specs/S14-testing-ci.md: "fuzz smokes
 * ... run 10k iters in CI") so a regression doesn't sit undetected between
 * the rare human-run, real, timed libFuzzer sessions.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int LLVMFuzzerTestOneInput(uint8_t const *data, size_t size);

/* xorshift32 — same tiny non-cryptographic generator mc_client.c uses for
 * want_config_id; good enough for "spray varied bytes deterministically",
 * not a security property. */
static uint32_t xs_state;

static uint32_t xs_next(void)
{
    uint32_t x = xs_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    xs_state = x;
    return x;
}

#ifndef FF_FUZZ_SMOKE_MAX_LEN
#define FF_FUZZ_SMOKE_MAX_LEN 512u
#endif

#ifndef FF_FUZZ_SMOKE_DEFAULT_ITERS
#define FF_FUZZ_SMOKE_DEFAULT_ITERS 20000u
#endif

static uint8_t s_buf[FF_FUZZ_SMOKE_MAX_LEN];

/* A handful of fixed edge cases run before the random loop, every time,
 * regardless of seed — these are the inputs most likely to hit an
 * off-by-one at a size boundary, and pure random generation would only
 * hit them by chance. */
static void run_fixed_edge_cases(void)
{
    /* Empty input. */
    (void)LLVMFuzzerTestOneInput(s_buf, 0);

    /* All-zero and all-0xFF, at a spread of lengths including the max. */
    static const size_t lens[] = {1, 2, 3, 4, 8, 9, 10, 16, 24, 32, 64, 128,
                                   200, 256, 512};
    for (size_t li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
        size_t n = lens[li];
        if (n > FF_FUZZ_SMOKE_MAX_LEN) {
            continue;
        }
        memset(s_buf, 0x00, n);
        (void)LLVMFuzzerTestOneInput(s_buf, n);
        memset(s_buf, 0xFF, n);
        (void)LLVMFuzzerTestOneInput(s_buf, n);
    }
}

int main(int argc, char **argv)
{
    uint32_t iters = FF_FUZZ_SMOKE_DEFAULT_ITERS;
    if (argc > 1) {
        long v = strtol(argv[1], NULL, 10);
        if (v > 0) {
            iters = (uint32_t)v;
        }
    }

    /* Fixed seed: this driver's whole point is a reproducible smoke check,
     * not real randomness. */
    xs_state = 0x9E3779B9u;

    run_fixed_edge_cases();

    for (uint32_t i = 0; i < iters; i++) {
        uint32_t len_roll = xs_next() % (FF_FUZZ_SMOKE_MAX_LEN + 1u);
        size_t len = (size_t)len_roll;
        for (size_t j = 0; j < len; j++) {
            /* Fill a byte at a time from successive xs_next() draws so a
             * given (seed, iteration index) always reproduces the exact
             * same input bytes regardless of len. */
            if ((j & 3u) == 0u) {
                uint32_t w = xs_next();
                s_buf[j] = (uint8_t)(w & 0xFFu);
                if (j + 1 < len) s_buf[j + 1] = (uint8_t)((w >> 8) & 0xFFu);
                if (j + 2 < len) s_buf[j + 2] = (uint8_t)((w >> 16) & 0xFFu);
                if (j + 3 < len) s_buf[j + 3] = (uint8_t)((w >> 24) & 0xFFu);
            }
        }
        (void)LLVMFuzzerTestOneInput(s_buf, len);
    }

    printf("fuzz smoke: fixed edge cases + %u random iterations, no crash/UB\n",
           (unsigned)iters);
    return 0;
}
