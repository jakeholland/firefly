/**
 * test_now_layout.c — assertion-level coverage for now_layout.c's pure
 * format/geometry helpers (S07 slice b). Same rationale as
 * app/screens/tests/test_radar_layout.c's top comment: "goldens are
 * pixel-diffs against themselves... the test must be assertion-level on
 * the resolver's output", not just on a PNG a human has to eyeball.
 *
 * Named S07_AC6_* — continuing the merged engine's (slice a) AC numbering
 * (S07_AC1..AC5, firmware/festpack/tests/test_sched.c) under the spec's
 * AC6 ("Goldens: now_live.json and now_tbd.json match"): these are the
 * assertion-level checks backing that same behavior, per this task's
 * "goldens are pixel-diffs against themselves, so also assert
 * geometry/format in unit tests" instruction.
 */
#include "unity.h"

#include "now_layout.h"

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------------------
 * now_layout_format_countdown — the spec's literal "IN N MIN" text.
 * ------------------------------------------------------------------- */

static void S07_AC6_countdown_format_matches_spec_example(void)
{
    /* docs/specs/S07-now-face.md's Behavior section literally writes
     * "IN N MIN" with the worked example "IN 33 MIN" (also the S07 slice
     * b brief's own example) — this is the exact repro. */
    char buf[16];
    now_layout_format_countdown(33, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("IN 33 MIN", buf);
}

static void S07_AC6_countdown_format_zero(void)
{
    char buf[16];
    now_layout_format_countdown(0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("IN 0 MIN", buf);
}

static void S07_AC6_countdown_format_clamps_negative_to_zero(void)
{
    /* ff_next_t's mins_until is documented > 0 for real engine output,
     * but this is fixture/render-facing — a bad fixture must not print a
     * confusing negative countdown. */
    char buf[16];
    now_layout_format_countdown(-5, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("IN 0 MIN", buf);
}

static void S07_AC6_countdown_format_single_digit(void)
{
    char buf[16];
    now_layout_format_countdown(1, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("IN 1 MIN", buf);
}

/* PR #21 code review finding #6: the 59/60-minute boundary specifically.
 * now_layout_format_countdown has no hour-conversion branch — it's a flat
 * "IN %d MIN" print for every value — so 59 and 60 aren't actually
 * different code paths from 33 today, but the boundary is asserted
 * explicitly anyway: it's the exact pair a FUTURE hour-conversion feature
 * (e.g. "IN 1 HR 5 MIN") would need to get right, and a test written
 * before that feature exists is the one most likely to survive being
 * forgotten when it lands. */
static void S07_AC6_countdown_format_59_minutes(void)
{
    char buf[16];
    now_layout_format_countdown(59, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("IN 59 MIN", buf);
}

static void S07_AC6_countdown_format_60_minutes(void)
{
    char buf[16];
    now_layout_format_countdown(60, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("IN 60 MIN", buf);
}

static void S07_AC6_countdown_format_null_out_is_a_noop(void)
{
    /* Must not crash — same defensive-NULL convention as
     * radar_layout.c's resolvers (`if (!out) return;`). */
    now_layout_format_countdown(10, NULL, 16);
    TEST_PASS();
}

/* ---------------------------------------------------------------------
 * now_layout_bar_fill_px — progress-bar pixel width from pct_done.
 * ------------------------------------------------------------------- */

static void S07_AC6_bar_fill_zero_pct_is_zero_width(void)
{
    TEST_ASSERT_EQUAL_INT32(0, now_layout_bar_fill_px(0, 200));
}

static void S07_AC6_bar_fill_hundred_pct_is_full_width(void)
{
    TEST_ASSERT_EQUAL_INT32(200, now_layout_bar_fill_px(100, 200));
}

static void S07_AC6_bar_fill_midpoint(void)
{
    /* Exact: 50% of 210 (NOW_LAYOUT_ROW_BAR_TRACK_W_PX) is 105 — no
     * rounding ambiguity to paper over. */
    TEST_ASSERT_EQUAL_INT32(105, now_layout_bar_fill_px(50, 210));
}

static void S07_AC6_bar_fill_clamps_over_100_pct(void)
{
    /* ff_now_row_t documents pct_done as already 0-100, but this guards
     * a malformed/mutated caller the same way ff_theme_crew_color's
     * modulo wrap guards an out-of-range color_idx — "honest but safe,
     * never a crash or an overflowed bar over a display nicety". */
    TEST_ASSERT_EQUAL_INT32(200, now_layout_bar_fill_px(255, 200));
}

static void S07_AC6_bar_fill_clamps_negative_track_width(void)
{
    TEST_ASSERT_EQUAL_INT32(0, now_layout_bar_fill_px(50, -10));
}

/* ---------------------------------------------------------------------
 * now_layout_chord_half_width_px — round-screen keep-out primitive.
 * ------------------------------------------------------------------- */

static void S07_AC6_chord_half_width_at_center_equals_radius(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 220.0f, now_layout_chord_half_width_px(0.0f));
}

static void S07_AC6_chord_half_width_at_top_edge_is_zero(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, now_layout_chord_half_width_px(-220.0f));
}

static void S07_AC6_chord_half_width_at_bottom_edge_is_zero(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, now_layout_chord_half_width_px(220.0f));
}

static void S07_AC6_chord_half_width_beyond_radius_clamps_to_zero(void)
{
    /* Would be sqrt(negative) without the clamp — must not be NaN. */
    float w = now_layout_chord_half_width_px(300.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, w);
    TEST_ASSERT_FALSE(w != w); /* NaN check: NaN != NaN is the only way this is ever true */
}

static void S07_AC6_chord_half_width_known_pythagorean_value(void)
{
    /* 132-176-220 is a 3-4-5 triple scaled by 44: 132^2 + 176^2 = 220^2
     * (17424 + 30976 = 48400) — an exact expected value, not a
     * rounded/approximate one. */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 176.0f, now_layout_chord_half_width_px(132.0f));
}

static void S07_AC6_chord_half_width_symmetric_above_and_below_center(void)
{
    TEST_ASSERT_EQUAL_FLOAT(now_layout_chord_half_width_px(90.0f), now_layout_chord_half_width_px(-90.0f));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S07_AC6_countdown_format_matches_spec_example);
    RUN_TEST(S07_AC6_countdown_format_zero);
    RUN_TEST(S07_AC6_countdown_format_clamps_negative_to_zero);
    RUN_TEST(S07_AC6_countdown_format_single_digit);
    RUN_TEST(S07_AC6_countdown_format_59_minutes);
    RUN_TEST(S07_AC6_countdown_format_60_minutes);
    RUN_TEST(S07_AC6_countdown_format_null_out_is_a_noop);

    RUN_TEST(S07_AC6_bar_fill_zero_pct_is_zero_width);
    RUN_TEST(S07_AC6_bar_fill_hundred_pct_is_full_width);
    RUN_TEST(S07_AC6_bar_fill_midpoint);
    RUN_TEST(S07_AC6_bar_fill_clamps_over_100_pct);
    RUN_TEST(S07_AC6_bar_fill_clamps_negative_track_width);

    RUN_TEST(S07_AC6_chord_half_width_at_center_equals_radius);
    RUN_TEST(S07_AC6_chord_half_width_at_top_edge_is_zero);
    RUN_TEST(S07_AC6_chord_half_width_at_bottom_edge_is_zero);
    RUN_TEST(S07_AC6_chord_half_width_beyond_radius_clamps_to_zero);
    RUN_TEST(S07_AC6_chord_half_width_known_pythagorean_value);
    RUN_TEST(S07_AC6_chord_half_width_symmetric_above_and_below_center);

    return UNITY_END();
}
