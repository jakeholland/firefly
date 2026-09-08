/**
 * test_find.c — S29 PR2 core/find acceptance criteria.
 *
 * Test names follow docs/specs/S29-radio-only.md's own numbered
 * "Tests" list under "PR 2 — FIND mode". All `now_ms` values are plain
 * literals (no fake-clock harness needed): ff_find's entry points take
 * `now_ms` explicitly, same "no hidden clock" shape ff_flare.h/ff_radar.h
 * already use.
 */
#include <string.h>

#include "unity.h"

#include "ff_find.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* Start / stop / single-active-session                                 */
/* ------------------------------------------------------------------- */

static void S29_start_arms_session_but_sends_nothing_yet(void)
{
    ff_find_t f;
    memset(&f, 0, sizeof(f));
    ff_find_start(&f, 42u, 1000u);

    TEST_ASSERT_TRUE(f.active);
    TEST_ASSERT_EQUAL_UINT32(42u, f.target_node_id);
    TEST_ASSERT_EQUAL_UINT32(0u, f.ping_count);
    TEST_ASSERT_FALSE(f.has_their_reading);
}

static void S29_first_tick_sends_immediately_no_10s_wait(void)
{
    ff_find_t f;
    ff_find_start(&f, 42u, 1000u);

    ff_find_result_t r = ff_find_tick(&f, 1000u);
    TEST_ASSERT_EQUAL_INT(FF_FIND_INTENT_SEND_PING, r.intent);
    TEST_ASSERT_EQUAL_UINT32(1u, f.ping_count);
}

static void S29_stop_clears_active(void)
{
    ff_find_t f;
    ff_find_start(&f, 42u, 1000u);
    ff_find_stop(&f);

    TEST_ASSERT_FALSE(f.active);
    ff_find_result_t r = ff_find_tick(&f, 5000u);
    TEST_ASSERT_EQUAL_INT(FF_FIND_INTENT_NONE, r.intent);
}

static void S29_leave_face_cancels_like_stop(void)
{
    ff_find_t f;
    ff_find_start(&f, 42u, 1000u);
    ff_find_leave_face(&f);

    TEST_ASSERT_FALSE(f.active);
}

static void S29_start_on_new_target_cancels_prior_session_and_its_history(void)
{
    ff_find_t f;
    ff_find_start(&f, 42u, 1000u);
    ff_find_tick(&f, 1000u); /* ping_count -> 1 */
    ff_find_on_pong(&f, 42u, 0u, -50, false, 0.0f, 1500u);
    TEST_ASSERT_TRUE(f.has_their_reading);

    ff_find_start(&f, 99u, 2000u); /* different target: replaces outright */
    TEST_ASSERT_EQUAL_UINT32(99u, f.target_node_id);
    TEST_ASSERT_EQUAL_UINT32(0u, f.ping_count);
    TEST_ASSERT_FALSE(f.has_their_reading); /* prior target's reading must not bleed through */
}

/* ------------------------------------------------------------------- */
/* Rate limit (sender side): <= 1 ping / 10s / peer                     */
/* ------------------------------------------------------------------- */

static void S29_tick_faster_than_10s_never_sends_twice_in_the_window(void)
{
    ff_find_t f;
    ff_find_start(&f, 42u, 0u);

    TEST_ASSERT_EQUAL_INT(FF_FIND_INTENT_SEND_PING, ff_find_tick(&f, 0u).intent);
    /* Ticking every 100ms for the next 9.9s: never sends again. */
    for (uint32_t t = 100u; t < FF_FIND_PING_INTERVAL_MS; t += 100u) {
        ff_find_result_t r = ff_find_tick(&f, t);
        TEST_ASSERT_EQUAL_INT_MESSAGE(FF_FIND_INTENT_NONE, r.intent, "sent a second ping inside the 10s window");
    }
    TEST_ASSERT_EQUAL_UINT32(1u, f.ping_count);
}

static void S29_tick_at_exactly_10s_sends_again(void)
{
    ff_find_t f;
    ff_find_start(&f, 42u, 0u);
    ff_find_tick(&f, 0u);

    ff_find_result_t r = ff_find_tick(&f, FF_FIND_PING_INTERVAL_MS);
    TEST_ASSERT_EQUAL_INT(FF_FIND_INTENT_SEND_PING, r.intent);
    TEST_ASSERT_EQUAL_UINT32(2u, f.ping_count);
}

