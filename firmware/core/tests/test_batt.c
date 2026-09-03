/**
 * test_batt.c — unit tests for `ff_batt` (S25 slice c:
 * docs/specs/S25-power-latch.md "(c) Battery gauge").
 *
 * THE PROXY, stated up front (AGENTS.md standing brief, item 6): a
 * filter that just returns the RAW `ff_batt_pct_from_mv` value every
 * push would pass a naive "first sample shows immediately" test and a
 * naive "a big move updates the display" test equally well — the
 * property that actually matters is that a SMALL move (<2%) does NOT
 * reach the display while a move that CROSSES `FF_BATT_LOW_PCT` always
 * does, however small. Every hysteresis test below therefore drives a
 * SEQUENCE of pushes and asserts the return value at EACH step (not
 * just the final one), so a filter with no hysteresis at all, or one
 * that also delays the low-battery crossing, fails visibly rather than
 * by coincidence.
 */
#include <string.h>

#include "unity.h"

#include "ff_batt.h"
#include "ff_radar.h" /* FF_BATT_LOW_PCT — the boundary the crossing tests exercise */

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* ff_batt_pct_from_mv — the OCV/SOC table                              */
/* ------------------------------------------------------------------- */

static void unknown_when_zero(void)
{
    TEST_ASSERT_EQUAL_INT8(-1, ff_batt_pct_from_mv(0));
}

static void unknown_below_plausibility_window(void)
{
    /* 2400 mV: below FF_BATT_MV_PLAUSIBLE_MIN (2500) — a broken/absent
     * sense line, not a battery at some extreme charge. */
    TEST_ASSERT_EQUAL_INT8(-1, ff_batt_pct_from_mv(2400));
}

static void unknown_above_plausibility_window(void)
{
    /* 4700 mV: above FF_BATT_MV_PLAUSIBLE_MAX (4600). */
    TEST_ASSERT_EQUAL_INT8(-1, ff_batt_pct_from_mv(4700));
}

/* Mutation (a) target: shift the 3700 mV table point away from 30% and
 * this (and the whole-table test below) fails. */
static void table_point_3700_is_30_pct(void)
{
    TEST_ASSERT_EQUAL_INT8(30, ff_batt_pct_from_mv(3700));
}

static void table_boundaries_by_literal(void)
{
    TEST_ASSERT_EQUAL_INT8(0,   ff_batt_pct_from_mv(3300));
    TEST_ASSERT_EQUAL_INT8(5,   ff_batt_pct_from_mv(3500));
    TEST_ASSERT_EQUAL_INT8(10,  ff_batt_pct_from_mv(3600));
    TEST_ASSERT_EQUAL_INT8(30,  ff_batt_pct_from_mv(3700));
    TEST_ASSERT_EQUAL_INT8(50,  ff_batt_pct_from_mv(3800));
    TEST_ASSERT_EQUAL_INT8(65,  ff_batt_pct_from_mv(3900));
    TEST_ASSERT_EQUAL_INT8(80,  ff_batt_pct_from_mv(4000));
    TEST_ASSERT_EQUAL_INT8(92,  ff_batt_pct_from_mv(4100));
    TEST_ASSERT_EQUAL_INT8(100, ff_batt_pct_from_mv(4200));
}

/* Exact arithmetic midpoint of the 3700(30%)/3800(50%) segment: 3750 mV
 * is halfway between the two mV endpoints, and (30+50)/2 == 40 exactly
 * — no rounding ambiguity, so this pins the interpolation direction and
 * scale, not just its rounding rule. */
static void interpolation_midpoint(void)
{
    TEST_ASSERT_EQUAL_INT8(40, ff_batt_pct_from_mv(3750));
}

static void clamps_below_table_to_zero(void)
{
    /* 3000 mV: inside the plausibility window, below the table's lowest
     * point (3300) — clamped to empty, not extrapolated negative. */
    TEST_ASSERT_EQUAL_INT8(0, ff_batt_pct_from_mv(3000));
    /* The plausibility floor itself. */
    TEST_ASSERT_EQUAL_INT8(0, ff_batt_pct_from_mv(FF_BATT_MV_PLAUSIBLE_MIN));
}

