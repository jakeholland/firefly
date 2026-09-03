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
    TEST_ASSERT_EQUAL_UINT8(0, f.count);
    TEST_ASSERT_EQUAL_UINT8(0, f.next);
    TEST_ASSERT_FALSE(f.has_displayed);
    TEST_ASSERT_FALSE(f.has_last_push);
    TEST_ASSERT_EQUAL_INT8(-1, f.displayed_pct);
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
     * right away, not wait for a full FF_BATT_FILTER_WINDOW of samples. */
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

/* An implausible reading AFTER a real one is not folded into the
 * history and does not disturb the currently displayed value. */
static void unknown_reading_after_a_real_sample_keeps_displayed_value(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 3800, 0u));
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 0, 1000u)); /* broken read */
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 3800, 2000u)); /* still fine */
}

/* Second-ever sample exercises the even-count (n=2) median branch:
 * average of the two, rounded. mv=3800->50%, mv=3900->65%;
 * median(50,65) rounds to 58, an 8-point move that clears the 2%
 * hysteresis, so it must reach the display. */
static void second_sample_blends_via_two_point_median(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 3800, 0u));
    TEST_ASSERT_EQUAL_INT8(58, ff_batt_filter_push(&f, 3900, 1000u));
}

/* ------------------------------------------------------------------- */
/* Mutation (b) target: a 1% wobble must not move the display; a >=2%   */
/* move must. Both fill a full FF_BATT_FILTER_WINDOW of one steady      */
/* reading first, so the median's own outlier rejection (needing a     */
/* MAJORITY of the window to agree) is part of what's being measured — */
/* not just the raw-vs-displayed comparison in isolation.              */
/* ------------------------------------------------------------------- */

/* Push the same mv FF_BATT_FILTER_WINDOW times so the window is full
 * and the median has settled — every call after the first must keep
 * returning the same value (nothing to smooth, no drift). */
static void fill_window(ff_batt_filter_t *f, uint16_t mv, uint32_t t0)
{
    int8_t const expect = ff_batt_pct_from_mv(mv);
    for (uint8_t i = 0; i < FF_BATT_FILTER_WINDOW; i++) {
        int8_t const got = ff_batt_filter_push(f, mv, t0 + (uint32_t)i * 100u);
        TEST_ASSERT_EQUAL_INT8(expect, got);
    }
}

static void one_percent_wobble_does_not_move_display(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);
    fill_window(&f, 3800, 0u); /* steady 50% */

    /* mv=3807 -> raw 51% (1 point above the settled 50%). Push it three
     * times (enough to become the window's median-dominant value —
     * FF_BATT_FILTER_WINDOW=5, so 3 of 5 is a majority) and the
     * displayed value must stay 50 throughout: the eventual median
     * (51) is only 1 point from what's displayed, under
     * FF_BATT_HYSTERESIS_PCT (2). */
    TEST_ASSERT_EQUAL_INT8(51, ff_batt_pct_from_mv(3807)); /* sanity: interpolated between 3800(50%)/3900(65%) */
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 3807, 1000u));
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 3807, 1100u));
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 3807, 1200u));
}

static void two_percent_move_does_move_display(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);
    fill_window(&f, 3800, 0u); /* steady 50% */

    /* mv=3813 -> raw 52% (2 points above the settled 50% — exactly
     * FF_BATT_HYSTERESIS_PCT). The first two pushes are still a
     * minority in the 5-slot window (median stays 50); the third makes
     * 52 the window's median, which is >= the hysteresis threshold
     * away from the displayed 50, so it must show. */
    TEST_ASSERT_EQUAL_INT8(52, ff_batt_pct_from_mv(3813)); /* sanity */
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 3813, 1000u));
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 3813, 1100u));
    TEST_ASSERT_EQUAL_INT8(52, ff_batt_filter_push(&f, 3813, 1200u));
}

/* ------------------------------------------------------------------- */
/* FF_BATT_LOW_PCT crossing is NEVER masked by hysteresis, even for a  */
/* sub-threshold (<2%) move — the property FF_BATT_LOW_PCT's own       */
/* ff_radar.h doc comment and this header's push() doc comment name    */
/* explicitly.                                                          */
/* ------------------------------------------------------------------- */

