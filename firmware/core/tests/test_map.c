/**
 * test_map.c — S09 core/map acceptance criteria (ff_map_xform_t).
 *
 * Test names follow docs/specs/S09-map-face.md's numbered acceptance
 * criteria: S09_ACn_description.
 */
#include <math.h>

#include "unity.h"

#include "ff_map.h"

#define FF_TEST_RADIUS_PX 206.0f /* S09 spec: "the 412 circle" */
#define FF_TEST_MARGIN_PX 24.0f
#define FF_TEST_USABLE_R (FF_TEST_RADIUS_PX - FF_TEST_MARGIN_PX)

void setUp(void) {}
void tearDown(void) {}

static float dist(float x, float y)
{
    return sqrtf(x * x + y * y);
}

/* ------------------------------------------------------------------- */
/* AC1 — bbox fit: every point inside circle radius-margin; aspect      */
/* preserved; degenerate bbox handled.                                  */
/* ------------------------------------------------------------------- */

static void S09_AC1_all_points_inside_usable_radius_rectangular_bbox(void)
{
    /* A non-square bbox with points at all four corners plus a couple of
     * interior points — the exact shape ff_map.h's doc comment argues a
     * naive diameter fit would clip. */
    float pts[][2] = {
        {-200.0f, -50.0f}, {200.0f, -50.0f}, {200.0f, 50.0f}, {-200.0f, 50.0f}, /* corners, 400x100 bbox */
        {0.0f, 0.0f},  {-50.0f, 20.0f},
    };
    int const n = (int)(sizeof(pts) / sizeof(pts[0]));

    ff_map_xform_t x;
    ff_map_xform_fit(&x, pts, n, FF_TEST_RADIUS_PX, FF_TEST_MARGIN_PX);

    for (int i = 0; i < n; i++) {
        float px, py;
        ff_map_project(&x, pts[i][0], pts[i][1], &px, &py);
        TEST_ASSERT_TRUE_MESSAGE(dist(px, py) <= FF_TEST_USABLE_R + 0.01f,
                                  "a feature point landed outside the usable circle radius");
    }
}

static void S09_AC1_square_bbox_corner_lands_exactly_on_usable_radius(void)
{
    /* A perfectly square bbox with a point at one corner is the equality
     * case the inscribed-square fit is built around: that corner should
     * land AT (not beyond) the usable radius, not comfortably inside it —
     * pins the exact scale factor, not just "somewhere inside". */
    float pts[][2] = {{-100.0f, -100.0f}, {100.0f, 100.0f}};
    ff_map_xform_t x;
    ff_map_xform_fit(&x, pts, 2, FF_TEST_RADIUS_PX, FF_TEST_MARGIN_PX);

    float px, py;
    ff_map_project(&x, 100.0f, 100.0f, &px, &py);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, FF_TEST_USABLE_R, dist(px, py));
}

static void S09_AC1_aspect_preserved_wide_and_tall_bbox_scale_equal_by_symmetry(void)
{
    /* A 400x100 (wide) bbox and a 100x400 (tall) bbox are mirror images
     * of each other across the diagonal — a fit that preserves aspect
     * (single uniform scale) must compute the identical scale factor for
     * both, since in each case the longer side is 400 and the shorter
     * 100. A fit that used independent x/y scales (breaking aspect)
     * would not necessarily agree here. */
    float wide[][2] = {{-200.0f, -50.0f}, {200.0f, 50.0f}};
    float tall[][2] = {{-50.0f, -200.0f}, {50.0f, 200.0f}};

    ff_map_xform_t xw, xt;
    ff_map_xform_fit(&xw, wide, 2, FF_TEST_RADIUS_PX, FF_TEST_MARGIN_PX);
    ff_map_xform_fit(&xt, tall, 2, FF_TEST_RADIUS_PX, FF_TEST_MARGIN_PX);

    TEST_ASSERT_FLOAT_WITHIN(0.0001f, xw.scale_px_per_m, xt.scale_px_per_m);
}

static void S09_AC1_north_up_a_point_north_of_another_renders_higher_on_screen(void)
{
    float pts[][2] = {{0.0f, -100.0f}, {0.0f, 100.0f}};
    ff_map_xform_t x;
    ff_map_xform_fit(&x, pts, 2, FF_TEST_RADIUS_PX, FF_TEST_MARGIN_PX);

    float px_south, py_south, px_north, py_north;
    ff_map_project(&x, 0.0f, -100.0f, &px_south, &py_south);
    ff_map_project(&x, 0.0f, 100.0f, &px_north, &py_north);

    /* North-up: greater north_m must render at a SMALLER (higher up)
     * screen y — screen y grows downward. */
    TEST_ASSERT_TRUE(py_north < py_south);
}

static void S09_AC1_degenerate_single_point_bbox_scale_is_finite_and_point_centers(void)
{
    float pts[][2] = {{123.0f, -45.0f}};
    ff_map_xform_t x;
    ff_map_xform_fit(&x, pts, 1, FF_TEST_RADIUS_PX, FF_TEST_MARGIN_PX);

    /* No divide-by-zero: scale must be finite and positive, never NaN or
     * Inf, and never zero (which would collapse every projection onto
     * the same pixel with no useful map at all). */
    TEST_ASSERT_TRUE(isfinite(x.scale_px_per_m));
    TEST_ASSERT_TRUE(x.scale_px_per_m > 0.0f);

    /* The lone point IS the bbox center, so it must project to the
     * circle's own center — (0, 0) in center-relative coordinates. */
    float px, py;
    ff_map_project(&x, 123.0f, -45.0f, &px, &py);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, px);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, py);
}

