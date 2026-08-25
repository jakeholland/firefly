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

#include <stdbool.h>
#include <stdint.h>

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

/**
 * ff_map_clip_point_to_circle — issue #75's actual fix: radially clamps a
 * CENTER-RELATIVE point `(x, y)` to lie within `radius_px` of the origin,
 * scaling it toward the origin along the SAME direction if it's already
 * outside, leaving it untouched otherwise. `radius_px <= 0` is treated as
 * "no valid circle" and passes the point through unchanged (defensive,
 * never fabricates a position) — same posture as `ff_map_project`'s
 * `x == NULL` case. Either output pointer may be NULL if the caller only
 * wants the other axis.
 *
 * ## Why this, not `lv_obj_set_style_clip_corner` (issue #75)
 * The camera fit (`ff_map_xform_fit`) only guarantees feature ANCHOR
 * points stay inside the circle (see this header's own doc comment on
 * that function) — a large boundary polygon's OWN vertices can
 * legitimately project outside it. The obvious fix is an LVGL pixel clip
 * on the puck's children, but `lv_obj_set_style_clip_corner` reliably
 * HANGS `ffsim --headless` at this face's draw-object count (up to ~300
 * full-puck-sized triangle/segment proxy objects — scr_map.c's own header
 * comment has the full count/hypothesis). This function moves the fix
 * into PURE GEOMETRY instead, at the one point every callsite already has
 * to touch anyway (right after `ff_map_project`): clamp each projected
 * vertex/endpoint to the circle BEFORE handing it to LVGL, so nothing
 * ever asks LVGL to draw a pixel outside the circle in the first place —
 * no clip pass, no hang, and (since a disk is convex) clamping every
 * vertex of a polygon or line independently is provably sufficient: the
 * straight edge between any two clamped-or-already-inside points can
 * never bulge back outside a convex region. `scr_map.c` applies this to
 * every projected polygon/line vertex at `FF_MAP_CIRCLE_RADIUS_PX` — see
 * that file's header comment for the caller-side rationale and issue #75
 * for the investigation this fix is based on.
 *
 * Pure C11 math (sqrtf) — no allocation, no I/O, same shape as every
 * other function in this module.
 */
void ff_map_clip_point_to_circle(float x, float y, float radius_px, float *out_x, float *out_y);

/**
 * ff_map_feature_render_kind_t — the untraced-feature render POLICY
 * decision (S09 spec's Amendments: "n_pts 0/1/2/>=3"), as a pure
 * function of `n_pts` and whether the feature is a stage — not
 * "recomputed inline in scr_map.c" (CLAUDE.md: "if you're writing an
 * `if` about domain behavior inside a screen file, it belongs in
 * core"), and independently unit-testable without LVGL (PR #73 review
 * finding #6 — this decision used to be reachable only through a loose
 * pixel-diff golden threshold, which a mutation on the `n_pts==0`
 * early-out slipped underneath).
 */
typedef enum {
    FF_MAP_RENDER_OMIT,       /* n_pts == 0: no polygon, no point — nothing to honestly draw or label */
    FF_MAP_RENDER_LABEL_ONLY, /* n_pts == 1, not a stage: a label anchored at the one point, no shape */
    FF_MAP_RENDER_STAGE_STUB, /* n_pts == 1, a stage: the spec's named labeled 30m stub circle */
    FF_MAP_RENDER_LINE,       /* n_pts == 2: a stroked line between the two points, no fill */
    FF_MAP_RENDER_POLYGON,    /* n_pts >= 3: the normal filled+stroked polygon */
} ff_map_render_kind_t;

/** ff_map_feature_render_kind — see `ff_map_render_kind_t`'s doc comment. */
ff_map_render_kind_t ff_map_feature_render_kind(uint8_t n_pts, int is_stage);