static void clamps_above_table_to_hundred(void)
{
    /* 4400 mV: inside the plausibility window, above the table's
     * highest point (4200) — clamped to full, not extrapolated past 100. */
    TEST_ASSERT_EQUAL_INT8(100, ff_batt_pct_from_mv(4400));
    /* The plausibility ceiling itself. */
    TEST_ASSERT_EQUAL_INT8(100, ff_batt_pct_from_mv(FF_BATT_MV_PLAUSIBLE_MAX));
}

/* ------------------------------------------------------------------- */
/* ff_batt_filter_t — init                                              */
/* ------------------------------------------------------------------- */


static void filter_init_clears_all_state(void)
{
    ff_batt_filter_t f;
    memset(&f, 0xAA, sizeof(f));
    ff_batt_filter_init(&f);
    TEST_ASSERT_FALSE(f.filled);
    TEST_ASSERT_EQUAL_UINT8(0, f.next);
    TEST_ASSERT_FALSE(f.has_displayed);
    TEST_ASSERT_FALSE(f.has_last_push);
    TEST_ASSERT_EQUAL_INT8(-1, f.displayed_pct);
    TEST_ASSERT_EQUAL_UINT8(0, f.consecutive_bad);
}

static void filter_init_is_null_safe(void)
{
    ff_batt_filter_init(NULL); /* must not crash */
}

static void filter_push_is_null_safe(void)
{
    TEST_ASSERT_EQUAL_INT8(-1, ff_batt_filter_push(NULL, 3800, 0u));
}

/* ------------------------------------------------------------------- */
/* First sample: shows immediately, no warm-up                          */
/* ------------------------------------------------------------------- */

static void first_sample_shows_immediately(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);
    /* mv=3800 -> raw 50%. One call, no prior history — must reflect
     * right away (a pre-filled window's mean equals that one sample),
     * not wait for FF_BATT_FILTER_WINDOW worth of samples. */
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 3800, 0u));
}

static void before_any_push_display_is_unknown(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);
    TEST_ASSERT_FALSE(f.has_displayed);
    TEST_ASSERT_EQUAL_INT8(-1, f.displayed_pct);
}

/* An implausible/zero reading is honestly reported as still-unknown
 * (never a fabricated 0%) and does not fake a "first sample". */
static void unknown_reading_before_any_real_sample_stays_unknown(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);
    TEST_ASSERT_EQUAL_INT8(-1, ff_batt_filter_push(&f, 0, 0u));
    TEST_ASSERT_FALSE(f.has_displayed);
}

/* A single implausible reading AFTER a real one is not folded into the
 * window and does not disturb the currently displayed value (it only
 * counts toward FF_BATT_FILTER_DEAD_AFTER — see the dead-sensor tests
 * below for what happens once that threshold is reached). */
static void unknown_reading_after_a_real_sample_keeps_displayed_value(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 3800, 0u));
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 0, 1000u)); /* broken read */
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 3800, 2000u)); /* still fine */
}

/* Second-ever sample: the pre-filled window means the new reading
 * carries exactly 1/FF_BATT_FILTER_WINDOW weight in the mV mean, not a
 * 50/50 blend — mv=3800(50%) then mv=3900(65%) means (3800*3+3900)/4 =
 * 3825 mV, which converts to 54% (computed by ff_batt_pct_from_mv, not
 * re-derived here): a 4-point move that clears FF_BATT_HYSTERESIS_PCT,
 * so it must reach the display. */
static void second_sample_partially_blends_by_one_window_slot(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 3800, 0u));
    TEST_ASSERT_EQUAL_INT8(54, ff_batt_filter_push(&f, 3900, 1000u));
}

/* ------------------------------------------------------------------- */
/* Mutation (b) target: a 1% wobble must not move the display; a >=2%   */
/* move must. Both start from a pre-filled window (one push already     */
/* fills every slot — see FF_BATT_FILTER_WINDOW's doc comment), so a    */
/* SECOND reading only ever carries 1/FF_BATT_FILTER_WINDOW weight in   */
/* the averaged mV value.                                               */
/* ------------------------------------------------------------------- */

static void one_percent_wobble_does_not_move_display(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 3800, 0u)); /* pre-fills window, reveals 50% */

    /* mv=3814 averaged with the three settled 3800s in the window:
     * (3800*3+3814+2)/4 = 3804 mV (round-to-nearest), which converts
     * to 51% — a GENUINE 1-point move from the displayed 50%, not
     * rounded away to 0 by the mV-domain averaging (the proxy-check
     * trap: an mv chosen too close to 3800 would land back on exactly
     * 50% after averaging regardless of whether hysteresis exists at
     * all, which would make this test pass even with hysteresis fully
     * removed — mutation (b) is what this literal mv value guards
     * against). Under FF_BATT_HYSTERESIS_PCT (2), a 1-point move must
     * be absorbed. */
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 3814, 1000u));
}

