/**
 * test_ticks_at_least_one.c — host-buildable unit test for
 * ff_ticks_at_least_one_calc() (../ff_ticks_at_least_one.h), the pure
 * integer arithmetic behind app_main.c's `ff_ticks_at_least_one()`
 * helper (fix/mic-dump-device-path).
 *
 * app_main.c itself is device-only — it #includes ESP-IDF/FreeRTOS
 * headers that do not exist off-device, so it cannot be built or
 * exercised by this host gate (same honest gap
 * mc_transport_uart_accept.h's own test documents for the identical
 * shape). ff_ticks_at_least_one_calc() was factored into a header with
 * ZERO FreeRTOS includes specifically so the one thing that actually
 * needs a real regression test — "a sub-tick millisecond period must
 * still produce at least one tick, not zero" — has one, parameterized
 * by `tick_rate_hz` so this test can exercise the project's REAL
 * CONFIG_FREERTOS_HZ=100 (firmware/targets/esp32s3/sdkconfig) without
 * linking FreeRTOS at all.
 */
#include <stdint.h>

#include "unity.h"

#include "ff_ticks_at_least_one.h"

void setUp(void) {}
void tearDown(void) {}

/* This project's real tick rate (CONFIG_FREERTOS_HZ=100,
 * firmware/targets/esp32s3/sdkconfig) — 10ms/tick. The exact case that
 * shipped broken: `mic dump`'s FF_MIC_DUMP_POLL_MS=5 poll computed to 0
 * ticks and never actually slept (docs/specs/S30-audio-input.md, "mic
 * dump device notes"). */
enum { FF_TEST_TICK_RATE_HZ = 100u };

/* Every whole-millisecond value below one tick period (10ms) must clamp
 * up to 1 tick, never compute to 0 — the entire point of this helper. */
static void test_sub_tick_ms_clamps_to_one_tick(void)
{
    for (uint32_t ms = 1u; ms < 10u; ms++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, ff_ticks_at_least_one_calc(ms, FF_TEST_TICK_RATE_HZ),
                                          "a sub-tick ms period computed to 0 ticks instead of clamping to 1");
    }
}

/* ms=0 (a caller bug, or a period genuinely configured to 0) still must
 * never yield a 0-tick "sleep" that is really a bare yield — same
 * clamp, same reasoning. */
static void test_zero_ms_clamps_to_one_tick(void)
{
    TEST_ASSERT_EQUAL_UINT32(1u, ff_ticks_at_least_one_calc(0u, FF_TEST_TICK_RATE_HZ));
}

/* Exactly one tick period (10ms at 100Hz): pdMS_TO_TICKS(10) == 1
 * already — the clamp must be a no-op here, not silently inflating an
 * already-correct value. */
static void test_exactly_one_tick_is_unchanged(void)
{
    TEST_ASSERT_EQUAL_UINT32(1u, ff_ticks_at_least_one_calc(10u, FF_TEST_TICK_RATE_HZ));
}

/* Comfortably-above-one-tick periods this file's own polling loops
 * actually use (FF_MIC_DUMP_WRITE_RETRY_MS=20, the render loop's 20ms
 * frame pacing, FF_MIC_WATCH_PERIOD_MS=250, ff_park's 5000ms heartbeat)
 * must pass through EXACTLY as pdMS_TO_TICKS would compute them — the
 * clamp must never fire once a period already resolves to more than one
 * tick. */
static void test_multi_tick_ms_passes_through_unclamped(void)
{
    TEST_ASSERT_EQUAL_UINT32(2u, ff_ticks_at_least_one_calc(20u, FF_TEST_TICK_RATE_HZ));
    TEST_ASSERT_EQUAL_UINT32(25u, ff_ticks_at_least_one_calc(250u, FF_TEST_TICK_RATE_HZ));
    TEST_ASSERT_EQUAL_UINT32(100u, ff_ticks_at_least_one_calc(1000u, FF_TEST_TICK_RATE_HZ));
    TEST_ASSERT_EQUAL_UINT32(500u, ff_ticks_at_least_one_calc(5000u, FF_TEST_TICK_RATE_HZ));
}

/* A different (hypothetical) tick rate is honored, not hard-coded —
 * proves this is the SAME formula pdMS_TO_TICKS uses (parameterized by
 * configTICK_RATE_HZ), not a value pinned to this project's current
 * 100Hz alone. At 1000Hz (1ms/tick) nothing below 1ms could ever exist
 * (ms is a whole number), so the clamp should never need to fire for
 * ms >= 1. */
static void test_different_tick_rate_matches_pdms_to_ticks_formula(void)
{
    enum { FF_TEST_TICK_RATE_1KHZ = 1000u };
    TEST_ASSERT_EQUAL_UINT32(1u, ff_ticks_at_least_one_calc(1u, FF_TEST_TICK_RATE_1KHZ));
    TEST_ASSERT_EQUAL_UINT32(20u, ff_ticks_at_least_one_calc(20u, FF_TEST_TICK_RATE_1KHZ));

    /* At a slow 10Hz tick (100ms/tick), even a "generous-looking" 50ms
     * period still truncates to 0 raw ticks and must clamp to 1. */
    enum { FF_TEST_TICK_RATE_10HZ = 10u };
    TEST_ASSERT_EQUAL_UINT32(1u, ff_ticks_at_least_one_calc(50u, FF_TEST_TICK_RATE_10HZ));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sub_tick_ms_clamps_to_one_tick);
    RUN_TEST(test_zero_ms_clamps_to_one_tick);
    RUN_TEST(test_exactly_one_tick_is_unchanged);
    RUN_TEST(test_multi_tick_ms_passes_through_unclamped);
    RUN_TEST(test_different_tick_rate_matches_pdms_to_ticks_formula);
    return UNITY_END();
}
