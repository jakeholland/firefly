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

/* ------------------------------------------------------------------- */
/* PR #73 review — ff_map_feature_render_kind (finding #6)              */
/* ------------------------------------------------------------------- */

static void S09_render_kind_zero_points_is_omit(void)
{
    TEST_ASSERT_EQUAL_INT(FF_MAP_RENDER_OMIT, ff_map_feature_render_kind(0, 0));
    TEST_ASSERT_EQUAL_INT(FF_MAP_RENDER_OMIT, ff_map_feature_render_kind(0, 1)); /* even a stage: no point at all */
}

static void S09_render_kind_one_point_stage_is_stub(void)
{
    TEST_ASSERT_EQUAL_INT(FF_MAP_RENDER_STAGE_STUB, ff_map_feature_render_kind(1, 1));
}

static void S09_render_kind_one_point_non_stage_is_label_only(void)
{
    TEST_ASSERT_EQUAL_INT(FF_MAP_RENDER_LABEL_ONLY, ff_map_feature_render_kind(1, 0));
}

static void S09_render_kind_two_points_is_line(void)
{
    TEST_ASSERT_EQUAL_INT(FF_MAP_RENDER_LINE, ff_map_feature_render_kind(2, 0));
    TEST_ASSERT_EQUAL_INT(FF_MAP_RENDER_LINE, ff_map_feature_render_kind(2, 1)); /* a 2-pt stage is still a line */
}

static void S09_render_kind_three_or_more_points_is_polygon(void)
{
    TEST_ASSERT_EQUAL_INT(FF_MAP_RENDER_POLYGON, ff_map_feature_render_kind(3, 0));
    TEST_ASSERT_EQUAL_INT(FF_MAP_RENDER_POLYGON, ff_map_feature_render_kind(9, 0));
    TEST_ASSERT_EQUAL_INT(FF_MAP_RENDER_POLYGON, ff_map_feature_render_kind(255, 0));
}

/* ------------------------------------------------------------------- */
/* PR #73, second review round — ff_map_feature_label_priority          */
/* (coordinator ruling: stages/landmarks/YOU are top priority and never */
/* dropped; a non-stage area polygon's label is droppable).             */
/* ------------------------------------------------------------------- */

static void S09_label_priority_stage_is_always_high_regardless_of_shape(void)
{
    TEST_ASSERT_EQUAL_INT(FF_MAP_LABEL_PRIORITY_HIGH, ff_map_feature_label_priority(1, 1)); /* stub */
    TEST_ASSERT_EQUAL_INT(FF_MAP_LABEL_PRIORITY_HIGH, ff_map_feature_label_priority(2, 1)); /* a 2-pt "stage" */
    TEST_ASSERT_EQUAL_INT(FF_MAP_LABEL_PRIORITY_HIGH,
                           ff_map_feature_label_priority(9, 1)); /* a real traced stage polygon */
}

static void S09_label_priority_single_point_landmark_is_high(void)
{
    TEST_ASSERT_EQUAL_INT(FF_MAP_LABEL_PRIORITY_HIGH, ff_map_feature_label_priority(1, 0));
}

static void S09_label_priority_line_is_high(void)
{
    TEST_ASSERT_EQUAL_INT(FF_MAP_LABEL_PRIORITY_HIGH, ff_map_feature_label_priority(2, 0));
}

static void S09_label_priority_non_stage_polygon_is_low(void)
{
    TEST_ASSERT_EQUAL_INT(FF_MAP_LABEL_PRIORITY_LOW, ff_map_feature_label_priority(3, 0));
    TEST_ASSERT_EQUAL_INT(FF_MAP_LABEL_PRIORITY_LOW, ff_map_feature_label_priority(9, 0)); /* e.g. venue extent */
    TEST_ASSERT_EQUAL_INT(FF_MAP_LABEL_PRIORITY_LOW, ff_map_feature_label_priority(255, 0));
}