static void S09_AC1_degenerate_coincident_points_bbox_scale_is_finite(void)
{
    /* Several points that all happen to land on the same coordinate —
     * width and height both exactly zero, distinct code path from the
     * single-point-array case above (n > 1, not n == 1). */
    float pts[][2] = {{10.0f, 10.0f}, {10.0f, 10.0f}, {10.0f, 10.0f}};
    ff_map_xform_t x;
    ff_map_xform_fit(&x, pts, 3, FF_TEST_RADIUS_PX, FF_TEST_MARGIN_PX);

    TEST_ASSERT_TRUE(isfinite(x.scale_px_per_m));
    TEST_ASSERT_TRUE(x.scale_px_per_m > 0.0f);
}

static void S09_AC1_zero_features_falls_back_to_1km_square_around_origin(void)
{
    ff_map_xform_t x;
    ff_map_xform_fit(&x, NULL, 0, FF_TEST_RADIUS_PX, FF_TEST_MARGIN_PX);

    TEST_ASSERT_TRUE(isfinite(x.scale_px_per_m));
    TEST_ASSERT_TRUE(x.scale_px_per_m > 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, x.center_east_m);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, x.center_north_m);

    /* Hand-computed: fallback span is 1000m on the longer side, so
     * scale = usable_r * sqrt(2) / 1000. */
    float const expected_scale = (FF_TEST_USABLE_R * 1.41421356f) / 1000.0f;
    TEST_ASSERT_FLOAT_WITHIN(0.001f, expected_scale, x.scale_px_per_m);

    /* A point 500m east (the fallback square's own edge) must land
     * exactly at the inscribed-square's half-side, well inside the
     * usable circle radius (this is the flat-edge case, not the corner
     * equality case AC1's other test pins). */
    float px, py;
    ff_map_project(&x, 500.0f, 0.0f, &px, &py);
    TEST_ASSERT_TRUE(dist(px, py) <= FF_TEST_USABLE_R + 0.01f);
}

static void S09_AC1_null_out_is_a_safe_noop(void)
{
    /* Must not crash — nothing to assert beyond "returns". */
    ff_map_xform_fit(NULL, NULL, 0, FF_TEST_RADIUS_PX, FF_TEST_MARGIN_PX);
    TEST_PASS();
}

static void S09_AC1_null_xform_project_writes_zero_not_garbage(void)
{
    float px = 999.0f, py = 999.0f;
    ff_map_project(NULL, 42.0f, 42.0f, &px, &py);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, px);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, py);
}

/* ------------------------------------------------------------------- */
/* AC2 — crew/rally/YOU share the transform: a hand-computed 1px check. */
/* ------------------------------------------------------------------- */

static void S09_AC2_crew_rally_you_share_transform_hand_computed_1px(void)
{
    /* Two feature points define a simple, hand-checkable bbox: a 200m
     * (east) x 100m (north) rectangle centered on (50, 25). */
    float features[][2] = {{-50.0f, -25.0f}, {150.0f, 75.0f}};
    ff_map_xform_t x;
    ff_map_xform_fit(&x, features, 2, FF_TEST_RADIUS_PX, FF_TEST_MARGIN_PX);

    /* Hand computation: bbox center (50, 25), span = max(200,100) = 200,
     * scale = usable_r*sqrt(2)/200. */
    float const expected_scale = (FF_TEST_USABLE_R * 1.41421356f) / 200.0f;
    TEST_ASSERT_FLOAT_WITHIN(0.001f, expected_scale, x.scale_px_per_m);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 50.0f, x.center_east_m);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, x.center_north_m);

    /* A crew member 20m east, 10m north of the bbox center. */
    float crew_px, crew_py;
    ff_map_project(&x, 70.0f, 35.0f, &crew_px, &crew_py);
    float const expected_crew_dx = (70.0f - 50.0f) * expected_scale;
    float const expected_crew_dy = -((35.0f - 25.0f) * expected_scale);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, expected_crew_dx, crew_px);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, expected_crew_dy, crew_py);

    /* The rally point, 30m west, 40m south of the bbox center. */
    float rally_px, rally_py;
    ff_map_project(&x, 20.0f, -15.0f, &rally_px, &rally_py);
    float const expected_rally_dx = (20.0f - 50.0f) * expected_scale;
    float const expected_rally_dy = -((-15.0f - 25.0f) * expected_scale);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, expected_rally_dx, rally_px);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, expected_rally_dy, rally_py);

    /* YOU, exactly at the bbox center — must project to (0, 0). */
    float you_px, you_py;
    ff_map_project(&x, 50.0f, 25.0f, &you_px, &you_py);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 0.0f, you_px);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 0.0f, you_py);

    /* All three used the exact same `x` — proving they share the
     * transform is then just proving each one's math used x's own
     * fields, which the hand computations above already pin. */
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S09_AC1_all_points_inside_usable_radius_rectangular_bbox);
    RUN_TEST(S09_AC1_square_bbox_corner_lands_exactly_on_usable_radius);
    RUN_TEST(S09_AC1_aspect_preserved_wide_and_tall_bbox_scale_equal_by_symmetry);
    RUN_TEST(S09_AC1_north_up_a_point_north_of_another_renders_higher_on_screen);
    RUN_TEST(S09_AC1_degenerate_single_point_bbox_scale_is_finite_and_point_centers);
    RUN_TEST(S09_AC1_degenerate_coincident_points_bbox_scale_is_finite);
    RUN_TEST(S09_AC1_zero_features_falls_back_to_1km_square_around_origin);
    RUN_TEST(S09_AC1_null_out_is_a_safe_noop);
    RUN_TEST(S09_AC1_null_xform_project_writes_zero_not_garbage);

    RUN_TEST(S09_AC2_crew_rally_you_share_transform_hand_computed_1px);

    return UNITY_END();
}