static void two_percent_move_does_move_display(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 3800, 0u)); /* pre-fills window, reveals 50% */
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 3814, 1000u)); /* wobble, absorbed (see above) */

    /* A genuine, sustained reading: mv=3900 (table-exact 65%) averaged
     * with the window's remaining two 3800s/one 3814 gives 3827 mV ->
     * 54%, a 4-point move that clears FF_BATT_HYSTERESIS_PCT (2). */
    TEST_ASSERT_EQUAL_INT8(54, ff_batt_filter_push(&f, 3900, 2000u));
}

/* ------------------------------------------------------------------- */
/* PR #180 review, blocking finding #1: the OLD moving-median design    */
/* flipped its majority every sample under a perfectly alternating      */
/* input (the load-sag pattern a radio TX duty cycle actually produces) */
/* — 29/29 displayed changes on a 3720/3650 mV, 2 s-interval alternation */
/* over 60 s. These two tests run that EXACT scenario (and the low-band */
/* companion) against the real ff_batt_filter_push in a loop and COUNT  */
/* the displayed transitions, rather than asserting a hand-derived      */
/* number — the acceptance test the review itself specified.            */
/* ------------------------------------------------------------------- */

/* Reviewer's literal acceptance test #1: alternating 3720 mV / 3650 mV
 * every 2 s for 60 s (30 pushes) must produce <= 2 displayed changes
 * AFTER the first reveal. */
static void S180_alternating_3720_3650_settles_within_two_changes(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);

    int8_t prev = -2; /* sentinel distinct from any real -1..100 value */
    int changes_after_reveal = 0;
    for (int i = 0; i < 30; i++) {
        uint16_t const mv = (i % 2 == 0) ? 3720u : 3650u;
        uint32_t const now_ms = (uint32_t)i * 2000u;
        int8_t const d = ff_batt_filter_push(&f, mv, now_ms);
        if (i > 0 && d != prev) {
            changes_after_reveal++;
        }
        prev = d;
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(2, changes_after_reveal,
        "3720/3650 mV alternation produced more than 2 displayed changes after the reveal - "
        "the moving-average window is not damping alternating input (mutation-(a)-adjacent: "
        "a median-shaped or ungrown filter would fail this exact scenario)");
    /* Pinned exact trace (not just the bound above) so a future change
     * to the averaging/hysteresis math is caught even if it happens to
     * stay under the <=2 budget by coincidence: reveal at 34%, one step
     * to 31%, settles at 27% forever. */
    ff_batt_filter_t f2;
    ff_batt_filter_init(&f2);
    TEST_ASSERT_EQUAL_INT8(34, ff_batt_filter_push(&f2, 3720, 0u));
    TEST_ASSERT_EQUAL_INT8(31, ff_batt_filter_push(&f2, 3650, 2000u));
    TEST_ASSERT_EQUAL_INT8(31, ff_batt_filter_push(&f2, 3720, 4000u));
    TEST_ASSERT_EQUAL_INT8(27, ff_batt_filter_push(&f2, 3650, 6000u));
    for (int i = 4; i < 30; i++) {
        uint16_t const mv = (i % 2 == 0) ? 3720u : 3650u;
        TEST_ASSERT_EQUAL_INT8(27, ff_batt_filter_push(&f2, mv, (uint32_t)i * 2000u));
    }
}

/* Reviewer's literal acceptance test #2: alternating 3625 mV (15%,
 * FF_BATT_LOW_PCT exactly) / 3630 mV (16%) every 2 s for 60 s must
 * settle at <=15% and never leave the low band during the run (<=1
 * change) — the asymmetric exit margin (mutation (c)-adjacent: a
 * symmetric exemption would strobe the amber alert here). */