static void S29_each_sent_ping_gets_a_distinct_nonce(void)
{
    ff_find_t f;
    ff_find_start(&f, 42u, 0u);
    uint32_t n1 = ff_find_tick(&f, 0u).nonce;
    uint32_t n2 = ff_find_tick(&f, FF_FIND_PING_INTERVAL_MS).nonce;
    TEST_ASSERT_NOT_EQUAL(n1, n2);
}

/* ------------------------------------------------------------------- */
/* Session cap: 5 minutes / 30 pings, whichever first                   */
/* ------------------------------------------------------------------- */

static void S29_stops_after_30_pings(void)
{
    ff_find_t f;
    ff_find_start(&f, 42u, 0u);

    uint32_t t = 0u;
    for (uint32_t i = 0; i < FF_FIND_MAX_PINGS; i++) {
        ff_find_result_t r = ff_find_tick(&f, t);
        TEST_ASSERT_EQUAL_INT_MESSAGE(FF_FIND_INTENT_SEND_PING, r.intent, "expected a send within the cap");
        t += FF_FIND_PING_INTERVAL_MS;
    }
    TEST_ASSERT_EQUAL_UINT32(FF_FIND_MAX_PINGS, f.ping_count);
    TEST_ASSERT_TRUE(f.active); /* cap reached exactly at the 30th send, not yet auto-stopped */

    ff_find_result_t r31 = ff_find_tick(&f, t);
    TEST_ASSERT_EQUAL_INT(FF_FIND_INTENT_NONE, r31.intent);
    TEST_ASSERT_FALSE(f.active); /* auto-stopped */
}

static void S29_stops_after_5_minutes_even_with_pings_remaining(void)
{
    /* An irregular tick loop (e.g. one that ticks far less often than
     * every 10s) must still hit the wall-clock cap, not dodge it by
     * never reaching 30 sends. */
    ff_find_t f;
    ff_find_start(&f, 42u, 0u);
    ff_find_tick(&f, 0u); /* ping_count -> 1, nowhere near 30 */

    ff_find_result_t r = ff_find_tick(&f, FF_FIND_SESSION_MAX_MS);
    TEST_ASSERT_EQUAL_INT(FF_FIND_INTENT_NONE, r.intent);
    TEST_ASSERT_FALSE(f.active);
}

static void S29_does_not_stop_one_ms_before_5_minutes(void)
{
    ff_find_t f;
    ff_find_start(&f, 42u, 0u);
    ff_find_tick(&f, 0u); /* ping_count -> 1, well under the 30-ping cap */

    /* One ms short of the wall-clock cap: the session is still active
     * and (since well over 10s has passed since the single earlier
     * ping) still due to send. */
    ff_find_result_t r = ff_find_tick(&f, FF_FIND_SESSION_MAX_MS - 1u);
    TEST_ASSERT_EQUAL_INT(FF_FIND_INTENT_SEND_PING, r.intent);
    TEST_ASSERT_TRUE(f.active);
}

/* ------------------------------------------------------------------- */
/* Auto-reply's counterpart: ff_find_on_pong records our reading of THEM */
/* ------------------------------------------------------------------- */

static void S29_on_pong_ignored_when_inactive(void)
{
    ff_find_t f;
    memset(&f, 0, sizeof(f));
    ff_find_haptic_t h = ff_find_on_pong(&f, 42u, 0u, -50, true, -3.5f, 1000u);
    TEST_ASSERT_EQUAL_INT(FF_FIND_HAPTIC_NONE, h);
    TEST_ASSERT_FALSE(f.has_their_reading);
}

static void S29_on_pong_ignored_from_wrong_node(void)
{
    ff_find_t f;
    ff_find_start(&f, 42u, 0u);
    ff_find_tick(&f, 0u); /* nonce 0 sent */
    ff_find_on_pong(&f, 999u, 0u, -50, true, -3.5f, 500u);
    TEST_ASSERT_FALSE(f.has_their_reading);
}

static void S29_on_pong_ignored_on_nonce_mismatch(void)
{
    ff_find_t f;
    ff_find_start(&f, 42u, 0u);
    ff_find_tick(&f, 0u); /* nonce 0 sent */
    ff_find_on_pong(&f, 42u, 123u /* wrong nonce */, -50, true, -3.5f, 500u);
    TEST_ASSERT_FALSE(f.has_their_reading);
}