/* ------------------------------------------------------------------- */
/* PR #73 THIRD review round — ff_map_place_labels, tested directly     */
/* against REAL Lost Lands label positions (finding #2, non-blocking    */
/* but closed anyway: a golden pixel-diff was proven NOT to catch a     */
/* mutation that fully undoes the drop-on-collision behavior — 780px    */
/* out of 207936 is comfortably under the 0.5% threshold. These assert  */
/* on the actual placement decision directly, the same "pull it into    */
/* core and test the real coordinates" fix ff_map_triangulate already   */
/* got for finding #2 of the first round).                              */
/*                                                                       */
/* Every position below is the REAL projected screen px (center-        */
/* relative) computed by feeding the actual, currently-merged Lost      */
/* Lands pack's real feature anchor points through the exact fit        */
/* ff_scr_map_build uses (bbox of all 11 real anchor points -> the      */
/* inscribed-square fit, radius 206/margin 24) — reproduced by hand      */
/* once (see the PR's fix-round reply for the derivation) and pinned    */
/* here as literals so this test has no dependency on scr_map.c/LVGL    */
/* at all. Two of these pairs are the review's own findings:            */
/* YOU-vs-Wompy-Woods (~39px, blocking finding #1) and Venue-extent-vs- */
/* Subsidia (~42px, the drop this whole mechanism exists to produce).   */
/* ------------------------------------------------------------------- */

static void S09_place_labels_you_nudges_off_real_wompy_woods_collision(void)
{
    /* Real px: Wompy Woods stage label (119.21, 50.80); YOU's label
     * before any nudge (82.63, 64.59) — ~39.09px apart, under the 48px
     * threshold. This is the exact real-pack collision PR #73's third
     * review round found: YOU never joined the collision system, so it
     * silently landed on top of a stage label. */
    ff_map_label_request_t in[2] = {
        {119.21074552754379f, 50.79579360612669f, FF_MAP_LABEL_PRIORITY_HIGH, 0.0f, 0.0f},
        {82.63474245710869f, 64.59494476689082f, FF_MAP_LABEL_PRIORITY_HIGH, 0.0f, 0.0f},
    };
    ff_map_label_result_t out[2];
    TEST_ASSERT_EQUAL_INT(2, ff_map_place_labels(in, 2, 48.0f, 6, FF_TEST_RADIUS_PX, out));

    /* Wompy Woods (placed first) never moves. */
    TEST_ASSERT_TRUE(out[0].placed);
    TEST_ASSERT_EQUAL_FLOAT(in[0].x, out[0].x);
    TEST_ASSERT_EQUAL_FLOAT(in[0].y, out[0].y);

    /* YOU is placed (HIGH priority: never dropped) and nudged — the
     * fix's whole point — to a position that ACTUALLY clears Wompy
     * Woods by the separation threshold, not merely "closer than
     * before". */
    TEST_ASSERT_TRUE(out[1].placed);
    float const dx = out[1].x - out[0].x;
    float const dy = out[1].y - out[0].y;
    float const dist = sqrtf(dx * dx + dy * dy);
    TEST_ASSERT_TRUE_MESSAGE(dist >= 48.0f, "YOU's label still collides with Wompy Woods after placement");
    /* Nudging only ever moves y (map_place_label_decluttered's own
     * documented mechanism) — x must be untouched. */
    TEST_ASSERT_EQUAL_FLOAT(in[1].x, out[1].x);
    TEST_ASSERT_TRUE(out[1].y > in[1].y); /* moved, not left in place */
}

static void S09_place_labels_venue_extent_drops_on_real_subsidia_collision(void)
{
    /* Real px: Subsidia Stage label (-10.84, -2.21), HIGH; Venue extent's
     * centroid label position (10.39, -38.13), LOW — ~41.7px apart,
     * under the 48px threshold. */
    ff_map_label_request_t in[2] = {
        {-10.837106496790302f, -2.208402922222037f, FF_MAP_LABEL_PRIORITY_HIGH, 0.0f, 0.0f},
        {10.38585103411318f, -38.13346469131977f, FF_MAP_LABEL_PRIORITY_LOW, 0.0f, 0.0f},
    };
    ff_map_label_result_t out[2];
    TEST_ASSERT_EQUAL_INT(2, ff_map_place_labels(in, 2, 48.0f, 6, FF_TEST_RADIUS_PX, out));

    TEST_ASSERT_TRUE(out[0].placed); /* Subsidia (HIGH): always placed, unmoved */
    TEST_ASSERT_EQUAL_FLOAT(in[0].x, out[0].x);
    TEST_ASSERT_EQUAL_FLOAT(in[0].y, out[0].y);

    /* Venue extent (LOW) collides -> DROPPED, not nudged: placed must
     * read false, and (documented contract) a dropped entry's x/y are
     * meaningless, so this test does not assert on them. */
    TEST_ASSERT_FALSE(out[1].placed);
}

