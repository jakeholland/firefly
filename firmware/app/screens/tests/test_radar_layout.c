/**
 * test_radar_layout.c — geometry-level regression coverage for the
 * radar-face layout resolver (PR #16 UX review round 3 / code review
 * round 2: "goldens are pixel-diffs against themselves, so a
 * partially-broken render passes forever as long as it's stable... the
 * test must be assertion-level on the resolver's output geometry").
 *
 * No LVGL anywhere in this file — radar_layout.c is plain C11 geometry,
 * so every assertion here is a direct check on the actual numbers the
 * renderer will draw, not a pixel-diff proxy for them.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "radar_layout.h"

/* Avoid relying on the POSIX-only M_PI (undefined under strict -std=c11
 * on some libcs) — same rationale as core/src/ff_geo.c's own FF_GEO_PI. */
#define TEST_RADAR_LAYOUT_PI 3.14159265358979323846f

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------------------
 * Shared geometry helpers.
 * ------------------------------------------------------------------- */

static bool rects_overlap(radar_layout_rect_t const *a, radar_layout_rect_t const *b)
{
    return a->x1 < b->x2 && a->x2 > b->x1 && a->y1 < b->y2 && a->y2 > b->y1;
}

static radar_layout_rect_t square_at(float cx, float cy, float size)
{
    float half = size / 2.0f;
    radar_layout_rect_t r = {cx - half, cy - half, cx + half, cy + half};
    return r;
}

static radar_layout_rect_t dot_rect(radar_layout_dot_result_t const *d)
{
    return square_at(d->dx, d->dy, RADAR_LAYOUT_DOT_PX);
}

/* A minimal bounding square around the arrow's head (tip + both base
 * corners) — good enough to check "does the head land inside reserved
 * chrome", which is exactly what the resolver itself tests against. */
static radar_layout_rect_t arrow_head_bbox(radar_layout_arrow_t const *a)
{
    float min_x = a->tip_dx, max_x = a->tip_dx;
    float min_y = a->tip_dy, max_y = a->tip_dy;
    float xs[2] = {a->left_dx, a->right_dx};
    float ys[2] = {a->left_dy, a->right_dy};
    for (int i = 0; i < 2; i++) {
        if (xs[i] < min_x) min_x = xs[i];
        if (xs[i] > max_x) max_x = xs[i];
        if (ys[i] < min_y) min_y = ys[i];
        if (ys[i] > max_y) max_y = ys[i];
    }
    radar_layout_rect_t r = {min_x, min_y, max_x, max_y};
    return r;
}