static void S29_on_pong_records_reading_on_match(void)
{
    ff_find_t f;
    ff_find_start(&f, 42u, 0u);
    uint32_t nonce = ff_find_tick(&f, 0u).nonce;

    ff_find_on_pong(&f, 42u, nonce, -62, true, -4.5f, 700u);
    TEST_ASSERT_TRUE(f.has_their_reading);
    TEST_ASSERT_EQUAL_INT16(-62, f.their_rssi_of_us);
    TEST_ASSERT_TRUE(f.their_has_snr);
    TEST_ASSERT_EQUAL_FLOAT(-4.5f, f.their_snr_of_us);
    TEST_ASSERT_EQUAL_UINT32(700u, f.their_reading_age_ms);
}

static void S29_on_pong_absent_snr_never_leaks_a_fabricated_reading(void)
{
    ff_find_t f;
    ff_find_start(&f, 42u, 0u);
    uint32_t nonce = ff_find_tick(&f, 0u).nonce;
    ff_find_on_pong(&f, 42u, nonce, -62, false, 999.0f, 700u);
    TEST_ASSERT_FALSE(f.their_has_snr);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, f.their_snr_of_us);
}

/* ------------------------------------------------------------------- */
/* Trend-haptic trigger (docs/specs/S29-radio-only.md's own Tests §4)   */
/* ------------------------------------------------------------------- */

/* Helper: feed `n` PONG-driven rssi_dbm samples in sequence (one per
 * simulated 10s tick), returning the haptic verdict of the LAST one. */
static ff_find_haptic_t feed_samples(ff_find_t *f, int16_t const *samples, int n)
{
    ff_find_haptic_t last = FF_FIND_HAPTIC_NONE;
    uint32_t t = 0u;
    for (int i = 0; i < n; i++) {
        uint32_t nonce = ff_find_tick(f, t).nonce;
        last = ff_find_on_pong(f, f->target_node_id, nonce, samples[i], false, 0.0f, t + 1u);
        t += FF_FIND_PING_INTERVAL_MS;
    }
    return last;
}

static void S29_no_haptic_below_6_samples(void)
{
    ff_find_t f;
    ff_find_start(&f, 42u, 0u);
    /* Wildly improving, but only 5 samples — one short of the 6 (2x3)
     * this module requires for a real 3-vs-3 comparison. */
    int16_t samples[] = {-100, -95, -90, -85, -80};
    ff_find_haptic_t h = feed_samples(&f, samples, 5);
    TEST_ASSERT_EQUAL_INT(FF_FIND_HAPTIC_NONE, h);
}

static void S29_warmer_fires_on_a_real_improving_crossing(void)
{
    ff_find_t f;
    ff_find_start(&f, 42u, 0u);
    /* Older 3: -100,-99,-101 (avg -100). Newer 3: -90,-89,-91 (avg -90).
     * delta = +10 dB, well past the 3 dB threshold. */
    int16_t samples[] = {-100, -99, -101, -90, -89, -91};
    ff_find_haptic_t h = feed_samples(&f, samples, 6);
    TEST_ASSERT_EQUAL_INT(FF_FIND_HAPTIC_WARMER, h);
}

static void S29_colder_fires_on_a_real_worsening_crossing(void)
{
    ff_find_t f;
    ff_find_start(&f, 42u, 0u);
    int16_t samples[] = {-70, -69, -71, -85, -86, -84}; /* delta ~ -15 dB */
    ff_find_haptic_t h = feed_samples(&f, samples, 6);
    TEST_ASSERT_EQUAL_INT(FF_FIND_HAPTIC_COLDER, h);
}

static void S29_under_3db_change_fires_neither(void)
{
    ff_find_t f;
    ff_find_start(&f, 42u, 0u);
    int16_t samples[] = {-80, -79, -81, -79, -80, -81}; /* avg -80 vs avg -80: 0 dB */
    ff_find_haptic_t h = feed_samples(&f, samples, 6);
    TEST_ASSERT_EQUAL_INT(FF_FIND_HAPTIC_NONE, h);
}