static void S180_alternating_3625_3630_settles_at_or_below_low_and_stays(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);

    int8_t prev = -2;
    int changes_after_reveal = 0;
    for (int i = 0; i < 30; i++) {
        uint16_t const mv = (i % 2 == 0) ? 3625u : 3630u;
        uint32_t const now_ms = (uint32_t)i * 2000u;
        int8_t const d = ff_batt_filter_push(&f, mv, now_ms);
        TEST_ASSERT_TRUE_MESSAGE(ff_radar_batt_is_low(d) || d < 0,
            "the display left the low band during a 3625/3630 mV alternation - "
            "the upward-exit margin is not holding");
        if (i > 0 && d != prev) {
            changes_after_reveal++;
        }
        prev = d;
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT(1, changes_after_reveal);

    /* Pinned exact trace: reveal at 15%, then holds at 15% for the
     * whole run (every subsequent push either stays at the exact low
     * boundary or would only rise to 16%, which never clears the
     * FF_BATT_LOW_PCT + FF_BATT_HYSTERESIS_PCT = 17 exit margin). */
    ff_batt_filter_t f2;
    ff_batt_filter_init(&f2);
    for (int i = 0; i < 30; i++) {
        uint16_t const mv = (i % 2 == 0) ? 3625u : 3630u;
        TEST_ASSERT_EQUAL_INT8(15, ff_batt_filter_push(&f2, mv, (uint32_t)i * 2000u));
    }

    /* Reverse phase (16% first): reveals not-low, then the FIRST
     * downward push already crosses (three 16%s + one 15% still means
     * < 15.5 mV of headroom in this particular pair — see the
     * asymmetric-crossing test below for a slower, multi-step version
     * of the same property), and holds at 16% for the rest of the run
     * once the alternation's own mean settles there instead — a
     * different, but equally single-valued, steady state. */
    ff_batt_filter_t f3;
    ff_batt_filter_init(&f3);
    int8_t prev3 = -2;
    int changes3 = 0;
    for (int i = 0; i < 30; i++) {
        uint16_t const mv = (i % 2 == 0) ? 3630u : 3625u;
        int8_t const d = ff_batt_filter_push(&f3, mv, (uint32_t)i * 2000u);
        if (i > 0 && d != prev3) changes3++;
        prev3 = d;
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT(1, changes3);
}

/* ------------------------------------------------------------------- */
/* PR #180 review, blocking finding #2: the low-threshold exemption is  */
/* asymmetric — downward crossing is always prompt (even a 1-point      */
/* move), upward exit needs to clear FF_BATT_LOW_PCT + FF_BATT_         */
/* HYSTERESIS_PCT (17) before the display is allowed to leave the low   */
/* band. This test demonstrates each half of that rule as its own       */
/* clean, non-alternating step sequence (the two tests above prove the  */
/* SAME property under adversarial alternating input; this one is the   */
/* readable, one-property-at-a-time version).                           */
/* ------------------------------------------------------------------- */

static void crossing_to_low_shows_promptly_despite_small_delta(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);

    /* mv=3630 -> raw 16% (just ABOVE FF_BATT_LOW_PCT=15, not low). One
     * push pre-fills the whole window. */
    TEST_ASSERT_EQUAL_INT8(16, ff_batt_filter_push(&f, 3630, 0u));
    TEST_ASSERT_FALSE(ff_radar_batt_is_low(16));

    /* mv=3625 -> raw 15% (exactly FF_BATT_LOW_PCT). Each push only
     * overwrites one of the four window slots, so the mean crosses
     * into the low band gradually — the point being pinned is that the
     * push where it FIRST crosses (16 -> 15, a 1-point move, under
     * FF_BATT_HYSTERESIS_PCT of 2) still shows immediately, with no
     * extra delay beyond that. */
    TEST_ASSERT_EQUAL_INT8(16, ff_batt_filter_push(&f, 3625, 1000u)); /* 1 of 4 slots: still 16 */
    TEST_ASSERT_EQUAL_INT8(16, ff_batt_filter_push(&f, 3625, 2000u)); /* 2 of 4 slots: still 16 */
    TEST_ASSERT_EQUAL_INT8(15, ff_batt_filter_push(&f, 3625, 3000u)); /* 3 of 4 slots: crosses, shows promptly */
    TEST_ASSERT_EQUAL_INT8(15, ff_batt_filter_push(&f, 3625, 4000u)); /* window fully settled at 15 */
}