static void S09_place_labels_high_priority_pair_far_apart_neither_moves(void)
{
    /* Real px: First aid (-120.57, -60.51) vs Village Marketplace
     * (90.76, -118.82) — ~219px apart, nowhere near the 48px threshold.
     * Negative control: the algorithm must not nudge when there is no
     * real collision. */
    ff_map_label_request_t in[2] = {
        {-120.56511570809566f, -60.512880866697f, FF_MAP_LABEL_PRIORITY_HIGH, 0.0f, 0.0f},
        {90.76272725015073f, -118.81778782164704f, FF_MAP_LABEL_PRIORITY_HIGH, 0.0f, 0.0f},
    };
    ff_map_label_result_t out[2];
    TEST_ASSERT_EQUAL_INT(2, ff_map_place_labels(in, 2, 48.0f, 6, FF_TEST_RADIUS_PX, out));

    TEST_ASSERT_TRUE(out[0].placed);
    TEST_ASSERT_TRUE(out[1].placed);
    TEST_ASSERT_EQUAL_FLOAT(in[1].x, out[1].x);
    TEST_ASSERT_EQUAL_FLOAT(in[1].y, out[1].y); /* unmoved: no collision to nudge away from */
}

static void S09_place_labels_low_priority_not_dropped_when_far_enough(void)
{
    /* Real px: First aid (HIGH) vs RV/tent camping (LOW), ~198px apart
     * — proves LOW isn't unconditionally dropped, only on an actual
     * collision. */
    ff_map_label_request_t in[2] = {
        {-120.56511570809566f, -60.512880866697f, FF_MAP_LABEL_PRIORITY_HIGH, 0.0f, 0.0f},
        {77.21607798377555f, -61.396443829401875f, FF_MAP_LABEL_PRIORITY_LOW, 0.0f, 0.0f},
    };
    ff_map_label_result_t out[2];
    TEST_ASSERT_EQUAL_INT(2, ff_map_place_labels(in, 2, 48.0f, 6, FF_TEST_RADIUS_PX, out));

    TEST_ASSERT_TRUE(out[0].placed);
    TEST_ASSERT_TRUE(out[1].placed);
    TEST_ASSERT_EQUAL_FLOAT(in[1].x, out[1].x);
    TEST_ASSERT_EQUAL_FLOAT(in[1].y, out[1].y);
}

static void S09_place_labels_rejects_bad_args(void)
{
    ff_map_label_request_t in[1] = {{0.0f, 0.0f, FF_MAP_LABEL_PRIORITY_HIGH, 0.0f, 0.0f}};
    ff_map_label_result_t out[1];
    TEST_ASSERT_EQUAL_INT(-1, ff_map_place_labels(NULL, 1, 48.0f, 6, FF_TEST_RADIUS_PX, out));
    TEST_ASSERT_EQUAL_INT(-1, ff_map_place_labels(in, 1, 48.0f, 6, FF_TEST_RADIUS_PX, NULL));
    TEST_ASSERT_EQUAL_INT(-1, ff_map_place_labels(in, -1, 48.0f, 6, FF_TEST_RADIUS_PX, out));
    TEST_ASSERT_EQUAL_INT(-1, ff_map_place_labels(in, FF_MAP_LABEL_MAX_ITEMS + 1, 48.0f, 6, FF_TEST_RADIUS_PX, out));
}

static void S09_place_labels_zero_items_is_a_safe_noop(void)
{
    ff_map_label_result_t out[1];
    TEST_ASSERT_EQUAL_INT(0, ff_map_place_labels(NULL, 0, 48.0f, 6, FF_TEST_RADIUS_PX, out));
}

/* ------------------------------------------------------------------- */
/* Issue #75/#77 — ff_map_clip_point_to_circle and the circle-bounds     */
/* half of ff_map_place_labels (folding in #77's "ultra-long labels run  */
/* off the circle edge uncropped").                                      */
/* ------------------------------------------------------------------- */

