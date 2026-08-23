/**
 * test_ff_layout.c — assertion-level geometry tests for ff_layout.h.
 *
 * Same rationale as test_radar_layout.c's header comment: this is plain
 * C11 math, so every assertion here is a direct check on real numbers,
 * not a pixel-diff proxy for them. No LVGL, no fixtures — see
 * targets/sim/tests/test_face_hit_targets.c for the black-box sweep that
 * applies this module to every real committed fixture's actual built
 * screen.
 */
#include <math.h>

#include "unity.h"

#include "ff_layout.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* ff_layout_rect_in_circle                                             */
/* ------------------------------------------------------------------- */

static void rect_fully_inside_is_true(void)
{
    ff_layout_rect_t r = {100.0f, 100.0f, 140.0f, 130.0f};
    TEST_ASSERT_TRUE(ff_layout_rect_in_circle(r, 220.0f, 220.0f, 220.0f));
}

static void rect_centered_on_circle_center_is_true(void)
{
    ff_layout_rect_t r = {200.0f, 200.0f, 240.0f, 240.0f};
    TEST_ASSERT_TRUE(ff_layout_rect_in_circle(r, 220.0f, 220.0f, 220.0f));
}

static void rect_with_one_corner_outside_is_false(void)
{
    /* Exactly the Compose back-button shape from the PR #25 UX review:
     * TOP_LEFT(16,10), 44x44, against puck center (220,220) r=220. */
    ff_layout_rect_t r = {16.0f, 10.0f, 60.0f, 54.0f};
    TEST_ASSERT_FALSE(ff_layout_rect_in_circle(r, 220.0f, 220.0f, 220.0f));
}

static void rect_with_all_corners_outside_is_false(void)
{
    ff_layout_rect_t r = {0.0f, 0.0f, 5.0f, 5.0f};
    TEST_ASSERT_FALSE(ff_layout_rect_in_circle(r, 220.0f, 220.0f, 220.0f));
}

static void rect_corner_exactly_on_boundary_is_true(void)
{
    /* A single point at exactly radius distance: 3-4-5 triangle scaled by
     * 44 -> (132, 176), hypotenuse 220 exactly. "exactly on" counts as
     * inside per the header's documented contract (<=, not <). */
    ff_layout_rect_t r = {220.0f - 132.0f, 220.0f - 176.0f, 220.0f, 220.0f};
    TEST_ASSERT_TRUE(ff_layout_rect_in_circle(r, 220.0f, 220.0f, 220.0f));
}

static void rect_corner_one_unit_past_boundary_is_false(void)
{
    /* The exact-boundary rect's touching corner is (x1,y1) = (88,44)
     * (220-132, 220-176) — pushing x1 one unit FARTHER from center
     * (87, not 221: x2 is the near corner at the center itself and
     * moving it doesn't affect the already-touching far corner at all)
     * is what actually puts a corner past the boundary. */
    ff_layout_rect_t r = {220.0f - 132.0f - 1.0f, 220.0f - 176.0f, 220.0f, 220.0f};
    TEST_ASSERT_FALSE(ff_layout_rect_in_circle(r, 220.0f, 220.0f, 220.0f));
}

static void negative_radius_is_always_false(void)
{
    ff_layout_rect_t r = {219.0f, 219.0f, 221.0f, 221.0f}; /* straddles the center either way */
    TEST_ASSERT_FALSE(ff_layout_rect_in_circle(r, 220.0f, 220.0f, -1.0f));
}

static void tiny_rect_at_exact_center_fits_any_nonneg_radius(void)
{
    ff_layout_rect_t r = {220.0f, 220.0f, 220.0f, 220.0f}; /* degenerate, zero-size */
    TEST_ASSERT_TRUE(ff_layout_rect_in_circle(r, 220.0f, 220.0f, 0.0f));
}

/* ------------------------------------------------------------------- */
/* ff_layout_chord_half_width                                           */
/* ------------------------------------------------------------------- */

static void chord_at_center_equals_radius(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 220.0f, ff_layout_chord_half_width(0.0f, 220.0f));
}

static void chord_at_exact_radius_is_zero(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, ff_layout_chord_half_width(220.0f, 220.0f));
}

static void chord_past_radius_is_zero(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, ff_layout_chord_half_width(300.0f, 220.0f));
}

static void chord_is_symmetric_in_dy_sign(void)
{
    float pos = ff_layout_chord_half_width(150.0f, 220.0f);
    float neg = ff_layout_chord_half_width(-150.0f, 220.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, pos, neg);
}

static void chord_matches_pythagorean_worked_example(void)
{
    /* 3-4-5 triangle scaled by 44: dy=176, expect half-width 132 at
     * radius 220 (132^2 + 176^2 = 220^2: 17424 + 30976 = 48400 = 220^2). */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 132.0f, ff_layout_chord_half_width(176.0f, 220.0f));
}

static void chord_negative_radius_is_zero(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, ff_layout_chord_half_width(0.0f, -5.0f));
}