static bool registry_overlaps_rect(radar_layout_registry_t const *reg, radar_layout_rect_t const *r)
{
    for (int i = 0; i < reg->count; i++) {
        if (rects_overlap(&reg->rects[i], r)) {
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------
 * AC — every dot, at every bearing, in every mode: never overlaps the
 * registry. Full tenth-degree sweep (3600 samples/mode), matching the
 * code review's own audit methodology exactly, so this is the automated
 * version of the check that caught the bug by hand.
 * ------------------------------------------------------------------- */

static void sweep_single_dot_never_overlaps_registry(radar_mode_t mode, bool never_fixed, char const *label)
{
    radar_layout_registry_t reg;
    radar_layout_build_registry(mode, never_fixed, &reg);

    for (int tenth_deg = 0; tenth_deg < 3600; tenth_deg++) {
        float bearing = (float)tenth_deg / 10.0f;
        radar_layout_dot_result_t dot;
        radar_layout_resolve_dots(&reg, &bearing, 1, &dot);

        radar_layout_rect_t dr = dot_rect(&dot);
        if (registry_overlaps_rect(&reg, &dr)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "%s: dot at bearing %.1f resolved to (%.1f,%.1f), still inside reserved chrome",
                      label, (double)bearing, (double)dot.dx, (double)dot.dy);
            TEST_FAIL_MESSAGE(msg);
        }
    }
}

static void test_dot_sweep_live(void)
{
    sweep_single_dot_never_overlaps_registry(RADAR_LIVE, false, "LIVE");
}
static void test_dot_sweep_stale(void)
{
    sweep_single_dot_never_overlaps_registry(RADAR_STALE, false, "STALE");
}
static void test_dot_sweep_lost_real_fix(void)
{
    sweep_single_dot_never_overlaps_registry(RADAR_LOST, false, "LOST (real fix)");
}
static void test_dot_sweep_lost_never_fixed(void)
{
    sweep_single_dot_never_overlaps_registry(RADAR_LOST, true, "LOST (never fixed)");
}
static void test_dot_sweep_close(void)
{
    /* CLOSE has the most reserved chrome of any mode (ring stack + name/
     * chip + FLARE + status bar + page dots) — the worst case for a
     * single dot to have to dodge. */
    sweep_single_dot_never_overlaps_registry(RADAR_CLOSE, false, "CLOSE");
}
static void test_dot_sweep_nofix(void)
{
    sweep_single_dot_never_overlaps_registry(RADAR_NOFIX, false, "NOFIX");
}
static void test_dot_sweep_nosel(void)
{
    sweep_single_dot_never_overlaps_registry(RADAR_NOSEL, false, "NOSEL");
}

/* ---------------------------------------------------------------------
 * AC — arrow: full tenth-degree sweep for every mode that actually shows
 * one (arrow_valid is true only for LIVE/STALE/LOST-with-a-real-fix —
 * ff_radar.h) — never overlaps the registry, and never changes bearing
 * (only ever gives up length).
 * ------------------------------------------------------------------- */

static void sweep_arrow_never_overlaps_registry_or_changes_bearing(radar_mode_t mode, char const *label)
{
    radar_layout_registry_t reg;
    radar_layout_build_registry(mode, false, &reg);

    for (int tenth_deg = 0; tenth_deg < 3600; tenth_deg++) {
        float bearing = (float)tenth_deg / 10.0f;
        radar_layout_arrow_t arrow;
        radar_layout_resolve_arrow(&reg, bearing, &arrow);

        radar_layout_rect_t bbox = arrow_head_bbox(&arrow);
        if (registry_overlaps_rect(&reg, &bbox)) {
            char msg[160];
            snprintf(msg, sizeof(msg), "%s: arrow at bearing %.1f (len %.1f) still overlaps reserved chrome", label,
                      (double)bearing, (double)arrow.len_px);
            TEST_FAIL_MESSAGE(msg);
        }

        /* Never fakes the bearing: the tip must sit exactly on the true
         * ray, just possibly closer in (CLAUDE.md's honesty rule — see
         * radar_layout.h's doc comment). Reconstruct the expected
         * unit-direction from the resolved tip and compare against the
         * bearing's own direction; skip the (extremely rare) case where
         * shortening bottomed all the way out to ~0 length, where the
         * direction of a near-zero vector is numerically meaningless. */
        float tip_mag = sqrtf(arrow.tip_dx * arrow.tip_dx + arrow.tip_dy * arrow.tip_dy);
        if (tip_mag > 1.0f) {
            float rad = bearing * (TEST_RADAR_LAYOUT_PI / 180.0f);
            float expected_x = sinf(rad), expected_y = -cosf(rad);
            float actual_x = arrow.tip_dx / tip_mag, actual_y = arrow.tip_dy / tip_mag;
            TEST_ASSERT_FLOAT_WITHIN(0.02f, expected_x, actual_x);
            TEST_ASSERT_FLOAT_WITHIN(0.02f, expected_y, actual_y);
        }
    }
}

static void test_arrow_sweep_live(void)
{
    sweep_arrow_never_overlaps_registry_or_changes_bearing(RADAR_LIVE, "LIVE");
}
static void test_arrow_sweep_stale(void)
{
    sweep_arrow_never_overlaps_registry_or_changes_bearing(RADAR_STALE, "STALE");
}
static void test_arrow_sweep_lost(void)
{
    /* This is the exact regression: LOST's ghost arrowhead was found
     * drawn through the "~1.1 km" distance text (PR #16 UX review round
     * 3, finding #2). */
    sweep_arrow_never_overlaps_registry_or_changes_bearing(RADAR_LOST, "LOST (real fix)");
}

static void test_arrow_not_shortened_when_clear(void)
{
    radar_layout_registry_t reg;
    radar_layout_build_registry(RADAR_LIVE, false, &reg);

    /* Due "north" (straight up, away from the name/dist/chip stack and
     * the status bar) needs no shortening at all. */
    radar_layout_arrow_t arrow;
    radar_layout_resolve_arrow(&reg, 0.0f, &arrow);

    TEST_ASSERT_FALSE(arrow.shortened);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, RADAR_LAYOUT_ARROW_LEN_PX, arrow.len_px);
}

/* ---------------------------------------------------------------------
 * AC — dot-vs-dot: ORCHESTRATOR RULING round 4 — cluster, never hide.
 * The reviewer's own worst-case fixture: all 8 crew dots at (nearly) the
 * same bearing, in CLOSE mode (max chrome).
 * ------------------------------------------------------------------- */

static void test_all_8_dots_same_bearing_close_mode_cluster_not_hidden(void)
{
    radar_layout_registry_t reg;
    radar_layout_build_registry(RADAR_CLOSE, false, &reg);

    float ring_deg[8] = {90.0f, 90.0f, 90.0f, 90.0f, 90.0f, 90.0f, 90.0f, 90.0f};
    radar_layout_dot_result_t dots[8];
    radar_layout_resolve_dots(&reg, ring_deg, 8, dots);

    /* 1. Nobody is dropped: every one of the 8 original dots belongs to
     * exactly one cluster, and the cluster sizes across the DISTINCT
     * cluster ids sum to exactly 8 — the honesty invariant this whole
     * rework exists for. */
    bool seen_cluster[8] = {false};
    int total_accounted = 0;
    for (int i = 0; i < 8; i++) {
        int id = dots[i].cluster_id;
        TEST_ASSERT_TRUE_MESSAGE(id >= 0 && id < 8, "cluster_id out of range");
        if (!seen_cluster[id]) {
            seen_cluster[id] = true;
            total_accounted += dots[i].cluster_size;
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(8, total_accounted,
                                    "cluster sizes across distinct clusters must sum to all 8 original dots");

    /* 2. No two DISTINCT cluster markers overlap each other or any
     * reserved rectangle. */
    for (int i = 0; i < 8; i++) {
        if (!seen_cluster[dots[i].cluster_id] || dots[i].cluster_id != i) {
            /* only check each distinct cluster once, at its root index */
            continue;
        }
        radar_layout_rect_t dr = dot_rect(&dots[i]);
        TEST_ASSERT_FALSE_MESSAGE(registry_overlaps_rect(&reg, &dr), "a cluster marker overlaps reserved chrome");

        for (int j = 0; j < 8; j++) {
            if (j == i || dots[j].cluster_id != j) {
                continue; /* only compare against other distinct cluster roots */
            }
            radar_layout_rect_t dr2 = dot_rect(&dots[j]);
            TEST_ASSERT_FALSE_MESSAGE(rects_overlap(&dr, &dr2), "two distinct cluster markers overlap each other");
        }
    }
}

static void test_widely_spaced_dots_stay_distinct_not_clustered(void)
{
    radar_layout_registry_t reg;
    radar_layout_build_registry(RADAR_LIVE, false, &reg);

    /* Four dots 90 degrees apart (N/E/S/W) — far enough apart that
     * clustering them would itself be a bug (losing real distinctions
     * between crew members who are nowhere near each other). */
    float ring_deg[4] = {0.0f, 90.0f, 180.0f, 270.0f};
    radar_layout_dot_result_t dots[4];
    radar_layout_resolve_dots(&reg, ring_deg, 4, dots);

    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, dots[i].cluster_size, "widely-spaced dots should not have clustered");
        TEST_ASSERT_EQUAL_INT(i, dots[i].cluster_id);
    }
}

/* ---------------------------------------------------------------------
 * Cluster marker wedge ring (issue #18 — "the cluster marker reads as a
 * badge, not as friends"). radar_layout_cluster_wedges is the pure half
 * of that fix: scr_radar.c only paints the angles it returns, so these
 * assertions are on the actual geometry that reaches the glass.
 *
 * The invariant that matters most here is the same one the clustering
 * ruling itself protects — EVERY member is represented. A wedge ring
 * that silently dropped a member would be the same lie by omission as
 * hiding the dot, just one layer further in.
 * ------------------------------------------------------------------- */

/* Angular width of a wedge, handling the one that crosses the 0/360
 * seam (end_deg < start_deg — see radar_layout_wedge_t's doc comment). */
static float wedge_span(radar_layout_wedge_t const *w)
{
    float span = w->end_deg - w->start_deg;
    if (span < 0.0f) {
        span += 360.0f;
    }
    return span;
}

static void test_cluster_wedges_one_per_member_in_index_order(void)
{
    /* A hand-built resolved[] — this function's contract is over the
     * cluster_id/cluster_size fields alone, so it doesn't need a real
     * resolve pass to exercise. Four dots, all one cluster rooted at 0. */
    radar_layout_dot_result_t resolved[4];
    for (int i = 0; i < 4; i++) {
        resolved[i].dx = 0.0f;
        resolved[i].dy = 0.0f;
        resolved[i].cluster_id = 0;
        resolved[i].cluster_size = 4;
    }

    radar_layout_wedge_t wedges[FF_CREW_MAX];
    int n = radar_layout_cluster_wedges(resolved, 4, 0, wedges, FF_CREW_MAX);

    TEST_ASSERT_EQUAL_INT_MESSAGE(4, n, "every clustered member must get a wedge — none dropped");
    for (int k = 0; k < n; k++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(k, wedges[k].index,
                                        "wedges must be emitted in ascending original-dot-index order");
    }
}

static void test_cluster_wedges_only_include_that_cluster(void)
{
    /* Two separate clusters in one resolved[]: {0,2} and {1,3}. Asking
     * for one must never leak a member of the other into its ring —
     * that would paint a crew color onto a marker standing somewhere
     * else entirely. */
    radar_layout_dot_result_t resolved[4];
    int ids[4] = {0, 1, 0, 1};
    for (int i = 0; i < 4; i++) {
        resolved[i].dx = 0.0f;
        resolved[i].dy = 0.0f;
        resolved[i].cluster_id = ids[i];
        resolved[i].cluster_size = 2;
    }

    radar_layout_wedge_t wedges[FF_CREW_MAX];

    int n0 = radar_layout_cluster_wedges(resolved, 4, 0, wedges, FF_CREW_MAX);
    TEST_ASSERT_EQUAL_INT(2, n0);
    TEST_ASSERT_EQUAL_INT(0, wedges[0].index);
    TEST_ASSERT_EQUAL_INT(2, wedges[1].index);

    int n1 = radar_layout_cluster_wedges(resolved, 4, 1, wedges, FF_CREW_MAX);
    TEST_ASSERT_EQUAL_INT(2, n1);
    TEST_ASSERT_EQUAL_INT(1, wedges[0].index);
    TEST_ASSERT_EQUAL_INT(3, wedges[1].index);
}

static void test_cluster_wedges_are_equal_gapped_and_cover_the_ring(void)
{
    /* Each wedge is an equal slice minus one full gap's worth of dark
     * fill, and the wedges plus their gaps account for the whole 360 —
     * i.e. no member silently gets a bigger or a zero-width share. Swept
     * across every cluster size a real crew can produce (FF_CREW_MAX
     * is 8). */
    for (int size = 2; size <= FF_CREW_MAX; size++) {
        radar_layout_dot_result_t resolved[FF_CREW_MAX];
        for (int i = 0; i < size; i++) {
            resolved[i].dx = 0.0f;
            resolved[i].dy = 0.0f;
            resolved[i].cluster_id = 0;
            resolved[i].cluster_size = size;
        }

        radar_layout_wedge_t wedges[FF_CREW_MAX];
        int n = radar_layout_cluster_wedges(resolved, size, 0, wedges, FF_CREW_MAX);
        TEST_ASSERT_EQUAL_INT(size, n);

        float expected_span = (360.0f / (float)size) - RADAR_LAYOUT_CLUSTER_WEDGE_GAP_DEG;
        TEST_ASSERT_TRUE_MESSAGE(expected_span > 0.0f,
                                  "the gap must never consume a whole wedge at any supported cluster size");

        float total = 0.0f;
        for (int k = 0; k < n; k++) {
            float span = wedge_span(&wedges[k]);
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, expected_span, span, "wedges must be equal slices");
            TEST_ASSERT_TRUE_MESSAGE(wedges[k].start_deg >= 0.0f && wedges[k].start_deg < 360.0f,
                                      "start angle must be normalized into [0, 360) for LVGL");
            TEST_ASSERT_TRUE_MESSAGE(wedges[k].end_deg >= 0.0f && wedges[k].end_deg < 360.0f,
                                      "end angle must be normalized into [0, 360) for LVGL");
            total += span;
        }
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, 360.0f - (float)size * RADAR_LAYOUT_CLUSTER_WEDGE_GAP_DEG, total,
                                          "wedges + gaps must account for the entire ring");
    }
}