static void S29_warmer_fires_exactly_once_per_crossing_not_once_per_sample(void)
{
    ff_find_t f;
    ff_find_start(&f, 42u, 0u);
    /* 6 samples to first cross into WARMER, then 3 MORE samples that
     * keep the average improving (still well above threshold) — must
     * NOT fire a second time while the trend stays the same direction. */
    int16_t warm_up[] = {-100, -99, -101, -90, -89, -91};
    TEST_ASSERT_EQUAL_INT(FF_FIND_HAPTIC_WARMER, feed_samples(&f, warm_up, 6));

    uint32_t t = 6u * FF_FIND_PING_INTERVAL_MS;
    int16_t more_warm[] = {-88, -87, -86};
    for (int i = 0; i < 3; i++) {
        uint32_t nonce = ff_find_tick(&f, t).nonce;
        ff_find_haptic_t h = ff_find_on_pong(&f, 42u, nonce, more_warm[i], false, 0.0f, t + 1u);
        TEST_ASSERT_EQUAL_INT_MESSAGE(FF_FIND_HAPTIC_NONE, h, "re-fired WARMER without a fresh crossing");
        t += FF_FIND_PING_INTERVAL_MS;
    }
}

static void S29_warmer_can_fire_again_after_returning_to_steady(void)
{
    ff_find_t f;
    ff_find_start(&f, 42u, 0u);
    int16_t warm_up[] = {-100, -99, -101, -90, -89, -91};
    TEST_ASSERT_EQUAL_INT(FF_FIND_HAPTIC_WARMER, feed_samples(&f, warm_up, 6));

    /* Flatten out: three more samples right around -90 bring the
     * window back to ~0 delta (steady) — resets the crossing latch. */
    uint32_t t = 6u * FF_FIND_PING_INTERVAL_MS;
    int16_t steady[] = {-90, -90, -90};
    ff_find_haptic_t h = FF_FIND_HAPTIC_NONE;
    for (int i = 0; i < 3; i++) {
        uint32_t nonce = ff_find_tick(&f, t).nonce;
        h = ff_find_on_pong(&f, 42u, nonce, steady[i], false, 0.0f, t + 1u);
        t += FF_FIND_PING_INTERVAL_MS;
    }
    TEST_ASSERT_EQUAL_INT(FF_FIND_HAPTIC_NONE, h);

    /* Now improve again by a real margin: fires WARMER a second time —
     * on the FIRST sample of this run that crosses back past threshold
     * (not necessarily the last, since once re-latched at +1 the
     * following samples correctly fire NONE again — "once per
     * crossing", pinned by the OTHER test above; this test only cares
     * that the crossing happened again at all, not which exact sample
     * carried it). */
    int16_t warm_again[] = {-80, -79, -78};
    int warmer_fires = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t nonce = ff_find_tick(&f, t).nonce;
        h = ff_find_on_pong(&f, 42u, nonce, warm_again[i], false, 0.0f, t + 1u);
        if (h == FF_FIND_HAPTIC_WARMER) {
            warmer_fires++;
        }
        t += FF_FIND_PING_INTERVAL_MS;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, warmer_fires, "expected exactly one WARMER re-fire after the steady dip");
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S29_start_arms_session_but_sends_nothing_yet);
    RUN_TEST(S29_first_tick_sends_immediately_no_10s_wait);
    RUN_TEST(S29_stop_clears_active);
    RUN_TEST(S29_leave_face_cancels_like_stop);
    RUN_TEST(S29_start_on_new_target_cancels_prior_session_and_its_history);

    RUN_TEST(S29_tick_faster_than_10s_never_sends_twice_in_the_window);
    RUN_TEST(S29_tick_at_exactly_10s_sends_again);
    RUN_TEST(S29_each_sent_ping_gets_a_distinct_nonce);

    RUN_TEST(S29_stops_after_30_pings);
    RUN_TEST(S29_stops_after_5_minutes_even_with_pings_remaining);
    RUN_TEST(S29_does_not_stop_one_ms_before_5_minutes);

    RUN_TEST(S29_on_pong_ignored_when_inactive);
    RUN_TEST(S29_on_pong_ignored_from_wrong_node);
    RUN_TEST(S29_on_pong_ignored_on_nonce_mismatch);
    RUN_TEST(S29_on_pong_records_reading_on_match);
    RUN_TEST(S29_on_pong_absent_snr_never_leaks_a_fabricated_reading);

    RUN_TEST(S29_no_haptic_below_6_samples);
    RUN_TEST(S29_warmer_fires_on_a_real_improving_crossing);
    RUN_TEST(S29_colder_fires_on_a_real_worsening_crossing);
    RUN_TEST(S29_under_3db_change_fires_neither);
    RUN_TEST(S29_warmer_fires_exactly_once_per_crossing_not_once_per_sample);
    RUN_TEST(S29_warmer_can_fire_again_after_returning_to_steady);

    return UNITY_END();
}
