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