static void test_cluster_wedges_two_members_split_left_right(void)
{
    /* Two people standing together should split the marker vertically
     * (side by side), not horizontally — the first wedge starts at 12
     * o'clock, which in LVGL's arc convention (0 == 3 o'clock, clockwise)
     * is 270 degrees, offset by half a gap. */
    radar_layout_dot_result_t resolved[2];
    for (int i = 0; i < 2; i++) {
        resolved[i].dx = 0.0f;
        resolved[i].dy = 0.0f;
        resolved[i].cluster_id = 0;
        resolved[i].cluster_size = 2;
    }

    radar_layout_wedge_t wedges[FF_CREW_MAX];
    int n = radar_layout_cluster_wedges(resolved, 2, 0, wedges, FF_CREW_MAX);
    TEST_ASSERT_EQUAL_INT(2, n);

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 270.0f + RADAR_LAYOUT_CLUSTER_WEDGE_GAP_DEG / 2.0f, wedges[0].start_deg);
    /* First wedge runs 12 o'clock -> 6 o'clock (the right half) and so
     * crosses the 0/360 seam: end_deg wraps BELOW start_deg, which is
     * exactly what lv_arc expects and must not be "corrected". */
    TEST_ASSERT_TRUE_MESSAGE(wedges[0].end_deg < wedges[0].start_deg,
                              "the seam-crossing wedge must keep its wrapped form for lv_arc");
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 90.0f - RADAR_LAYOUT_CLUSTER_WEDGE_GAP_DEG / 2.0f, wedges[0].end_deg);
}