static void S75_clip_point_inside_circle_is_unchanged(void)
{
    float ox, oy;
    ff_map_clip_point_to_circle(30.0f, -40.0f, FF_TEST_RADIUS_PX, &ox, &oy);
    TEST_ASSERT_EQUAL_FLOAT(30.0f, ox);
    TEST_ASSERT_EQUAL_FLOAT(-40.0f, oy);
}

static void S75_clip_point_outside_circle_lands_exactly_on_radius(void)
{
    /* (300, 400): distance 500, well outside a 100px radius. Scaled by
     * 100/500 = 0.2 along the SAME direction -> (60, 80), distance
     * exactly 100. */
    float ox, oy;
    ff_map_clip_point_to_circle(300.0f, 400.0f, 100.0f, &ox, &oy);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 60.0f, ox);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 80.0f, oy);
    float const d = sqrtf(ox * ox + oy * oy);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, d);
}

static void S75_clip_point_zero_or_negative_radius_passes_through(void)
{
    float ox, oy;
    ff_map_clip_point_to_circle(300.0f, 400.0f, 0.0f, &ox, &oy);
    TEST_ASSERT_EQUAL_FLOAT(300.0f, ox);
    TEST_ASSERT_EQUAL_FLOAT(400.0f, oy);

    ff_map_clip_point_to_circle(300.0f, 400.0f, -10.0f, &ox, &oy);
    TEST_ASSERT_EQUAL_FLOAT(300.0f, ox);
    TEST_ASSERT_EQUAL_FLOAT(400.0f, oy);
}

static void S75_clip_point_at_origin_stays_at_origin(void)
{
    float ox, oy;
    ff_map_clip_point_to_circle(0.0f, 0.0f, FF_TEST_RADIUS_PX, &ox, &oy);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ox);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, oy);
}

/* #77's actual repro shape: a LOW-priority (non-stage polygon) label,
 * far enough from every other label to rule out an ordinary collision,
 * whose own text half-width pushes its bounding box past the circle —
 * must be DROPPED (shape still draws — this test only exercises the
 * label decision), not silently drawn off-glass uncropped. */
static void S77_place_labels_low_priority_ultra_long_label_drops_off_circle(void)
{
    /* Anchor at (150, 0), 154px from center — inside FF_TEST_RADIUS_PX
     * (206) on its own, but half_w=120 (a "made-up ~30-char label",
     * #77's own words) pushes the far corner to 270px, well past 206. */
    ff_map_label_request_t in[1] = {{150.0f, 0.0f, FF_MAP_LABEL_PRIORITY_LOW, 120.0f, 8.0f}};
    ff_map_label_result_t out[1];
    TEST_ASSERT_EQUAL_INT(1, ff_map_place_labels(in, 1, 48.0f, 6, FF_TEST_RADIUS_PX, out));
    TEST_ASSERT_FALSE_MESSAGE(out[0].placed, "ultra-long LOW label must be dropped, not drawn past the circle");
}

/* The HIGH-priority counterpart: never dropped (it has no other visual
 * representation), so an off-circle result must be pulled radially
 * inward until its own bounding box is honestly contained. */
static void S77_place_labels_high_priority_ultra_long_label_clamped_not_dropped(void)
{
    ff_map_label_request_t in[1] = {{150.0f, 0.0f, FF_MAP_LABEL_PRIORITY_HIGH, 120.0f, 8.0f}};
    ff_map_label_result_t out[1];
    TEST_ASSERT_EQUAL_INT(1, ff_map_place_labels(in, 1, 48.0f, 6, FF_TEST_RADIUS_PX, out));
    TEST_ASSERT_TRUE_MESSAGE(out[0].placed, "HIGH labels are never dropped");

    /* The RESULT's own bounding box (half_w/half_h unchanged by a
     * position clamp) must fit inside the circle — same per-corner check
     * ff_map_place_labels itself uses internally. */
    float const fx = (out[0].x < 0.0f ? -out[0].x : out[0].x) + in[0].half_w;
    float const fy = (out[0].y < 0.0f ? -out[0].y : out[0].y) + in[0].half_h;
    float const far_corner = sqrtf(fx * fx + fy * fy);
    TEST_ASSERT_TRUE_MESSAGE(far_corner <= FF_TEST_RADIUS_PX + 0.01f,
                              "HIGH label's bounding box must be pulled inside the circle, not left hanging off it");
    /* And it actually moved (wasn't left at its original off-circle spot). */
    TEST_ASSERT_TRUE(out[0].x != in[0].x || out[0].y != in[0].y);
}

