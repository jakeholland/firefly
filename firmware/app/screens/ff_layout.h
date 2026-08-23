/**
 * ff_layout.h — shared circle-fit geometry for round-glass face layouts.
 *
 * PR #25 UX review (blocking finding): `scr_compose.c` positioned its
 * header/footer chrome against the puck's SQUARE bounding box
 * (`LV_ALIGN_TOP_LEFT`/`TOP_RIGHT` with plain pixel offsets) instead of
 * the round glass those corners don't actually cover — tens of pixels of
 * a real control (the back button, the mode chip the S08 Amendments
 * ruling requires always be legible) ended up off-glass, unreachable on
 * real hardware. `radar_layout.h` already solved this problem for the
 * Radar face with a full search-based collision resolver; this header is
 * NOT that — it's the much smaller, face-agnostic primitive every face
 * needs underneath a resolver like that (or, for simpler chrome that
 * doesn't need collision search, on its own): "is this rect inside the
 * circle" and "how wide can I place something at this height". Extracted
 * here specifically so the next face (Now, Flare UI — see issue #24's
 * "each will add its own copies" warning, which was about *rendering*
 * widget duplication; this is the same lesson one level down, at the
 * *geometry* layer) reaches for this instead of re-deriving it or
 * skipping it.
 *
 * Deliberately NOT part of the issue #24 widget-library extraction: that
 * follow-up (explicitly scoped to LVGL widget builders — name labels,
 * chips) is intentionally deferred until the in-flight face PRs land, to
 * avoid three-way merge conflicts. This header is pure C11 math with zero
 * LVGL dependency (same "extraction-grade, unit-testable without a
 * display" shape as `radar_layout.h`) — a different kind of sharing that
 * doesn't conflict with that deferral, and is needed now precisely
 * because the bug it prevents already shipped once.
 *
 * No LVGL dependency whatsoever — plain C11 + <stdbool.h> — so it's
 * unit-testable directly (app/screens/tests/test_ff_layout.c) and usable
 * from a pure-geometry module like a future face-specific layout resolver
 * without pulling LVGL into that module's dependency graph.
 */
#ifndef FF_LAYOUT_H
#define FF_LAYOUT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** An axis-aligned rectangle in any consistent coordinate space the
 * caller chooses (center-relative, puck-local, or absolute display
 * pixels) — the functions below only require that the rect and
 * (cx, cy, radius) are expressed in that same space. `x2`/`y2` are the
 * EXCLUSIVE far edge (i.e. `x2 - x1` is the width), matching this
 * codebase's general "size = far - near" convention (NOT LVGL's own
 * `lv_area_t`, whose `x2`/`y2` are inclusive last-pixel coordinates —
 * callers converting from an `lv_area_t` must add 1 to `x2`/`y2` first;
 * see test_face_hit_targets.c for a worked example). */
typedef struct {
    float x1, y1, x2, y2;
} ff_layout_rect_t;

/**
 * ff_layout_rect_in_circle — true iff every corner of `rect` lies within
 * (or exactly on) a circle centered at `(cx, cy)` with radius `radius`.
 *
 * Checking all four corners (not just the rect's own bounding-circle, or
 * only its center point) is both required and sufficient for an
 * axis-aligned rectangle: the point of an axis-aligned rect farthest from
 * any given circle center is always one of its four corners (the other
 * boundary points — edge midpoints, etc. — are never farther), so a
 * per-corner check is exact, not an approximation.
 */
bool ff_layout_rect_in_circle(ff_layout_rect_t rect, float cx, float cy, float radius);

/**
 * ff_layout_chord_half_width — the maximum |dx| from the circle's own
 * center-x at which a point on a horizontal line `dy` away from
 * center-y still lies inside a circle of `radius` (0 if `|dy| >= radius`,
 * i.e. that horizontal line misses the circle, or touches it at a single
 * point, entirely). This is the "how wide can I place something at this
 * height" query a face's layout code needs BEFORE it decides where to
 * put a header/footer element — the primitive the S08 UX-review ruling
 * asked for so a layout can be sized to fit the glass by construction,
 * rather than built rectangle-first and only checked (or not checked at
 * all, which is exactly how the Compose bug shipped) after the fact.
 *
 * `radius` is expected non-negative; a negative `radius` returns 0 (no
 * valid circle, so no width fits), same as `|dy| >= radius`.
 */
float ff_layout_chord_half_width(float dy, float radius);

/**
 * ff_layout_safe_margin_x — the symmetric left/right margin (measured
 * from x=0 of a `2*center`-wide content area) that keeps ANY element
 * spanning the vertical band `[top_y, top_y + h)` entirely inside a
 * circle of `radius` centered at `(center, center)` — i.e. a circle
 * inscribed in that `2*center`-wide square, the convention every face in
 * this app's puck uses (origin at the puck's own top-left corner, center
 * at `(FF_THEME_PUCK_RADIUS_PX, FF_THEME_PUCK_RADIUS_PX)`, since the
 * puck IS that square with the circle drawn to exactly fill it) — with
 * `safety_px` extra px of slack subtracted from the raw chord width.
 * Never negative (clamped to 0: a band entirely within `safety_px` of
 * the circle's own vertical center needs no margin at all).
 *
 * This is the "how much does THIS row need to be inset" query a face's
 * layout code asks far more often than the raw chord half-width alone —
 * both `app/screens/scr_compose.c` and `scr_signals.c` needed this exact
 * computation for the S08 UX-review round-glass fix (PR #25), which is
 * what earned it this shared home instead of a second near-identical
 * per-file copy.
 */
float ff_layout_safe_margin_x(float top_y, float h, float center, float radius, float safety_px);

#ifdef __cplusplus
}
#endif

#endif /* FF_LAYOUT_H */