static void test_cluster_wedges_single_member_is_a_full_ring(void)
{
    /* Well-defined even though the renderer never takes this path (a
     * lone dot draws its own initial): one member, no gap, whole circle,
     * emitted as LVGL's own {0, 360} full-arc form — a normalized
     * {270, 270} would be indistinguishable from a zero-width wedge, so
     * this is the one case that deliberately isn't normalized. */
    radar_layout_dot_result_t resolved[1] = {{0.0f, 0.0f, 0, 1}};
    radar_layout_wedge_t wedges[FF_CREW_MAX];

    int n = radar_layout_cluster_wedges(resolved, 1, 0, wedges, FF_CREW_MAX);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, wedges[0].start_deg);
    TEST_ASSERT_EQUAL_FLOAT(360.0f, wedges[0].end_deg);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 360.0f, wedge_span(&wedges[0]));
}

static void test_cluster_wedges_guard_paths_return_zero(void)
{
    radar_layout_dot_result_t resolved[2];
    for (int i = 0; i < 2; i++) {
        resolved[i].dx = 0.0f;
        resolved[i].dy = 0.0f;
        resolved[i].cluster_id = 0;
        resolved[i].cluster_size = 2;
    }
    radar_layout_wedge_t wedges[FF_CREW_MAX];

    TEST_ASSERT_EQUAL_INT(0, radar_layout_cluster_wedges(NULL, 2, 0, wedges, FF_CREW_MAX));
    TEST_ASSERT_EQUAL_INT(0, radar_layout_cluster_wedges(resolved, 2, 0, NULL, FF_CREW_MAX));
    TEST_ASSERT_EQUAL_INT(0, radar_layout_cluster_wedges(resolved, 0, 0, wedges, FF_CREW_MAX));
    TEST_ASSERT_EQUAL_INT(0, radar_layout_cluster_wedges(resolved, 2, 0, wedges, 0));
    /* A cluster_id no dot belongs to: nothing to draw, not a wedge of
     * garbage. */
    TEST_ASSERT_EQUAL_INT(0, radar_layout_cluster_wedges(resolved, 2, 7, wedges, FF_CREW_MAX));
}