/* circle_radius_px <= 0 disables the bounds check entirely — a caller
 * that doesn't care about circle containment (or hasn't been updated to
 * supply half_w/half_h) gets exactly the pre-#77 behavior back. */
static void S77_place_labels_zero_radius_disables_bounds_check(void)
{
    ff_map_label_request_t in[1] = {{150.0f, 0.0f, FF_MAP_LABEL_PRIORITY_LOW, 120.0f, 8.0f}};
    ff_map_label_result_t out[1];
    TEST_ASSERT_EQUAL_INT(1, ff_map_place_labels(in, 1, 48.0f, 6, 0.0f, out));
    TEST_ASSERT_TRUE(out[0].placed);
    TEST_ASSERT_EQUAL_FLOAT(in[0].x, out[0].x);
    TEST_ASSERT_EQUAL_FLOAT(in[0].y, out[0].y);
}

/* PR #82 review (BLOCKING): a HIGH label's circle-pull must not silently
 * re-introduce a label-label collision it never re-checks. This is the
 * exact real-pack failure the review caught in map_real_lost_lands —
 * "Tunnel entrance (Rt 13)" pulled radially inward and landed on top of
 * "Prehistoric Stage", passing both the golden pixel-diff threshold AND
 * test_map_circle_containment.c's circle-only containment check, because
 * neither one checks "still clear of every other label AFTER the pull".
 *
 * Reproduced directly here, independent of any real pack: A is placed
 * first, safely inside the circle. B's RAW anchor sits far outside the
 * circle in the exact SAME angular direction as A — a radial pull always
 * moves a point straight toward the origin along its own direction, so
 * B's naive circle-pull lands close to A by construction, the precise
 * geometry that broke the old two-independent-passes version.
 *
 * Mutation check (confirmed against the actual code, not asserted from
 * reasoning): reverting `ff_map_place_labels` to pull ONCE after the
 * nudge loop, with no per-attempt re-pull/re-check (this function's
 * pre-#82-review shape), makes `sep_ok` below false — B lands within
 * `min_sep` of A and stays there, because the one-shot pull is never
 * re-validated against the labels already placed. */
static void S82_place_labels_high_priority_circle_pull_still_respects_separation(void)
{
    float const radius = 200.0f;
    float const min_sep = 40.0f;

    ff_map_label_request_t in[2] = {
        {190.0f, 0.0f, FF_MAP_LABEL_PRIORITY_HIGH, 0.0f, 0.0f},  /* A: already safely inside the circle */
        {1000.0f, 0.0f, FF_MAP_LABEL_PRIORITY_HIGH, 0.0f, 0.0f}, /* B: raw anchor far outside, same direction as A */
    };
    ff_map_label_result_t out[2];
    TEST_ASSERT_EQUAL_INT(2, ff_map_place_labels(in, 2, min_sep, 6, radius, out));

    TEST_ASSERT_TRUE(out[0].placed);
    TEST_ASSERT_TRUE(out[1].placed);

    /* Both HIGH labels must land inside the circle... */
    float const d0 = sqrtf(out[0].x * out[0].x + out[0].y * out[0].y);
    float const d1 = sqrtf(out[1].x * out[1].x + out[1].y * out[1].y);
    TEST_ASSERT_TRUE_MESSAGE(d0 <= radius + 0.01f, "A must stay inside the circle");
    TEST_ASSERT_TRUE_MESSAGE(d1 <= radius + 0.01f, "B's circle-pull must land inside the circle");

    /* ...AND B must still clear A by min_sep_px — the property a
     * one-shot, never-re-checked pull silently violates. */
    float const dx = out[1].x - out[0].x;
    float const dy = out[1].y - out[0].y;
    float const sep = sqrtf(dx * dx + dy * dy);
    bool const sep_ok = sep >= min_sep - 0.01f;
    TEST_ASSERT_TRUE_MESSAGE(sep_ok, "B's circle-pull re-introduced a collision with A that was never re-checked");
}

