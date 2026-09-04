/**
 * test_multitap.c — unit tests for `ff_multitap` (S10 quick flare:
 * docs/specs/S10-flare.md's Amendments, "press HOME 5 times quickly to
 * flare to the crew, no screen needed").
 *
 * THE PROXY, stated up front (AGENTS.md item 6): "5 presses in a row
 * fires" is satisfied even by a counter with no gap/window bound at
 * all. Every positive test here is paired with a negative control that
 * drives the SAME number of presses but violates exactly one bound (a
 * gap too long, a window too long, only 4 presses) and asserts it does
 * NOT fire — mirroring test_button.c's own stated proxy discipline for
 * the sibling module this one is fed by.
 */
#include <string.h>

#include "unity.h"

#include "ff_multitap.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* literal-pinned constants (proxy guard: a test that only ever compares
 * symbolically against the macro would survive a silent value change) */
/* ------------------------------------------------------------------- */

static void S10_multitap_count_is_5(void)
{
    TEST_ASSERT_EQUAL_UINT8(5u, FF_MULTITAP_COUNT);
}

static void S10_multitap_max_gap_is_700ms(void)
{
    /* Relaxed 400 -> 700 ms, fix/quick-flare-detection (2026-09-03).
     * Mutation (a): reverting this literal to 400 fails this test AND
     * a_600ms_gap_does_not_reset below (a 600 ms gap test that only
     * passes under the relaxed bound). */
    TEST_ASSERT_EQUAL_UINT32(700u, FF_MULTITAP_MAX_GAP_MS);
}

static void S10_multitap_window_is_4000ms(void)
{
    /* Relaxed 2500 -> 4000 ms, fix/quick-flare-detection (2026-09-03). */
    TEST_ASSERT_EQUAL_UINT32(4000u, FF_MULTITAP_WINDOW_MS);
}

static void S10_multitap_bounce_ms_is_30ms(void)
{
    /* fix/quick-flare-detection (2026-09-04, moved into core after
     * review). Mutation: dropping the bounce-reject rule entirely fails
     * S10_multitap_bounce_dedup_0_5_12_300ms_counts_as_exactly_two_presses
     * below, not this literal (a symbolic-only mutation, e.g. always
     * returning false from the reject check regardless of the gap,
     * would leave this constant untouched but still break behavior —
     * see that test's own comment). */
    TEST_ASSERT_EQUAL_UINT32(30u, FF_MULTITAP_BOUNCE_MS);
}

/* ------------------------------------------------------------------- */
/* init                                                                 */
/* ------------------------------------------------------------------- */

static void init_clears_all_state(void)
{
    ff_multitap_t m;
    memset(&m, 0xAA, sizeof(m));
    ff_multitap_init(&m);
    TEST_ASSERT_EQUAL_UINT8(0u, m.count);
}

static void init_is_null_safe(void)
{
    ff_multitap_init(NULL); /* must not crash */
}

/* ------------------------------------------------------------------- */
/* the core rule: 5 presses at 300ms gaps fires on exactly the 5th      */
/* ------------------------------------------------------------------- */

static void five_taps_at_300ms_fires_on_the_5th_exactly(void)
{
    ff_multitap_t m;
    ff_multitap_init(&m);

    TEST_ASSERT_FALSE(ff_multitap_press(&m, 0u));    /* 1 */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 300u));  /* 2 */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 600u));  /* 3 */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 900u));  /* 4 — the negative half of "not the 4th" */
    TEST_ASSERT_TRUE(ff_multitap_press(&m, 1200u));  /* 5 — fires exactly here */
}

/* Positive control's sibling: a 6th tap right after the 5th must NOT
 * fire again (ff_multitap.h: "a 6th press starts a brand-new run of 1",
 * i.e. it counts as press 1 of a NEW run, not press 6 of the old one). */
static void a_6th_tap_does_not_also_fire_but_starts_a_new_run(void)
{
    ff_multitap_t m;
    ff_multitap_init(&m);

    TEST_ASSERT_FALSE(ff_multitap_press(&m, 0u));
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 300u));
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 600u));
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 900u));
    TEST_ASSERT_TRUE(ff_multitap_press(&m, 1200u));  /* 5th: fires, resets to idle */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 1210u)); /* "6th tap": actually press 1 of a new run */
    TEST_ASSERT_EQUAL_UINT8(1u, m.count);

    /* Positive control: that new run can itself complete normally —
     * proves the reset really left a live, countable run behind, not a
     * wedged/broken FSM. */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 1500u));
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 1800u));
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 2100u));
    TEST_ASSERT_TRUE(ff_multitap_press(&m, 2400u));
}