/**
 * ff_map_label_priority_t — the LABEL priority tier a feature's text
 * gets when two labels would collide (PR #73's second review round,
 * coordinator ruling: the anchor-point camera fit alone fixed the
 * geometry SCALE but not real-pack label crowding — "Wompy Woods
 * treeline" overprinting "RV/tent camping", stage names buried under
 * area-polygon labels). Distinct from `ff_map_render_kind_t`, which
 * decides whether a SHAPE draws at all: every feature's shape (stub/
 * line/polygon) still draws unconditionally regardless of this tier —
 * only its TEXT can be dropped, and only for the LOW tier, and only
 * when it would otherwise collide.
 *
 * `FF_MAP_LABEL_PRIORITY_HIGH` — never dropped: a STAGE (any render
 * form — a stub today, but a future traced stage polygon must stay
 * labeled too, since "the stage labels... the things people actually
 * navigate by" is the whole point), or any single-point non-polygon
 * feature (`FF_MAP_RENDER_STAGE_STUB`/`_LABEL_ONLY`/`_LINE`) — dropping
 * one of THOSE would erase the feature entirely, since it has no other
 * visual representation to fall back on.
 *
 * `FF_MAP_LABEL_PRIORITY_LOW` — droppable: a non-stage feature whose
 * render kind is `FF_MAP_RENDER_POLYGON` (a real traced area/boundary
 * shape — venue extent, a treeline, a campground). The polygon itself
 * still draws either way; dropping only its TEXT is honest (the shape
 * carries the meaning) precisely because — unlike the HIGH tier — the
 * feature remains visually represented with its label gone.
 */
typedef enum {
    FF_MAP_LABEL_PRIORITY_HIGH,
    FF_MAP_LABEL_PRIORITY_LOW,
} ff_map_label_priority_t;

/** ff_map_feature_label_priority — see `ff_map_label_priority_t`'s doc
 * comment. Same two inputs as `ff_map_feature_render_kind` (this is a
 * refinement of that decision, not an independent one) — meaningless
 * for `FF_MAP_RENDER_OMIT` (n_pts == 0), which never reaches the label
 * placement pass at all. */
ff_map_label_priority_t ff_map_feature_label_priority(uint8_t n_pts, int is_stage);

/**
 * ff_map_place_labels — the WHOLE priority-based label collision
 * resolution algorithm, pulled out of `scr_map.c` and into core (PR #73
 * THIRD review round, non-blocking finding #2: the round-2 mechanism
 * lived only as static state + LVGL-adjacent calls in `scr_map.c`, so
 * the only way to prove "LOW labels actually get dropped on collision"
 * was a golden pixel-diff — which the round's own mutation test showed
 * absorbs a full regression of this PR's headline fix (780/207936px,
 * comfortably under the 0.5% threshold). This function is the same
 * "pull the geometry into core, unit-test it directly against real
 * coordinates" fix `ff_map_triangulate` already got for finding #2).
 *
 * `in[0..n)` MUST be ordered with every `FF_MAP_LABEL_PRIORITY_HIGH`
 * entry before every `FF_MAP_LABEL_PRIORITY_LOW` one — this function
 * does not sort or reorder. That is a caller contract, not a runtime
 * check: mixing tiers out of order silently defeats the "stages/
 * landmarks/YOU always win" guarantee this whole mechanism exists for,
 * because a LOW item processed before some HIGH item can't yet see it
 * as occupied space. `scr_map.c`'s own doc comment on its single call
 * site states how it satisfies this.
 *
 * For each `in[i]` in order:
 *   - `FF_MAP_LABEL_PRIORITY_HIGH`: nudges `y` downward (deterministic,
 *     bounded by `max_nudge_tries`) until it clears every
 *     ALREADY-PLACED result (from earlier `in[]` entries) by
 *     `min_sep_px`, or the try budget runs out. ALWAYS placed —
 *     `out[i].placed` is unconditionally true. If, after nudging, the
 *     label's own bounds (see `half_w`/`half_h` below) would still cross
 *     `circle_radius_px`, the position is pulled radially inward via
 *     `ff_map_clip_point_to_circle` instead of dropped — a HIGH label can
 *     never be dropped (it has no other visual representation), so the
 *     honest response to "too far out" is "move it in", not "hide it".
 *   - `FF_MAP_LABEL_PRIORITY_LOW`: checked once, at its original
 *     position, against every already-placed result AND against
 *     `circle_radius_px` (issue #77's "ultra-long labels run off the
 *     circle edge uncropped" — see below). Either kind of collision means
 *     `out[i].placed = false` (dropped, NOT nudged) and it contributes
 *     nothing to later entries' collision checks; otherwise placed at
 *     its original position.
 *
 * ## Circle-bounds rejection (issue #77's "ultra-long labels run off the
 * circle edge uncropped", folded in here rather than left as a
 * standalone follow-up — see scr_map.c's own header comment for why this
 * is the natural home for it)
 * `in[i].half_w`/`half_h` are the label's own approximate half-width/
 * half-height in px (the caller's font/text measurement — this module
 * stays LVGL-free, so it takes the measurement as input rather than
 * computing it) — together with `(x, y)` they describe the same centered
 * bounding box `lv_obj_align(..., LV_ALIGN_CENTER, x, y)` would produce.
 * A conservative (never under-claims containment) corner-distance check
 * against `circle_radius_px` decides whether that box is honestly inside
 * the circle; `circle_radius_px <= 0` disables this check entirely
 * (existing callers that don't care about circle containment pass 0).
 *
 * `out[0..n)` mirrors `in[]` index-for-index — `out[i]` is always the
 * answer for `in[i]`, whether placed or dropped, so callers can recover
 * which SOURCE item (feature/YOU/rally) each result belongs to via the
 * same index they used to build `in[]`.
 *
 * Returns `n` on success (including `n == 0`, a safe no-op — `in`/`out`
 * may both be NULL in that case, since nothing is ever dereferenced), or
 * -1 for `n < 0`, `n > FF_MAP_LABEL_MAX_ITEMS`, or (whenever `n > 0`)
 * either pointer NULL — never a partial result.
 */