/* ------------------------------------------------------------------- */
/* PR #73 review — ff_map_triangulate (finding #2: concave fill)        */
/* ------------------------------------------------------------------- */

/* Shoelace polygon area (unsigned) — the reference this file checks a
 * triangulation's summed triangle area against. A correct triangulation
 * of a simple polygon covers it EXACTLY once, so the two must match
 * regardless of convexity — this is a much stronger property than "some
 * triangles came out", the same "measure the property, not a proxy"
 * discipline docs/review/code-review.md asks for. */
static float polygon_area(float const pts[][2], int n)
{
    float area2 = 0.0f;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area2 += pts[i][0] * pts[j][1] - pts[j][0] * pts[i][1];
    }
    return (area2 < 0.0f ? -area2 : area2) * 0.5f;
}

static float triangle_area(float ax, float ay, float bx, float by, float cx, float cy)
{
    float area2 = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
    return (area2 < 0.0f ? -area2 : area2) * 0.5f;
}

static float triangulation_area(float const pts[][2], uint8_t const tris[][3], int n_tris)
{
    float sum = 0.0f;
    for (int i = 0; i < n_tris; i++) {
        uint8_t a = tris[i][0], b = tris[i][1], c = tris[i][2];
        sum += triangle_area(pts[a][0], pts[a][1], pts[b][0], pts[b][1], pts[c][0], pts[c][1]);
    }
    return sum;
}

static void S09_triangulate_convex_square_covers_exact_area(void)
{
    float pts[][2] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f}, {0.0f, 10.0f}};
    uint8_t tris[2][3];
    int n = ff_map_triangulate(pts, 4, tris, 2);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, polygon_area(pts, 4), triangulation_area(pts, tris, n));
}

/* The real, currently-merged Lost Lands festpack's "Venue extent"
 * feature, projected through the actual ff_geo_project formula
 * fp_parse uses (origin = festival.venue, 39.9387/-82.4027) — the exact
 * concave polygon PR #73's tier-3 review found the old fan-fill
 * mis-rendering (reflex turn at vertex 7). Hand-verified concave via a
 * cross-product convexity sweep before this test was written (see the
 * PR's fix-round reply for the numbers). This is the regression test:
 * if triangulation ever regresses to a fan (or anything else that
 * doesn't exactly tile a concave polygon), the area check below catches
 * it — a fan over this exact shape overcounts the notch near vertex 7. */
static void S09_triangulate_real_venue_extent_is_concave_and_covers_exact_area(void)
{
    float pts[][2] = {
        {-477.44f, 177.91f}, {-537.12f, 366.94f}, {-281.35f, 478.14f}, {59.68f, 478.14f},
        {187.56f, 255.75f},  {144.94f, -133.43f}, {59.68f, -211.27f},  {-153.46f, -22.24f},
        {-366.60f, 88.96f},
    };
    int const n = 9;
    uint8_t tris[7][3];
    int const n_tris = ff_map_triangulate(pts, n, tris, 7);
    TEST_ASSERT_EQUAL_INT(7, n_tris); /* n - 2 */
    TEST_ASSERT_FLOAT_WITHIN(5.0f, polygon_area(pts, n), triangulation_area(pts, tris, n_tris));

    /* Every emitted triangle must have a REAL (non-degenerate) area —
     * the mutation-conscious half of this test: a fan-fill regression
     * over this concave shape would still emit n-2 triangles and could
     * still coincidentally sum close to the right total area while
     * containing a triangle that folds back on itself; requiring every
     * individual triangle to carry a meaningfully positive area rules
     * that out. */
    for (int i = 0; i < n_tris; i++) {
        uint8_t a = tris[i][0], b = tris[i][1], c = tris[i][2];
        float area = triangle_area(pts[a][0], pts[a][1], pts[b][0], pts[b][1], pts[c][0], pts[c][1]);
        TEST_ASSERT_TRUE(area > 100.0f);
    }
}