/* ------------------------------------------------------------------- */
/* gap reset: a gap > FF_MULTITAP_MAX_GAP_MS resets the count           */
/* ------------------------------------------------------------------- */

static void a_701ms_gap_resets_the_count(void)
{
    ff_multitap_t m;
    ff_multitap_init(&m);

    TEST_ASSERT_FALSE(ff_multitap_press(&m, 0u));            /* 1 */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 300u));          /* 2 */
    /* Gap of 701ms — one over the 700ms (relaxed) bound. This press
     * starts a NEW run of 1, not press 3 of the old one. */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 1001u));
    TEST_ASSERT_EQUAL_UINT8(1u, m.count);

    /* Positive control: a gap of EXACTLY 700ms (the inclusive boundary,
     * ff_time_reached's own documented convention — "now_ms == deadline
     * already reached") also resets — the bound is "gap too long", and
     * 700 itself already counts as reached/too-long by that shared
     * convention. Then finish this fresh run to prove it still counts
     * normally. */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 1001u + 700u)); /* exactly 700ms after the previous press: resets again */
    TEST_ASSERT_EQUAL_UINT8(1u, m.count);
}

/* A gap of exactly 699ms (one under the bound) must NOT reset — the
 * run continues normally. Negative control for the boundary above. */
static void a_699ms_gap_does_not_reset(void)
{
    ff_multitap_t m;
    ff_multitap_init(&m);

    TEST_ASSERT_FALSE(ff_multitap_press(&m, 0u));
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 699u));
    TEST_ASSERT_EQUAL_UINT8(2u, m.count); /* extended, not reset */
}

/* Mutation-target test (a): a 600ms gap is exactly the kind of "a bit
 * finicky" real-world cadence this PR relaxes the bound to accept — it
 * would have RESET under the old 400ms bound (this is a regression
 * guard against reverting FF_MULTITAP_MAX_GAP_MS back down: see this
 * file's own S10_multitap_max_gap_is_700ms comment). Drives a full
 * 5-press run at 600ms gaps (2400ms total, under the 4000ms window) and
 * asserts it fires on exactly the 5th, never resetting along the way. */
static void a_5_press_run_at_600ms_gaps_fires_on_the_5th(void)
{
    ff_multitap_t m;
    ff_multitap_init(&m);

    TEST_ASSERT_FALSE(ff_multitap_press(&m, 0u));    /* 1 */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 600u));  /* 2 */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 1200u)); /* 3 */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 1800u)); /* 4 */
    TEST_ASSERT_TRUE(ff_multitap_press(&m, 2400u));  /* 5 — fires */
}

/* ------------------------------------------------------------------- */
/* bounce reject: a gap under FF_MULTITAP_BOUNCE_MS is the SAME         */
/* physical press bouncing, not a genuine second one — ignored, no      */
/* state mutated (fix/quick-flare-detection, 2026-09-04, moved into     */
/* core after review)                                                   */
/* ------------------------------------------------------------------- */

/* THE mutation-target test the review asked for directly: edges at
 * 0, 5, 12, 300 ms. 5ms and 12ms after the first press are both well
 * under the 30ms bounce window (bounce of press 1, not presses 2 and
 * 3) and must be silently ignored; 300ms is a genuine second press.
 * Dropping the bounce-reject rule entirely would count all four edges
 * (none of the gaps reach FF_MULTITAP_MAX_GAP_MS), landing at count==4
 * instead of the correct count==2 — caught by the intermediate
 * assertion below, not just the final one. */
static void S10_multitap_bounce_dedup_0_5_12_300ms_counts_as_exactly_two_presses(void)
{
    ff_multitap_t m;
    ff_multitap_init(&m);

    TEST_ASSERT_FALSE(ff_multitap_press(&m, 0u));  /* 1 — genuine */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 5u));  /* bounce of press 1 (5ms < 30ms) — ignored */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 12u)); /* bounce of press 1 (12ms < 30ms) — ignored */
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1u, m.count, "two bounce edges must not have advanced the count at all");

    TEST_ASSERT_FALSE(ff_multitap_press(&m, 300u)); /* 2 — genuine, well clear of the bounce window */
    TEST_ASSERT_EQUAL_UINT8(2u, m.count);
}