static void test_cluster_wedges_refuse_rather_than_drop_a_member(void)
{
    /* PR #41 code review: a buffer too small for the whole cluster must
     * REFUSE (-1), not write out_max wedges and report the short count.
     * The short-count form would let this function satisfy its signature
     * by hiding a crew member — the exact lie by omission the clustering
     * ruling exists to prevent, one layer further in. Unreachable from
     * scr_radar.c; the point is that it cannot be reached at all. */
    radar_layout_dot_result_t resolved[4];
    for (int i = 0; i < 4; i++) {
        resolved[i].dx = 0.0f;
        resolved[i].dy = 0.0f;
        resolved[i].cluster_id = 0;
        resolved[i].cluster_size = 4;
    }

    radar_layout_wedge_t wedges[4];
    memset(wedges, 0, sizeof(wedges));
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, radar_layout_cluster_wedges(resolved, 4, 0, wedges, 2),
                                    "a cluster larger than out_max must refuse, not emit a partial ring");
    /* And nothing was written on the refusal path. */
    for (int k = 0; k < 4; k++) {
        TEST_ASSERT_EQUAL_INT(0, wedges[k].index);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, wedges[k].start_deg);
    }

    /* Exactly-fits is not an overflow. */
    TEST_ASSERT_EQUAL_INT(4, radar_layout_cluster_wedges(resolved, 4, 0, wedges, 4));
}