static void crossing_out_of_low_requires_the_full_exit_margin(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);
    TEST_ASSERT_EQUAL_INT8(15, ff_batt_filter_push(&f, 3625, 0u)); /* pre-fill, reveal at the low boundary */

    /* mv=3650 (20% in isolation) pushed one slot at a time. First push:
     * mean (3650+3625*3)/4 rounds to 3631 mV -> 16% — NOT low by the
     * plain FF_BATT_LOW_PCT test, but it has NOT cleared FF_BATT_LOW_PCT
     * (15) + FF_BATT_HYSTERESIS_PCT (2) = 17, so the display must stay
     * at 15 (unlike the ordinary hysteresis rule, which would have let
     * a mere 1-point move like 15->16 straight through with no floor
     * at all). */
    TEST_ASSERT_EQUAL_INT8(15, ff_batt_filter_push(&f, 3650, 1000u));
    TEST_ASSERT_FALSE_MESSAGE(ff_radar_batt_is_low(16),
        "sanity: 16% must not itself read as low, or this test proves nothing");

    /* Second push: mean (3650*2+3625*2)/4 rounds to 3638 mV -> 18%,
     * which HAS cleared the 17-point exit margin — the display finally
     * leaves the low band. */
    TEST_ASSERT_EQUAL_INT8(18, ff_batt_filter_push(&f, 3650, 2000u));

    /* Back in ordinary territory (both displayed and filtered not
     * low): the plain +/-2 hysteresis rule applies again, with no
     * further exemption. Third push: mean 3644 mV -> 19%, a 1-point
     * move from the now-displayed 18% — absorbed. */
    TEST_ASSERT_EQUAL_INT8(18, ff_batt_filter_push(&f, 3650, 3000u));
    /* Fourth push: window fully 3650 -> mean 3650 mV -> 20%, a 2-point
     * move from 18% — clears the ordinary threshold, updates. */
    TEST_ASSERT_EQUAL_INT8(20, ff_batt_filter_push(&f, 3650, 4000u));
}

/* ------------------------------------------------------------------- */
/* Stale-gap reset (ff_batt.h's FF_BATT_FILTER_STALE_GAP_MS) — with a   */
/* pre-filled window, the contrast is sharper than "no change happens": */
/* the SAME single post-gap sample dominates completely (full jump)     */
/* after a long gap, but only carries its ordinary 1/FF_BATT_FILTER_    */
/* WINDOW weight (a smaller, diluted jump) after a short one.           */
/* ------------------------------------------------------------------- */

static void long_gap_resets_the_window_instead_of_blending(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 3800, 0u)); /* pre-fill, steady 50% */

    /* A gap of exactly FF_BATT_FILTER_STALE_GAP_MS or more since the
     * last push: the window resets and this ONE post-gap sample (mv
     * 4000 -> 80%) pre-fills the WHOLE window (mean == itself) rather
     * than being diluted 3-to-1 by the stale pre-gap 3800s. */
    TEST_ASSERT_EQUAL_INT8(80, ff_batt_filter_push(&f, 4000, FF_BATT_FILTER_STALE_GAP_MS));
}

static void short_gap_does_not_reset_the_window(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 3800, 0u)); /* pre-fill, steady 50% */

    /* Comfortably under the stale-gap threshold: the new sample (mv
     * 4000 -> 80% in isolation) only overwrites ONE of the four
     * window slots, so the mean is (3800*3+4000)/4 = 3850 mV -> 58%,
     * NOT the undiluted 80% the long-gap test above gets for the exact
     * same reading — the contrast that pins the reset actually firing
     * only on the long gap. */
    TEST_ASSERT_EQUAL_INT8(58, ff_batt_filter_push(&f, 4000, 1000u));
}

/* ------------------------------------------------------------------- */
/* PR #180 review, should-fix #3: a sense line that goes dead (0 or an  */
/* implausible reading, repeatedly) must revert the display to -1       */
/* after FF_BATT_FILTER_DEAD_AFTER consecutive bad samples — a stale    */
/* percent shown as current is the honesty violation, not the -1 it     */
/* reverts to.                                                          */
/* ------------------------------------------------------------------- */