static void crossing_to_low_shows_promptly_despite_small_delta(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);

    /* mv=3630 -> raw 16% (just ABOVE FF_BATT_LOW_PCT=15, not low). */
    TEST_ASSERT_EQUAL_INT8(16, ff_batt_pct_from_mv(3630)); /* sanity */
    TEST_ASSERT_FALSE(ff_radar_batt_is_low(16));
    fill_window(&f, 3630, 0u); /* steady 16%, not low */

    /* mv=3625 -> raw 15% (exactly FF_BATT_LOW_PCT — low). Only a
     * 1-point move from the displayed 16%, which FF_BATT_HYSTERESIS_PCT
     * (2) would ordinarily swallow — but 15 IS low and 16 is NOT, so
     * the crossing must show as soon as the median itself reaches 15,
     * with no extra delay beyond that. */
    TEST_ASSERT_EQUAL_INT8(15, ff_batt_pct_from_mv(3625)); /* sanity */
    TEST_ASSERT_TRUE(ff_radar_batt_is_low(15));

    /* Minority so far (1 of 5, then 2 of 5): the median is still 16,
     * nothing crosses yet. */
    TEST_ASSERT_EQUAL_INT8(16, ff_batt_filter_push(&f, 3625, 1000u));
    TEST_ASSERT_EQUAL_INT8(16, ff_batt_filter_push(&f, 3625, 1100u));
    /* Third push: 15 becomes the window's median (3 of 5) — a mere
     * 1-point move below the hysteresis threshold, but it crosses
     * FF_BATT_LOW_PCT, so it must show on THIS push, not be deferred
     * until a 2-point move accumulates. */
    TEST_ASSERT_EQUAL_INT8(15, ff_batt_filter_push(&f, 3625, 1200u));
}

/* Mirror of the above in the other direction: recovering from 15%
 * (low) back to 16% (not low) is the same boundary and gets the same
 * promptness (this module's push() doc comment states the exemption is
 * symmetric on purpose). */
static void crossing_out_of_low_shows_promptly_despite_small_delta(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);
    fill_window(&f, 3625, 0u); /* steady 15%, low */
    TEST_ASSERT_TRUE(ff_radar_batt_is_low(ff_batt_pct_from_mv(3625)));

    TEST_ASSERT_EQUAL_INT8(15, ff_batt_filter_push(&f, 3630, 1000u));
    TEST_ASSERT_EQUAL_INT8(15, ff_batt_filter_push(&f, 3630, 1100u));
    TEST_ASSERT_EQUAL_INT8(16, ff_batt_filter_push(&f, 3630, 1200u));
}

/* ------------------------------------------------------------------- */
/* Stale-gap reset (ff_batt.h's FF_BATT_FILTER_STALE_GAP_MS)            */
/* ------------------------------------------------------------------- */

static void long_gap_resets_the_window_instead_of_blending(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);
    fill_window(&f, 3800, 0u); /* steady 50%, window full of 50s */

    /* A gap of exactly FF_BATT_FILTER_STALE_GAP_MS or more since the
     * last push: the window resets, so this ONE post-gap sample (mv
     * 4000 -> 80%) becomes the whole window (median == itself) rather
     * than being outvoted 4-to-1 by the stale pre-gap 50s. An 80 vs 50
     * displayed is a 30-point move, comfortably over the hysteresis
     * threshold either way, so this pins the reset itself: without it,
     * a single new sample among four old ones keeps the median at 50
     * (see one_percent_wobble/two_percent_move's own "3 of 5" comments)
     * and this assertion would fail. */
    uint32_t const t_gap = (FF_BATT_FILTER_WINDOW - 1u) * 100u + FF_BATT_FILTER_STALE_GAP_MS;
    TEST_ASSERT_EQUAL_INT8(80, ff_batt_filter_push(&f, 4000, t_gap));
}

static void short_gap_does_not_reset_the_window(void)
{
    ff_batt_filter_t f;
    ff_batt_filter_init(&f);
    fill_window(&f, 3800, 0u); /* steady 50% */

    /* Comfortably under the stale-gap threshold: an isolated new
     * sample is still just one of five, outvoted by the four settled
     * 50s, so the median (and thus the display) does not move yet. */
    uint32_t const t_short = (FF_BATT_FILTER_WINDOW - 1u) * 100u + 1000u;
    TEST_ASSERT_EQUAL_INT8(50, ff_batt_filter_push(&f, 4000, t_short));
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
    RUN_TEST(second_sample_blends_via_two_point_median);

    RUN_TEST(one_percent_wobble_does_not_move_display);
    RUN_TEST(two_percent_move_does_move_display);

    RUN_TEST(crossing_to_low_shows_promptly_despite_small_delta);
    RUN_TEST(crossing_out_of_low_shows_promptly_despite_small_delta);

    RUN_TEST(long_gap_resets_the_window_instead_of_blending);
    RUN_TEST(short_gap_does_not_reset_the_window);

    return UNITY_END();
}