/* A point exactly at the returned chord half-width, at the same dy, must
 * itself be reported as ON the circle (round-trip consistency between the
 * two functions — a layout that sizes itself to `ff_layout_chord_half_width`'s
 * result should never then fail `ff_layout_rect_in_circle`). */
static void chord_half_width_round_trips_through_rect_in_circle(void)
{
    float dy = 130.0f;
    float radius = 220.0f;
    float half_w = ff_layout_chord_half_width(dy, radius);

    ff_layout_rect_t exact = {220.0f - half_w, 220.0f + dy, 220.0f + half_w, 220.0f + dy};
    TEST_ASSERT_TRUE(ff_layout_rect_in_circle(exact, 220.0f, 220.0f, radius));

    ff_layout_rect_t one_wider = {220.0f - half_w - 1.0f, 220.0f + dy, 220.0f + half_w + 1.0f, 220.0f + dy};
    TEST_ASSERT_FALSE(ff_layout_rect_in_circle(one_wider, 220.0f, 220.0f, radius));
}

/* ------------------------------------------------------------------- */
/* ff_layout_safe_margin_x                                              */
/* ------------------------------------------------------------------- */

static void safe_margin_at_center_band_is_near_zero(void)
{
    /* A band straddling the exact center needs (almost) no margin —
     * only `safety_px` worth. */
    float margin = ff_layout_safe_margin_x(210.0f, 20.0f, 220.0f, 220.0f, 10.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 10.0f, margin);
}

static void safe_margin_grows_as_band_moves_toward_the_pole(void)
{
    /* Same band height/safety, three bands progressively closer to the
     * bottom pole (y=440) — margin must be monotonically non-decreasing,
     * which is exactly the "rows near the edge need to be narrower"
     * property the S08 UX-review ruling asked the layout to have. */
    float m_near_center = ff_layout_safe_margin_x(200.0f, 46.0f, 220.0f, 220.0f, 10.0f);
    float m_mid = ff_layout_safe_margin_x(300.0f, 46.0f, 220.0f, 220.0f, 10.0f);
    float m_near_pole = ff_layout_safe_margin_x(380.0f, 46.0f, 220.0f, 220.0f, 10.0f);

    TEST_ASSERT_TRUE(m_near_center <= m_mid);
    TEST_ASSERT_TRUE(m_mid <= m_near_pole);
}

static void safe_margin_matches_compose_back_button_worked_example(void)
{
    /* PR #25 UX review's own numbers: header row at y=56, 44px tall,
     * puck radius 220 (center 220), safety 10px -> margin ~84 (hand
     * computation in the PR's design notes: chord_half_width(164,220)
     * ~=146.6, minus 10 safety = 136.6, margin = 220-136.6 = 83.4). */
    float margin = ff_layout_safe_margin_x(56.0f, 44.0f, 220.0f, 220.0f, 10.0f);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 83.4f, margin);
}

static void safe_margin_is_never_negative(void)
{
    /* A band at the exact vertical center, with a radius large relative
     * to `center` (and zero safety): the raw chord half-width (~1000)
     * comfortably EXCEEDS `center` (5) — the naive `center - half_w`
     * arithmetic would go deeply negative here without the clamp. */
    float margin = ff_layout_safe_margin_x(5.0f, 0.0f, 5.0f, 1000.0f, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, margin);
}

static void safe_margin_zero_radius_needs_full_width(void)
{
    /* Degenerate: no circle at all -> chord half-width is always 0 ->
     * margin is the full center coordinate (nothing fits). */
    float margin = ff_layout_safe_margin_x(220.0f, 0.0f, 220.0f, 0.0f, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 220.0f, margin);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(rect_fully_inside_is_true);
    RUN_TEST(rect_centered_on_circle_center_is_true);
    RUN_TEST(rect_with_one_corner_outside_is_false);
    RUN_TEST(rect_with_all_corners_outside_is_false);
    RUN_TEST(rect_corner_exactly_on_boundary_is_true);
    RUN_TEST(rect_corner_one_unit_past_boundary_is_false);
    RUN_TEST(negative_radius_is_always_false);
    RUN_TEST(tiny_rect_at_exact_center_fits_any_nonneg_radius);

    RUN_TEST(chord_at_center_equals_radius);
    RUN_TEST(chord_at_exact_radius_is_zero);
    RUN_TEST(chord_past_radius_is_zero);
    RUN_TEST(chord_is_symmetric_in_dy_sign);
    RUN_TEST(chord_matches_pythagorean_worked_example);
    RUN_TEST(chord_negative_radius_is_zero);
    RUN_TEST(chord_half_width_round_trips_through_rect_in_circle);

    RUN_TEST(safe_margin_at_center_band_is_near_zero);
    RUN_TEST(safe_margin_grows_as_band_moves_toward_the_pole);
    RUN_TEST(safe_margin_matches_compose_back_button_worked_example);
    RUN_TEST(safe_margin_is_never_negative);
    RUN_TEST(safe_margin_zero_radius_needs_full_width);

    return UNITY_END();
}