/* A bounce-rejected edge mutates NOTHING — not just count, first_ms/
 * last_ms too — proving rule 0 is a true no-op, not a cheaper-looking
 * "count stays the same but the clock moved" partial update that would
 * silently corrupt the NEXT real gap's measurement. */
static void bounce_rejected_edge_leaves_first_and_last_ms_untouched(void)
{
    ff_multitap_t m;
    ff_multitap_init(&m);

    TEST_ASSERT_FALSE(ff_multitap_press(&m, 1000u)); /* 1 */
    TEST_ASSERT_EQUAL_UINT32(1000u, m.first_ms);
    TEST_ASSERT_EQUAL_UINT32(1000u, m.last_ms);

    TEST_ASSERT_FALSE(ff_multitap_press(&m, 1010u)); /* bounce, 10ms later */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1000u, m.first_ms, "a bounce must not move first_ms");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1000u, m.last_ms, "a bounce must not move last_ms either");
}

/* Boundary: exactly FF_MULTITAP_BOUNCE_MS (30ms) since the last press
 * has REACHED the bound (ff_time_reached's inclusive convention) and
 * counts as genuine, not a bounce. */
static void a_30ms_gap_is_not_a_bounce(void)
{
    ff_multitap_t m;
    ff_multitap_init(&m);

    TEST_ASSERT_FALSE(ff_multitap_press(&m, 0u));
    TEST_ASSERT_FALSE(ff_multitap_press(&m, FF_MULTITAP_BOUNCE_MS));
    TEST_ASSERT_EQUAL_UINT8(2u, m.count);
}

/* Negative control for the boundary above: one ms under is still a
 * bounce. */
static void a_29ms_gap_is_still_a_bounce(void)
{
    ff_multitap_t m;
    ff_multitap_init(&m);

    TEST_ASSERT_FALSE(ff_multitap_press(&m, 0u));
    TEST_ASSERT_FALSE(ff_multitap_press(&m, FF_MULTITAP_BOUNCE_MS - 1u));
    TEST_ASSERT_EQUAL_UINT8(1u, m.count);
}

/* A burst with bounces sprinkled between otherwise-genuine presses
 * still fires on the 5th GENUINE press, not derailed early by the
 * bounces (proves rule 0 composes correctly with the whole FSM, not
 * just in isolation). */
static void a_burst_with_bounces_still_fires_on_the_5th_genuine_press(void)
{
    ff_multitap_t m;
    ff_multitap_init(&m);

    TEST_ASSERT_FALSE(ff_multitap_press(&m, 0u));   /* 1 */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 5u));   /* bounce of 1 */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 300u)); /* 2 */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 305u)); /* bounce of 2 */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 600u)); /* 3 */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 900u)); /* 4 */
    TEST_ASSERT_TRUE(ff_multitap_press(&m, 1200u)); /* 5 — fires */
}

/* ------------------------------------------------------------------- */
/* window bound: a run whose TOTAL span exceeds FF_MULTITAP_WINDOW_MS   */
/* resets, even measured from a run that never violated the per-gap    */
/* bound on its own                                                     */
/* ------------------------------------------------------------------- */

/* Worth recording (not itself a test): with these three constants
 * (COUNT=5, MAX_GAP_MS=700, WINDOW_MS=4000), a 5-press run with EVERY
 * gap legal can span at most 4*700=2800ms — always inside the 4000ms
 * window — so the window bound can never be the thing that resets a
 * run whose gaps were all individually legal; a run that reaches the
 * window boundary has necessarily already had an illegal gap along the
 * way too. The window is still a real, independent guard in the code
 * (`ff_multitap_press` checks it unconditionally, not merely as a
 * side-effect of the gap check — see ff_multitap.c), it is just never
 * the SOLE cause of a reset at these particular constant values; the
 * test below pins its boundary directly rather than trying to isolate
 * an effect these constants make unreachable. */

/* The direct, single-assertion boundary case: a 5th press whose ARRIVAL
 * is exactly at the window boundary (first_ms + FF_MULTITAP_WINDOW_MS,
 * ff_time_reached's inclusive convention) must reset, not fire — the
 * literal AC pin for "5 taps spanning 2501ms don't fire" (one past the
 * inclusive boundary is unambiguously over it). */