#define FF_MAP_LABEL_MAX_ITEMS 24

typedef struct {
    float x, y; /* desired anchor position, px, center-relative */
    ff_map_label_priority_t priority;
    /* Approximate half-width/half-height of the label's own text, px —
     * see ff_map_place_labels's doc comment on the circle-bounds check.
     * Appended at the end (not inserted between existing fields) so
     * every pre-existing `{x, y, priority}` positional initializer in
     * this codebase still compiles, zero-initializing both to 0 — which
     * is exactly "this label has no width" and therefore never trips the
     * bounds check on its own, a safe default for a caller that hasn't
     * been updated to supply a real measurement yet. */
    float half_w, half_h;
} ff_map_label_request_t;

typedef struct {
    bool placed; /* false: dropped (LOW priority, collided or off-circle) */
    float x, y;  /* final position — may be nudged/clamped from the request; meaningless if !placed */
} ff_map_label_result_t;

int ff_map_place_labels(ff_map_label_request_t const *in, int n, float min_sep_px, int max_nudge_tries,
                         float circle_radius_px, ff_map_label_result_t *out);

/**
 * ff_map_triangulate — ear-clipping triangulation of a SIMPLE polygon
 * (`n` vertices, `n >= 3`) — correct for CONVEX or CONCAVE input, as
 * long as it doesn't self-intersect. Fixes PR #73 review finding #2: the
 * previous vertex-0 fan-triangulation this replaces is only correct for
 * convex/star-shaped input, and the real, currently-merged Lost Lands
 * pack's "Venue extent" feature (9 points) is concave — the fan mis-fills
 * it, bleeding outside the true boundary at the reflex vertex.
 *
 * Writes up to `n - 2` triangles as VERTEX-INDEX triples (each entry
 * indexes into `pts`) to `out_tris[0 .. return value)`, one triple per
 * triangle. `out_max` must be at least `n - 2` or the call fails
 * (returns -1) rather than writing a partial/truncated triangulation —
 * same "never silently drop geometry" contract as this module's other
 * functions.
 *
 * Returns the number of triangles written (always exactly `n - 2` for
 * well-formed simple-polygon input), or -1 for:
 *  - `n < 3`, `pts == NULL`, `out_tris == NULL`, or `out_max < n - 2`;
 *  - `n` exceeding this function's bounded internal working set (32 —
 *    comfortably above any cap this codebase's callers use);
 *  - a degenerate input (zero polygon area — collinear/duplicate points);
 *  - input pathological enough (self-intersecting) that no valid ear can
 *    be found in a bounded search — this cannot happen for a real,
 *    non-self-intersecting festival footprint, but is guarded rather
 *    than looped on, so a malformed pack can never hang the renderer.
 *
 * Callers get a clean binary contract: a non-negative return is a
 * complete, correct triangulation of the ENTIRE input polygon; a
 * negative return means "do not trust this input for a fill" — the
 * documented, spec-sanctioned fallback (S09-map-face.md's Amendments) is
 * to draw the polygon's OUTLINE only, never a wrong fill (a mis-filled
 * shape is worse than an unfilled one).
 *
 * Pure geometry: no allocation, no I/O, bounded iteration (a fixed
 * O(n^2) worst-case ear search, `n <= 32`).
 */
int ff_map_triangulate(float const pts[][2], int n, uint8_t out_tris[][3], int out_max);

#ifdef __cplusplus
}
#endif

#endif /* FF_MAP_H */