static void dead_sense_line_reverts_to_unknown_after_the_threshold(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);
    TEST_ASSERT_EQUAL_INT8(30, ff_batt_filter_push(&f, 3700, 0u)); /* real reading, pre-fill */

    /* FF_BATT_FILTER_DEAD_AFTER - 1 bad samples: still showing the
     * last real value (a stale-looking number is tolerated only up to
     * the threshold, not indefinitely). */
    for (uint8_t i = 1; i < FF_BATT_FILTER_DEAD_AFTER; i++) {
        TEST_ASSERT_EQUAL_INT8_MESSAGE(30, ff_batt_filter_push(&f, 0, (uint32_t)i * 1000u),
            "reverted to unknown before FF_BATT_FILTER_DEAD_AFTER consecutive bad samples");
    }
    /* The FF_BATT_FILTER_DEAD_AFTER-th consecutive bad sample: honest
     * revert to unknown. */
    TEST_ASSERT_EQUAL_INT8(-1, ff_batt_filter_push(&f, 0, (uint32_t)FF_BATT_FILTER_DEAD_AFTER * 1000u));

    /* A real reading afterward is treated as a fresh first-ever
     * sample: shows immediately, no residual weight from before the
     * sense line died. */
    TEST_ASSERT_EQUAL_INT8(30, ff_batt_filter_push(&f, 3700,
        (uint32_t)(FF_BATT_FILTER_DEAD_AFTER + 1) * 1000u));
}

static void fewer_than_threshold_bad_samples_does_not_revert(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);
    TEST_ASSERT_EQUAL_INT8(30, ff_batt_filter_push(&f, 3700, 0u));

    for (uint8_t i = 1; i < FF_BATT_FILTER_DEAD_AFTER; i++) {
        TEST_ASSERT_EQUAL_INT8(30, ff_batt_filter_push(&f, 0, (uint32_t)i * 1000u));
    }
    /* One real reading before hitting the threshold resets the
     * consecutive-bad count — still displaying, never reverted. */
    TEST_ASSERT_EQUAL_INT8(30, ff_batt_filter_push(&f, 3707,
        (uint32_t)FF_BATT_FILTER_DEAD_AFTER * 1000u));
    TEST_ASSERT_TRUE(f.has_displayed);
    TEST_ASSERT_EQUAL_UINT8(0, f.consecutive_bad);
}

/* Mutation target: dropping the dead-sensor revert entirely (e.g.
 * ignoring FF_BATT_FILTER_DEAD_AFTER) would keep returning 30 forever
 * here instead of -1. */
static void mutation_target_dead_revert_is_load_bearing(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);
    (void)ff_batt_filter_push(&f, 3700, 0u);
    (void)ff_batt_filter_push(&f, 0, 1000u);
    (void)ff_batt_filter_push(&f, 0, 2000u);
    int8_t const d = ff_batt_filter_push(&f, 0, 3000u);
    TEST_ASSERT_EQUAL_INT8(-1, d);
    TEST_ASSERT_NOT_EQUAL_INT8(30, d);
}

/* ------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(unknown_when_zero);
    RUN_TEST(unknown_below_plausibility_window);
    RUN_TEST(unknown_above_plausibility_window);
    RUN_TEST(table_point_3700_is_30_pct);
    RUN_TEST(table_boundaries_by_literal);
    RUN_TEST(interpolation_midpoint);
    RUN_TEST(clamps_below_table_to_zero);
    RUN_TEST(clamps_above_table_to_hundred);

    RUN_TEST(filter_init_clears_all_state);
    RUN_TEST(filter_init_is_null_safe);
    RUN_TEST(filter_push_is_null_safe);

    RUN_TEST(first_sample_shows_immediately);
    RUN_TEST(before_any_push_display_is_unknown);
    RUN_TEST(unknown_reading_before_any_real_sample_stays_unknown);
    RUN_TEST(unknown_reading_after_a_real_sample_keeps_displayed_value);
    RUN_TEST(second_sample_partially_blends_by_one_window_slot);

    RUN_TEST(one_percent_wobble_does_not_move_display);
    RUN_TEST(two_percent_move_does_move_display);

    RUN_TEST(S180_alternating_3720_3650_settles_within_two_changes);
    RUN_TEST(S180_alternating_3625_3630_settles_at_or_below_low_and_stays);

    RUN_TEST(crossing_to_low_shows_promptly_despite_small_delta);
    RUN_TEST(crossing_out_of_low_requires_the_full_exit_margin);

    RUN_TEST(long_gap_resets_the_window_instead_of_blending);
    RUN_TEST(short_gap_does_not_reset_the_window);

    RUN_TEST(dead_sense_line_reverts_to_unknown_after_the_threshold);
    RUN_TEST(fewer_than_threshold_bad_samples_does_not_revert);
    RUN_TEST(mutation_target_dead_revert_is_load_bearing);

    return UNITY_END();
}