static void S09_triangulate_concave_l_shape_covers_exact_area(void)
{
    /* An "L": a 10x10 square with a 5x5 notch bitten out of one corner —
     * the simplest genuinely concave polygon, independent of the
     * real-data fixture above. */
    float pts[][2] = {
        {0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 5.0f}, {5.0f, 5.0f}, {5.0f, 10.0f}, {0.0f, 10.0f},
    };
    uint8_t tris[4][3];
    int n_tris = ff_map_triangulate(pts, 6, tris, 4);
    TEST_ASSERT_EQUAL_INT(4, n_tris);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, polygon_area(pts, 6), triangulation_area(pts, tris, n_tris));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 75.0f, polygon_area(pts, 6)); /* 100 - 25 */
}

static void S09_triangulate_rejects_too_few_points(void)
{
    float pts[][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}};
    uint8_t tris[1][3];
    TEST_ASSERT_EQUAL_INT(-1, ff_map_triangulate(pts, 2, tris, 1));
}

static void S09_triangulate_rejects_insufficient_out_max(void)
{
    float pts[][2] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f}, {0.0f, 10.0f}};
    uint8_t tris[1][3]; /* needs 2, given 1 — never writes a partial triangulation */
    TEST_ASSERT_EQUAL_INT(-1, ff_map_triangulate(pts, 4, tris, 1));
}

static void S09_triangulate_rejects_degenerate_collinear_input(void)
{
    float pts[][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {2.0f, 0.0f}};
    uint8_t tris[1][3];
    TEST_ASSERT_EQUAL_INT(-1, ff_map_triangulate(pts, 3, tris, 1));
}

static void S09_triangulate_null_safe(void)
{
    uint8_t tris[1][3];
    TEST_ASSERT_EQUAL_INT(-1, ff_map_triangulate(NULL, 3, tris, 1));
    float pts[][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}};
    TEST_ASSERT_EQUAL_INT(-1, ff_map_triangulate(pts, 3, NULL, 1));
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

    RUN_TEST(S09_render_kind_zero_points_is_omit);
    RUN_TEST(S09_render_kind_one_point_stage_is_stub);
    RUN_TEST(S09_render_kind_one_point_non_stage_is_label_only);
    RUN_TEST(S09_render_kind_two_points_is_line);
    RUN_TEST(S09_render_kind_three_or_more_points_is_polygon);

    RUN_TEST(S09_label_priority_stage_is_always_high_regardless_of_shape);
    RUN_TEST(S09_label_priority_single_point_landmark_is_high);
    RUN_TEST(S09_label_priority_line_is_high);
    RUN_TEST(S09_label_priority_non_stage_polygon_is_low);

    RUN_TEST(S09_place_labels_you_nudges_off_real_wompy_woods_collision);
    RUN_TEST(S09_place_labels_venue_extent_drops_on_real_subsidia_collision);
    RUN_TEST(S09_place_labels_high_priority_pair_far_apart_neither_moves);
    RUN_TEST(S09_place_labels_low_priority_not_dropped_when_far_enough);
    RUN_TEST(S09_place_labels_rejects_bad_args);
    RUN_TEST(S09_place_labels_zero_items_is_a_safe_noop);

    RUN_TEST(S75_clip_point_inside_circle_is_unchanged);
    RUN_TEST(S75_clip_point_outside_circle_lands_exactly_on_radius);
    RUN_TEST(S75_clip_point_zero_or_negative_radius_passes_through);
    RUN_TEST(S75_clip_point_at_origin_stays_at_origin);

    RUN_TEST(S77_place_labels_low_priority_ultra_long_label_drops_off_circle);
    RUN_TEST(S77_place_labels_high_priority_ultra_long_label_clamped_not_dropped);
    RUN_TEST(S77_place_labels_zero_radius_disables_bounds_check);
    RUN_TEST(S82_place_labels_high_priority_circle_pull_still_respects_separation);

    RUN_TEST(S09_triangulate_convex_square_covers_exact_area);
    RUN_TEST(S09_triangulate_real_venue_extent_is_concave_and_covers_exact_area);
    RUN_TEST(S09_triangulate_concave_l_shape_covers_exact_area);
    RUN_TEST(S09_triangulate_rejects_too_few_points);
    RUN_TEST(S09_triangulate_rejects_insufficient_out_max);
    RUN_TEST(S09_triangulate_rejects_degenerate_collinear_input);
    RUN_TEST(S09_triangulate_null_safe);

    return UNITY_END();
}