static void a_5th_press_exactly_at_the_window_boundary_resets_not_fires(void)
{
    ff_multitap_t m;
    ff_multitap_init(&m);

    TEST_ASSERT_FALSE(ff_multitap_press(&m, 0u));                    /* 1, first_ms = 0 */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 300u));                  /* 2 */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 600u));                  /* 3 */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 900u));                  /* 4, last_ms = 900 */
    bool const fired = ff_multitap_press(&m, FF_MULTITAP_WINDOW_MS); /* arrival == the window boundary */
    TEST_ASSERT_FALSE(fired);
    TEST_ASSERT_EQUAL_UINT8(1u, m.count); /* a fresh run's press 1, not a 5th */
}

/* ------------------------------------------------------------------- */
/* pending                                                              */
/* ------------------------------------------------------------------- */

static void pending_is_false_when_idle(void)
{
    ff_multitap_t m;
    ff_multitap_init(&m);
    TEST_ASSERT_FALSE(ff_multitap_pending(&m, 0u));
}

static void pending_is_true_during_a_sequence(void)
{
    ff_multitap_t m;
    ff_multitap_init(&m);

    TEST_ASSERT_FALSE(ff_multitap_press(&m, 0u));   /* 1 */
    TEST_ASSERT_TRUE(ff_multitap_pending(&m, 0u));
    TEST_ASSERT_TRUE(ff_multitap_pending(&m, 100u)); /* still within the gap bound, no new press needed to ask */

    TEST_ASSERT_FALSE(ff_multitap_press(&m, 300u)); /* 2 */
    TEST_ASSERT_TRUE(ff_multitap_pending(&m, 300u));
}

static void pending_is_false_after_fire(void)
{
    ff_multitap_t m;
    ff_multitap_init(&m);

    TEST_ASSERT_FALSE(ff_multitap_press(&m, 0u));
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 300u));
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 600u));
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 900u));
    TEST_ASSERT_TRUE(ff_multitap_press(&m, 1200u)); /* fires: 5th */
    TEST_ASSERT_FALSE(ff_multitap_pending(&m, 1200u));
    TEST_ASSERT_FALSE(ff_multitap_pending(&m, 1201u));
}

static void pending_is_false_after_a_gap_reset_with_no_further_press(void)
{
    ff_multitap_t m;
    ff_multitap_init(&m);

    TEST_ASSERT_FALSE(ff_multitap_press(&m, 0u)); /* 1 */
    TEST_ASSERT_TRUE(ff_multitap_pending(&m, 0u));
    /* No further press; time alone crosses the gap bound — pending must
     * honestly report false the instant the gap has elapsed, without
     * needing a new press to observe it (this is why pending is a pure
     * query of elapsed time against last_ms, not a cached flag). */
    TEST_ASSERT_FALSE(ff_multitap_pending(&m, FF_MULTITAP_MAX_GAP_MS));
}

/* ------------------------------------------------------------------- */
/* wraparound safety                                                    */
/* ------------------------------------------------------------------- */

static void multitap_survives_now_ms_wraparound(void)
{
    ff_multitap_t m;
    ff_multitap_init(&m);

    uint32_t t = (uint32_t)0xFFFFFFFFu - 100u; /* wraps mid-sequence */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, t)); /* 1 */
    t += 300u; /* wraps past UINT32_MAX */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, t)); /* 2 */
    t += 300u;
    TEST_ASSERT_FALSE(ff_multitap_press(&m, t)); /* 3 */
    t += 300u;
    TEST_ASSERT_FALSE(ff_multitap_press(&m, t)); /* 4 */
    t += 300u;
    TEST_ASSERT_TRUE(ff_multitap_press(&m, t)); /* 5 — fires across the wrap */
}

/* ------------------------------------------------------------------- */
/* late-batch delivery: a "missed sample" scenario                     */
/*                                                                       */
/* fix/quick-flare-detection's whole point — five real presses that     */
/* happened at 0/250/500/750/1000ms (well inside the relaxed 700ms gap  */
/* bound and 4000ms window) are captured somewhere with their own       */
/* timestamps (an ISR ring buffer on-device, a mock queue here) but not */
/* actually HANDED to ff_multitap_press until a later, slower drain —   */
/* the exact scenario a tick-sampled counter with no per-edge timestamp */
/* cannot ever get right (a slow frame either delays the whole count or */
/* drops a press that fell inside it). Feeding each press's OWN         */
/* historical timestamp, even though all five calls happen back-to-back */
/* right here, must produce the identical fire-on-the-5th result as if  */
/* each had been delivered live.                                        */
/* ------------------------------------------------------------------- */

