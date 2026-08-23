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

    return UNITY_END();
}
