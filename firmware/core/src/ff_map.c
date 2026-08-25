/**
 * ff_map.c — see ff_map.h for the contract and the reasoning behind the
 * inscribed-square fit and the degenerate-bbox fallback.
 */
#include "ff_map.h"

#include <stdbool.h>
#include <stddef.h> /* NULL */

/* sqrt(2), as a named literal rather than <math.h>'s M_SQRT2 — M_SQRT2
 * is a BSD/POSIX math.h extension, not standard C11, and this repo
 * builds under -std=c11 on both clang (macOS) and gcc-14 (CI): relying
 * on it risks a feature-test-macro fight on the strict-C11 gcc side (see
 * CLAUDE.md's "CI's GCC is the authority" note). A literal is exact
 * enough at float precision either way. */
#define FF_MAP_SQRT2 1.41421356237f

/* The spec's own documented fallback: "1 km square around origin" —
 * applied both to zero features and to a degenerate (single-point)
 * bbox. Half-span, so the full square is 2x this on each axis. */
#define FF_MAP_FALLBACK_HALF_SPAN_M 500.0f

/* Below this, a bbox's width/height is treated as zero for fit purposes
 * — float noise, not a real spread. Meters-scale, so 1 mm is generous
 * headroom over any float rounding this module could produce. */
#define FF_MAP_DEGENERATE_EPS_M 0.001f

void ff_map_xform_fit(ff_map_xform_t *out, float const pts_en[][2], int n, float radius_px, float margin_px)
{
    if (out == NULL) {
        return;
    }
    if (n < 0) {
        n = 0;
    }

    float min_e = 0.0f, max_e = 0.0f, min_n = 0.0f, max_n = 0.0f;
    bool have = false;

    for (int i = 0; i < n; i++) {
        float const e = pts_en[i][0];
        float const nn = pts_en[i][1];
        if (!have) {
            min_e = max_e = e;
            min_n = max_n = nn;
            have = true;
        } else {
            if (e < min_e) min_e = e;
            if (e > max_e) max_e = e;
            if (nn < min_n) min_n = nn;
            if (nn > max_n) max_n = nn;
        }
    }

    float w, h, cx, cy;
    if (!have) {
        /* No features at all (S09 AC3's untraced-pack case): fall back
         * to the spec's 1km square, centered on the origin — there is no
         * real point to center on. */
        cx = 0.0f;
        cy = 0.0f;
        w = h = FF_MAP_FALLBACK_HALF_SPAN_M * 2.0f;
    } else {
        w = max_e - min_e;
        h = max_n - min_n;
        cx = (min_e + max_e) * 0.5f;
        cy = (min_n + max_n) * 0.5f;
        if (w < FF_MAP_DEGENERATE_EPS_M && h < FF_MAP_DEGENERATE_EPS_M) {
            /* AC1's "degenerate single-point bbox handled": every point
             * coincides. Same 1km fallback square, but centered on the
             * one real point (cx, cy already computed above) rather than
             * silently re-centering on the origin — this is a real
             * feature that happens to be alone, not an empty pack. */
            w = h = FF_MAP_FALLBACK_HALF_SPAN_M * 2.0f;
        }
    }

    float const usable_r = radius_px - margin_px;
    float const span = (w > h) ? w : h;
    /* Fit the bbox's longer side to the side of the SQUARE inscribed in
     * the usable circle (see ff_map.h's doc comment for why sqrt(2), not
     * a bare diameter fit, is the only choice that keeps every bbox
     * point — including a corner point — inside the circle). */
    out->scale_px_per_m = (span > 0.0f) ? (usable_r * FF_MAP_SQRT2) / span : 0.0f;
    out->center_east_m = cx;
    out->center_north_m = cy;
}

void ff_map_project(ff_map_xform_t const *x, float east_m, float north_m, float *out_x_px, float *out_y_px)
{
    if (x == NULL) {
        if (out_x_px != NULL) *out_x_px = 0.0f;
        if (out_y_px != NULL) *out_y_px = 0.0f;
        return;
    }

    float const dx = (east_m - x->center_east_m) * x->scale_px_per_m;
    float const dy = (north_m - x->center_north_m) * x->scale_px_per_m;

    if (out_x_px != NULL) *out_x_px = dx;
    /* North-up: north increases UPWARD on screen, so y DECREASES as
     * north_m increases (screen y grows downward). */
    if (out_y_px != NULL) *out_y_px = -dy;
}

