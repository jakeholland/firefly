/**
 * ff_map.h — core/map: the S09 map-face fixed-fit camera transform.
 *
 * Spec: docs/specs/S09-map-face.md
 *
 * Pure C11, `math.h` only (sqrtf), no allocation, no I/O — same shape as
 * every other core/ module (CLAUDE.md: "all logic goes in firmware/core/").
 * `ff_map_xform_t` is the ONE piece of geometry the Map face computes:
 * bounding-box-of-features -> screen px, single scale + offset, shared by
 * every movable thing scr_map.c draws (features, crew dots, the rally
 * pin, the YOU marker) so none of them can honestly disagree about where
 * "here" is on screen (S09 spec: "single scale+offset; crew/rally/YOU
 * share it").
 */
#ifndef FF_MAP_H
#define FF_MAP_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_map_xform_t — the fitted camera: a uniform (aspect-preserving)
 * meters-to-pixels scale, plus the meters-space point that maps to the
 * circle's own center. Screen coordinates this produces (via
 * `ff_map_project`) are CENTER-RELATIVE — circle center = (0, 0) — the
 * same convention `app/screens/radar_layout.h` already documents ("puck
 * center = (0,0)"), so scr_map.c can reuse the exact
 * half-puck-plus-offset positioning trick scr_radar.c's draw helpers use.
 */
typedef struct {
    float scale_px_per_m;  /* meters -> px, uniform on both axes (aspect preserved) */
    float center_east_m;   /* the bbox-center meters point that maps to (0,0) px */
    float center_north_m;
} ff_map_xform_t;

/**
 * ff_map_xform_fit — the S09 "fixed-fit v1" camera: fit the bounding box
 * of `pts_en[0..n)` (each `{east_m, north_m}`, S05's local flat
 * projection — see `ff_geo_project`) into a circle of `radius_px` with
 * `margin_px` of clearance, north-up, aspect preserved.
 *
 * ## Why the fit uses the circle's INSCRIBED SQUARE, not its bare diameter
 * The naive approach — scale so the bbox's longer side spans
 * `2*(radius_px-margin_px)` — only guarantees the bbox's flat EDGES stay
 * inside the circle. A real festival footprint can have a feature at (or
 * near) a bbox CORNER — simultaneously at the east extreme and the north
 * extreme — and for any non-square bbox that point lands OUTSIDE the
 * circle under the naive fit, which is exactly the "all points inside
 * circle radius - margin" guarantee S09 AC1 requires. Scaling instead so
 * the bbox's longer side spans the side of the SQUARE inscribed in the
 * usable circle (`(radius_px-margin_px) * sqrt(2)`) is the unique choice
 * that keeps every point of ANY axis-aligned bbox — including its
 * corners — inside the circle, with equality only in the worst case (a
 * square bbox with a feature at each corner). The cost is a modest amount
 * of unused margin for non-square bboxes (the common case), which is the
 * honest trade against ever letting a real feature clip outside the
 * round glass.
 *
 * ## Degenerate / empty input (AC1's "degenerate single-point bbox
 * handled", and the spec's own documented fallback)
 * - `n == 0` (no features at all — e.g. an all-untraced festpack, S09
 *   AC3's `map_untraced.json`): falls back to the spec's "1 km square
 *   around origin" — a bbox of [-500, 500] m on both axes, centered on
 *   (0, 0).
 * - `n >= 1` but every point coincides (a genuine single feature point,
 *   or several points that all happen to land on the same coordinate —
 *   width and height both under a millimeter-scale epsilon): the SAME 1
 *   km fallback square is used, centered on that one real point instead
 *   of silently re-centering on the origin. This is what keeps
 *   `scale_px_per_m` finite (never a divide-by-zero, never NaN/Inf) for
 *   a legitimately degenerate bbox, which the "no features" fallback
 *   alone would not cover (that branch is for zero points, not one).
 *
 * `out` is left untouched if NULL. `n < 0` is treated as `0`.
 */
void ff_map_xform_fit(ff_map_xform_t *out, float const pts_en[][2], int n, float radius_px, float margin_px);

/**
 * ff_map_project — `{east_m, north_m}` -> center-relative screen px under
 * `x` (see `ff_map_xform_t`'s doc comment for the coordinate convention).
 *
 * North-up: north increases UPWARD on screen, i.e. `*out_y_px` DECREASES
 * as `north_m` increases (screen y grows downward, same convention every
 * other center-relative element in this codebase uses).
 *
 * `x == NULL` writes `(0, 0)` to whichever of `out_x_px`/`out_y_px` are
 * non-NULL — the least-claiming answer, never a fabricated position.
 * Either output pointer may be NULL if the caller only wants the other
 * axis.
 */
void ff_map_project(ff_map_xform_t const *x, float east_m, float north_m, float *out_x_px, float *out_y_px);

#ifdef __cplusplus
}
#endif

#endif /* FF_MAP_H */