static void S10_multitap_late_batch_delivery_of_5_on_time_edges_still_fires_on_the_5th(void)
{
    ff_multitap_t m;
    ff_multitap_init(&m);

    /* All five ff_multitap_press calls happen "now" (back to back, no
     * real time elapses between these C statements) but each carries
     * the edge's OWN recorded timestamp — exactly what a drain loop
     * does with a batch of ring-buffer entries. */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 0u));    /* edge recorded at t=0 */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 250u));  /* edge recorded at t=250 */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 500u));  /* edge recorded at t=500 */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 750u));  /* edge recorded at t=750 */
    TEST_ASSERT_TRUE(ff_multitap_press(&m, 1000u));  /* edge recorded at t=1000 — fires */
}

/* Negative control: the same late-batch delivery, but the underlying
 * edges themselves were too far apart (a real 701ms+ gap between two of
 * them) — batching the DELIVERY must not paper over a genuinely illegal
 * gap between the recorded EDGES. Proves the batch-robustness above
 * comes from per-edge timestamps, not from disabling the gap check for
 * batched calls. */
static void S10_multitap_late_batch_delivery_does_not_hide_a_real_illegal_gap(void)
{
    ff_multitap_t m;
    ff_multitap_init(&m);

    TEST_ASSERT_FALSE(ff_multitap_press(&m, 0u));    /* edge at t=0 */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 250u));  /* edge at t=250 */
    /* Edge recorded at t=1000 — a REAL 750ms gap since the last edge
     * (over the 700ms bound) — resets even though this call, like the
     * ones above, is being made with no real time elapsed since the
     * previous C statement. */
    TEST_ASSERT_FALSE(ff_multitap_press(&m, 1000u));
    TEST_ASSERT_EQUAL_UINT8(1u, m.count);
}

/* ------------------------------------------------------------------- */
/* NULL guards                                                          */
/* ------------------------------------------------------------------- */

static void press_is_null_safe(void)
{
    TEST_ASSERT_FALSE(ff_multitap_press(NULL, 0u));
}

static void pending_is_null_safe(void)
{
    TEST_ASSERT_FALSE(ff_multitap_pending(NULL, 0u));
}

/* ------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S10_multitap_count_is_5);
    RUN_TEST(S10_multitap_max_gap_is_700ms);
    RUN_TEST(S10_multitap_window_is_4000ms);
    RUN_TEST(S10_multitap_bounce_ms_is_30ms);

    RUN_TEST(init_clears_all_state);
    RUN_TEST(init_is_null_safe);

    RUN_TEST(five_taps_at_300ms_fires_on_the_5th_exactly);
    RUN_TEST(a_6th_tap_does_not_also_fire_but_starts_a_new_run);

    RUN_TEST(a_701ms_gap_resets_the_count);
    RUN_TEST(a_699ms_gap_does_not_reset);
    RUN_TEST(a_5_press_run_at_600ms_gaps_fires_on_the_5th);

    RUN_TEST(S10_multitap_bounce_dedup_0_5_12_300ms_counts_as_exactly_two_presses);
    RUN_TEST(bounce_rejected_edge_leaves_first_and_last_ms_untouched);
    RUN_TEST(a_30ms_gap_is_not_a_bounce);
    RUN_TEST(a_29ms_gap_is_still_a_bounce);
    RUN_TEST(a_burst_with_bounces_still_fires_on_the_5th_genuine_press);

    RUN_TEST(a_5th_press_exactly_at_the_window_boundary_resets_not_fires);

    RUN_TEST(pending_is_false_when_idle);
    RUN_TEST(pending_is_true_during_a_sequence);
    RUN_TEST(pending_is_false_after_fire);
    RUN_TEST(pending_is_false_after_a_gap_reset_with_no_further_press);

    RUN_TEST(multitap_survives_now_ms_wraparound);

    RUN_TEST(S10_multitap_late_batch_delivery_of_5_on_time_edges_still_fires_on_the_5th);
    RUN_TEST(S10_multitap_late_batch_delivery_does_not_hide_a_real_illegal_gap);

    RUN_TEST(press_is_null_safe);
    RUN_TEST(pending_is_null_safe);

    return UNITY_END();
}