ff_map_render_kind_t ff_map_feature_render_kind(uint8_t n_pts, int is_stage)
{
    if (n_pts == 0) return FF_MAP_RENDER_OMIT;
    if (n_pts == 1) return is_stage ? FF_MAP_RENDER_STAGE_STUB : FF_MAP_RENDER_LABEL_ONLY;
    if (n_pts == 2) return FF_MAP_RENDER_LINE;
    return FF_MAP_RENDER_POLYGON;
}

ff_map_label_priority_t ff_map_feature_label_priority(uint8_t n_pts, int is_stage)
{
    ff_map_render_kind_t const rk = ff_map_feature_render_kind(n_pts, is_stage);
    if (rk == FF_MAP_RENDER_POLYGON && !is_stage) return FF_MAP_LABEL_PRIORITY_LOW;
    return FF_MAP_LABEL_PRIORITY_HIGH;
}

/* ---------------------------------------------------------------------
 * ff_map_place_labels — see ff_map.h's doc comment for the full
 * contract (caller-ordering requirement, HIGH-nudge/LOW-drop rules).
 * ------------------------------------------------------------------- */

int ff_map_place_labels(ff_map_label_request_t const *in, int n, float min_sep_px, int max_nudge_tries,
                         ff_map_label_result_t *out)
{
    if (n < 0 || n > FF_MAP_LABEL_MAX_ITEMS) return -1;
    if (n > 0 && (in == NULL || out == NULL)) return -1; /* n == 0 needs neither pointer: nothing to dereference */

    for (int i = 0; i < n; i++) {
        float x = in[i].x;
        float y = in[i].y;

        /* Collision test against every EARLIER result that was actually
         * placed — dropped LOW entries contribute nothing, matching
         * ff_map.h's documented rule ("contributes nothing to later
         * entries' collision checks"). */
        if (in[i].priority == FF_MAP_LABEL_PRIORITY_HIGH) {
            for (int attempt = 0; attempt < max_nudge_tries; attempt++) {
                bool collides = false;
                for (int j = 0; j < i; j++) {
                    if (!out[j].placed) continue;
                    float const dx = x - out[j].x;
                    float const dy = y - out[j].y;
                    if (dx * dx + dy * dy < min_sep_px * min_sep_px) {
                        collides = true;
                        break;
                    }
                }
                if (!collides) break;
                y += min_sep_px;
            }
            out[i].placed = true;
            out[i].x = x;
            out[i].y = y;
        } else {
            bool collides = false;
            for (int j = 0; j < i; j++) {
                if (!out[j].placed) continue;
                float const dx = x - out[j].x;
                float const dy = y - out[j].y;
                if (dx * dx + dy * dy < min_sep_px * min_sep_px) {
                    collides = true;
                    break;
                }
            }
            out[i].placed = !collides;
            out[i].x = x;
            out[i].y = y;
        }
    }

    return n;
}

/* ---------------------------------------------------------------------
 * ff_map_triangulate — ear clipping. See ff_map.h's doc comment for the
 * full contract; this is the standard textbook algorithm (repeatedly
 * clip a convex vertex none of the remaining polygon covers), bounded so
 * it always terminates even on input it can't cleanly handle.
 * ------------------------------------------------------------------- */

#define FF_MAP_TRIANGULATE_MAX_PTS 32

static float ff_map_cross2(float ax, float ay, float bx, float by)
{
    return ax * by - ay * bx;
}

/* Non-strict (boundary-inclusive) point-in-triangle test via same-sign
 * barycentric cross products — deliberately conservative: a polygon
 * vertex sitting exactly ON one of the candidate ear's edges (common for
 * real, axis-hugging festival geometry) counts as "inside" and
 * disqualifies the ear, rather than risking a triangle that clips a
 * real vertex. That can only ever make this function reject a valid ear
 * (never accept an invalid one), which the bounded relaxed-fallback pass
 * below still recovers from without producing wrong geometry. */
static int ff_map_point_in_or_on_triangle(float px, float py, float ax, float ay, float bx, float by, float cx,
                                           float cy)
{
    float const d1 = ff_map_cross2(bx - ax, by - ay, px - ax, py - ay);
    float const d2 = ff_map_cross2(cx - bx, cy - by, px - bx, py - by);
    float const d3 = ff_map_cross2(ax - cx, ay - cy, px - cx, py - cy);
    int const has_neg = (d1 < 0.0f) || (d2 < 0.0f) || (d3 < 0.0f);
    int const has_pos = (d1 > 0.0f) || (d2 > 0.0f) || (d3 > 0.0f);
    return !(has_neg && has_pos);
}