static void test_cluster_wedges_match_a_real_resolve_pass(void)
{
    /* End-to-end against the real resolver rather than a hand-built
     * array: whatever radar_layout_resolve_dots decides the clusters
     * are, the wedge counts must add back up to every original dot —
     * "cluster, never hide" all the way through to the ring. */
    radar_layout_registry_t reg;
    radar_layout_build_registry(RADAR_CLOSE, false, &reg);

    float ring_deg[8] = {180.0f, 181.0f, 182.0f, 183.0f, 0.0f, 1.0f, 90.0f, 270.0f};
    radar_layout_dot_result_t dots[8];
    radar_layout_resolve_dots(&reg, ring_deg, 8, dots);

    int total = 0;
    for (int i = 0; i < 8; i++) {
        if (dots[i].cluster_id != i) {
            continue; /* only the anchor draws a marker */
        }
        radar_layout_wedge_t wedges[FF_CREW_MAX];
        int n = radar_layout_cluster_wedges(dots, 8, i, wedges, FF_CREW_MAX);
        TEST_ASSERT_EQUAL_INT_MESSAGE(dots[i].cluster_size, n,
                                        "a marker's wedge count must equal its cluster_size");
        total += n;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(8, total, "every original dot must own exactly one wedge somewhere");
}

/* ---------------------------------------------------------------------
 * AC — termination. The resolver's loops are bounded by construction
 * (fixed step sizes, fixed iteration caps — see radar_layout.c); running
 * every sweep above to completion (25,200 dot resolutions + 10,800 arrow
 * resolutions) within one fast test binary run is itself empirical proof
 * none of them hang, on top of the static bound.
 * ------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_dot_sweep_live);
    RUN_TEST(test_dot_sweep_stale);
    RUN_TEST(test_dot_sweep_lost_real_fix);
    RUN_TEST(test_dot_sweep_lost_never_fixed);
    RUN_TEST(test_dot_sweep_close);
    RUN_TEST(test_dot_sweep_nofix);
    RUN_TEST(test_dot_sweep_nosel);

    RUN_TEST(test_arrow_sweep_live);
    RUN_TEST(test_arrow_sweep_stale);
    RUN_TEST(test_arrow_sweep_lost);
    RUN_TEST(test_arrow_not_shortened_when_clear);

    RUN_TEST(test_all_8_dots_same_bearing_close_mode_cluster_not_hidden);
    RUN_TEST(test_widely_spaced_dots_stay_distinct_not_clustered);

    RUN_TEST(test_cluster_wedges_one_per_member_in_index_order);
    RUN_TEST(test_cluster_wedges_only_include_that_cluster);
    RUN_TEST(test_cluster_wedges_are_equal_gapped_and_cover_the_ring);
    RUN_TEST(test_cluster_wedges_two_members_split_left_right);
    RUN_TEST(test_cluster_wedges_single_member_is_a_full_ring);
    RUN_TEST(test_cluster_wedges_guard_paths_return_zero);
    RUN_TEST(test_cluster_wedges_refuse_rather_than_drop_a_member);
    RUN_TEST(test_cluster_wedges_match_a_real_resolve_pass);

    return UNITY_END();
}