/* One ear-search pass over the current working set `idx[0..m)`.
 * `strict`: also reject a convex candidate if any OTHER working vertex
 * lies inside its triangle (the real ear-clipping test). When strict
 * finds nothing (a bounded, defensive fallback for otherwise-stuck
 * input — see ff_map.h's doc comment), a second, relaxed pass with
 * `strict == 0` accepts the first merely-convex candidate instead of
 * giving up, which guarantees termination without ever emitting a
 * triangle for input that has no convex vertex at all (impossible for
 * any real simple polygon, which always has at least 3). Returns the
 * clipped vertex's position in `idx` (so the caller can remove it), or
 * -1 if this pass found nothing. `out_ia`/`out_ib`/`out_ic` receive the
 * clipped triangle's point INDICES (into the original `pts` array). */
static int ff_map_find_ear(float const pts[][2], uint8_t const *idx, int m, int ccw, int strict, uint8_t *out_ia,
                            uint8_t *out_ib, uint8_t *out_ic)
{
    for (int k = 0; k < m; k++) {
        int const kp = (k + m - 1) % m;
        int const kn = (k + 1) % m;
        uint8_t const ia = idx[kp], ib = idx[k], ic = idx[kn];
        float const ax = pts[ia][0], ay = pts[ia][1];
        float const bx = pts[ib][0], by = pts[ib][1];
        float const cx = pts[ic][0], cy = pts[ic][1];

        float const turn = ff_map_cross2(bx - ax, by - ay, cx - bx, cy - by);
        int const convex = ccw ? (turn > 0.0f) : (turn < 0.0f);
        if (!convex) continue;

        if (strict) {
            int any_inside = 0;
            for (int t = 0; t < m; t++) {
                if (t == kp || t == k || t == kn) continue;
                uint8_t const ip = idx[t];
                if (ff_map_point_in_or_on_triangle(pts[ip][0], pts[ip][1], ax, ay, bx, by, cx, cy)) {
                    any_inside = 1;
                    break;
                }
            }
            if (any_inside) continue;
        }

        *out_ia = ia;
        *out_ib = ib;
        *out_ic = ic;
        return k;
    }
    return -1;
}

int ff_map_triangulate(float const pts[][2], int n, uint8_t out_tris[][3], int out_max)
{
    if (pts == NULL || out_tris == NULL || n < 3 || n > FF_MAP_TRIANGULATE_MAX_PTS) return -1;
    if (out_max < n - 2) return -1;

    /* Signed area (shoelace, doubled) — its sign is the polygon's overall
     * winding, which every convexity test below is measured against.
     * Zero means degenerate (collinear/duplicate points): no honest
     * triangulation exists. */
    float area2 = 0.0f;
    for (int i = 0; i < n; i++) {
        int const j = (i + 1) % n;
        area2 += pts[i][0] * pts[j][1] - pts[j][0] * pts[i][1];
    }
    if (area2 == 0.0f) return -1;
    int const ccw = area2 > 0.0f;

    uint8_t idx[FF_MAP_TRIANGULATE_MAX_PTS];
    for (int i = 0; i < n; i++) idx[i] = (uint8_t)i;
    int m = n;
    int out_n = 0;

    /* Bounded: each successful clip shrinks m by 1 (n-2 clips total), and
     * each search pass is O(m) — n^2 is a generous cap that can never be
     * reached by a correctly-terminating run, only by truly stuck input,
     * which this bound then fails out of rather than looping forever. */
    int guard = 0;
    int const guard_max = n * n + 8;

    while (m > 3) {
        if (guard++ > guard_max) return -1;

        uint8_t ia, ib, ic;
        int k = ff_map_find_ear(pts, idx, m, ccw, 1, &ia, &ib, &ic);
        if (k < 0) {
            /* No STRICT ear anywhere in the current working set — cannot
             * happen for a valid simple polygon, but guarded per
             * ff_map.h's doc comment. Relax once: take the first merely-
             * convex vertex instead of getting stuck. */
            k = ff_map_find_ear(pts, idx, m, ccw, 0, &ia, &ib, &ic);
            if (k < 0) return -1; /* no convex vertex at all: truly degenerate */
        }

        out_tris[out_n][0] = ia;
        out_tris[out_n][1] = ib;
        out_tris[out_n][2] = ic;
        out_n++;

        for (int t = k; t < m - 1; t++) idx[t] = idx[t + 1];
        m--;
    }

    /* Final triangle: whatever three vertices remain. */
    out_tris[out_n][0] = idx[0];
    out_tris[out_n][1] = idx[1];
    out_tris[out_n][2] = idx[2];
    out_n++;

    return out_n;
}
