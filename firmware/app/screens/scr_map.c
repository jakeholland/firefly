/**
 * scr_map.c — see scr_map.h. Pure render: ff_app_map_t -> LVGL objects.
 * No domain logic (CLAUDE.md) — every branch below is "how to draw
 * feature/dot X", never "should this feature/dot exist" (that call was
 * already made upstream, by fixture.c on a golden or ff_shell.c's
 * shell_project_map on a live tick, or by core's
 * `ff_map_feature_render_kind` for the untraced-feature policy below).
 *
 * ## Untraced-feature render policy (S09 spec, AC3)
 * A feature's `n_pts` (plus whether it's a stage) drives what gets
 * drawn — the decision itself is `ff_map_feature_render_kind`
 * (core/include/ff_map.h), a pure function unit-tested directly (PR #73
 * review finding #6: this "if" used to live inline here, reachable only
 * through a loose golden pixel-diff threshold that a mutation slipped
 * under). Nothing here invents a shape past what the data states:
 *   - `n_pts >= 3` (FF_MAP_RENDER_POLYGON): a real polygon. Filled (13%
 *     alpha) + stroked, in the feature's kind/stage color, label
 *     centered at the centroid.
 *   - `n_pts == 2` (FF_MAP_RENDER_LINE): no fillable area — a plain
 *     stroked line segment between the two points, label at the
 *     midpoint. (Not in the spec's own worked examples; a defensible
 *     minimal-invention reading of "no invented geometry" for a 2-point
 *     path-like feature, flagged per AGENTS.md.)
 *   - `n_pts == 1`, a STAGE (FF_MAP_RENDER_STAGE_STUB): the spec's own
 *     named stub — a labeled 30m circle at that point.
 *   - `n_pts == 1`, any OTHER kind (FF_MAP_RENDER_LABEL_ONLY): a label
 *     only, anchored at that point, no shape — the spec's stub
 *     treatment is stated for stages specifically, and drawing an
 *     invented shape for every other kind's lone point would be exactly
 *     the fabricated geometry CLAUDE.md rules out. Flagged as an
 *     interpretation call, same as the n_pts==2 case above.
 *   - `n_pts == 0` (FF_MAP_RENDER_OMIT): no polygon AND no point —
 *     nothing to honestly anchor even a label to. Omitted entirely
 *     ("otherwise omitted", per spec).
 *
 * ## Polygon fill — ear-clipping (concave-safe), stroke-only fallback
 * LVGL has no filled-polygon widget (scr_radar.c's own header comment on
 * its arrowhead triangle), so this file draws fills as triangles via the
 * same low-level `lv_draw_triangle()` callback technique scr_radar.c
 * uses for its arrowhead. PR #73 review finding #2 (BLOCKING): the
 * ORIGINAL vertex-0 fan-triangulation here was only correct for
 * convex/star-shaped input, and the real, currently-merged Lost Lands
 * pack's "Venue extent" feature (9 points) is concave — the fan visibly
 * mis-filled it, bleeding outside the true boundary at the reflex
 * vertex. Replaced with `ff_map_triangulate` (core/include/ff_map.h), an
 * ear-clipping triangulation correct for convex OR concave simple
 * polygons. Per this slice's own Amendment (docs/specs/S09-map-face.md):
 * a WRONG fill is worse than no fill, so if triangulation itself fails
 * (returns negative — degenerate/self-intersecting input, which real
 * simple festival geometry should never produce, but is guarded rather
 * than assumed), `map_draw_polygon` falls back to STROKE-ONLY — the
 * outline still draws, honestly, with no fill claim at all.
 *
 * ## Camera fit: feature ANCHOR POINTS, not full vertex extents
 * PR #73 review finding #3 (Bailey, BLOCKING): fitting the bbox to EVERY
 * vertex of every feature let one or two large boundary polygons (real
 * data: "Venue extent", "Wompy Woods treeline") dominate the scale,
 * crushing the actual cluster of things a rider cares about (stages,
 * pond, camping, ...) into an unreadable smear — verified by rendering
 * the real pack, not a hypothetical. `ff_scr_map_build` now feeds the
 * fit exactly ONE anchor point per feature (`map_feature_anchor_en`
 * below: the single point for a 1-point feature, the centroid for a
 * 2-or-more-point one) rather than every vertex — see this slice's PR
 * body / spec Amendment for why "bounding box of all features" reads as
 * "of each feature's own representative point", not "of every vertex of
 * every feature's full extent". A large polygon's own vertices can
 * therefore now legitimately project outside the fitted circle.
 * `lv_obj_set_style_clip_corner` was the obvious next step (clip the
 * puck's children to its own circular shape, so an over-extent shape
 * reads as "the boundary continues past the visible glass" instead of
 * bleeding into the sim window's square corners) — but it reliably
 * HANGS `ffsim --headless` at this file's draw-object count (issue #75:
 * up to ~300 full-puck-sized triangle/segment proxy objects, and the
 * hang tracks with that count, not with clip_corner alone — Radar's own
 * one or two such objects never hang). Rather than chase that interaction
 * further, issue #75's fix moves containment out of LVGL entirely and
 * into pure geometry, applied once at the SAME place every vertex/label
 * already passes through on its way from meters to pixels:
 *   - Every projected polygon/line vertex is radially clamped to
 *     `FF_MAP_CIRCLE_RADIUS_PX` via `ff_map_clip_point_to_circle`
 *     (core/include/ff_map.h) before it ever reaches
 *     `map_draw_segment`/`map_draw_filled_triangle` — a disk is convex,
 *     so clamping every vertex independently is provably sufficient: no
 *     straight edge between two clamped-or-already-inside points can
 *     bulge back outside. An over-extent boundary polygon now reads as
 *     "flattened against the glass" rather than "bleeding past it",
 *     honestly showing where the glass actually ends.
 *   - `ff_map_place_labels` (same header) takes each label's own
 *     half-width/half-height and rejects (LOW) or radially pulls inward
 *     (HIGH, which can never be dropped) any placement whose bounding
 *     box would cross the same circle — this is also issue #77's
 *     "ultra-long labels run off the circle edge uncropped" half, folded
 *     in here rather than left as a separate follow-up, since it's the
 *     exact same "keep this inside the round silhouette" mechanism.
 * No pixel clip, no LVGL draw-order dependency, no hang — see
 * `map_draw_feature_shape` and `map_label_collector_flush` below for
 * where each is applied, and `ff_map.h`'s doc comments for the full
 * geometric argument.
 *
 * ## Sub-pixel stroke width
 * The spec's "1.3px stroke" has no LVGL equivalent — `lv_obj_set_style_
 * line_width` takes an integer. Rounds to the nearest representable value
 * (1px); a platform limitation, not a spec deviation, same category as
 * this repo's documented font-size substitutions (ff_theme.h's own top
 * comment).
 */
#include "scr_map.h"

#include <math.h>
#include <stdio.h> /* snprintf — the "+N MORE" truncation indicator */

#include "ff_map.h"    /* core/include — the shared fixed-fit camera transform + triangulation */
#include "ff_theme.h"

/* Forward declaration: map_label_collector_flush (below) draws through
 * map_make_label, defined later in this file among the other draw
 * primitives — declared here so the collector, which conceptually
 * belongs with the layout constants near the top, doesn't have to move
 * below them. */
static lv_obj_t *map_make_label(lv_obj_t *parent, char const *text, uint32_t color_hex, float dx, float dy);

/* ---------------------------------------------------------------------
 * Layout constants.
 *
 * FF_MAP_CIRCLE_RADIUS_PX/FF_MAP_MARGIN_PX are the spec's OWN literal
 * numbers ("the 412 circle... 24 px margin") taken verbatim, the same
 * convention docs/specs/S06-radar-face.md's arrow/ring numbers (140px,
 * 185px) were transcribed under. As of S15 slice c the puck IS 412
 * (FF_THEME_PUCK_PX == 412, radius 206 == FF_MAP_CIRCLE_RADIUS_PX), so this
 * map circle now coincides exactly with the puck's own glass rather than
 * sitting inside a slightly larger one — the FF_MAP_MARGIN_PX inset keeps
 * fitted content clear of the bezel, never clipped by it.
 * ------------------------------------------------------------------- */
#define FF_MAP_CIRCLE_RADIUS_PX 206.0f
#define FF_MAP_MARGIN_PX 24.0f

#define FF_MAP_FILL_OPA ((lv_opa_t)33) /* 13% of 255, rounded — spec: "filled 13%-alpha" */
#define FF_MAP_STROKE_PX 1             /* spec: 1.3px — see this file's header note */

#define FF_MAP_STAGE_STUB_RADIUS_M 30.0f /* spec: "labeled 30 m circles" */
#define FF_MAP_CREW_RING_PX 18.0f        /* spec: "18px crew rings" */
#define FF_MAP_IMPRECISE_RING_PX 40.0f   /* larger, honestly-fuzzy stand-in — see ff_app_state.h's imprecise doc comment */
#define FF_MAP_YOU_ARROW_LEN_PX 26.0f
#define FF_MAP_YOU_ARROW_WIDTH_PX 20.0f
#define FF_MAP_RALLY_R_PX 9.0f

/* PR #73, TWO review rounds on label legibility.
 *
 * Round 1 (Bailey finding #3) added a plain nudge-on-collision pass.
 * Round 2 (coordinator, re-rendering the real pack after round 1
 * landed) found that wasn't enough: nudging every colliding label
 * straight down just piles them into a different collision further
 * down, and on the real pack the STAGE names — "the things people
 * actually navigate by" — were landing buried under large area-polygon
 * labels ("Wompy Woods treeline" over "RV/tent camping", "Venue extent
 * (approx.)" over "Subsidia Stage (approx.)"). A flat nudge has no
 * notion of which label matters more, so it protects all of them
 * equally badly.
 *
 * Round 2's fix is PRIORITY-BASED collision resolution, per the
 * coordinator's ruling and `ff_map_feature_label_priority`
 * (core/include/ff_map.h) — see that header for the full tier
 * definition:
 *   - HIGH-priority labels (stages, single-point landmarks, YOU) are
 *     placed FIRST, in pack order, and still use the round-1 nudge —
 *     they can never be dropped, so if two of them land close together
 *     (rare — these are precise points, not sprawling boundaries) the
 *     only honest option is to move one, not hide it.
 *   - LOW-priority labels (non-stage area/boundary polygons) are placed
 *     SECOND, checked against EVERY already-placed label (both tiers),
 *     and DROPPED — not nudged — on collision. A dropped label's SHAPE
 *     still draws (`map_draw_feature_shape`, unconditional); only the
 *     redundant text disappears, which is honest precisely because the
 *     polygon itself still carries the meaning (the coordinator's own
 *     example: "Venue extent (approx.)" arguably needs no label at all
 *     — nobody navigates to the venue outline).
 *
 * Still deliberately NOT radar_layout.c's full reserved-region search
 * (a much larger undertaking, and this screen has no FIXED chrome to
 * reserve against — every element here is data-driven, so there's
 * nothing to precompute a registry from). Bounded and cheap either way:
 * O(features^2) over at most FF_APP_MAP_MAX_FEATURES items. The SHAPE
 * (polygon/stub/line) always stays at its true geographic position;
 * only LABEL TEXT ever moves or disappears — the same honest-cartography
 * convention real maps use (a label's exact pixel, or its presence at
 * all past some density, is a legibility choice, not a claim about
 * where the feature is). */
#define FF_MAP_LABEL_MIN_SEP_PX 48.0f
#define FF_MAP_LABEL_MAX_NUDGE_TRIES 6

/**
 * map_label_collector_t — gathers every label this render wants to draw
 * (feature labels, YOU, rally) as plain requests, THEN resolves all of
 * them in one call to `ff_map_place_labels` (core/include/ff_map.h) and
 * draws whatever came back placed.
 *
 * PR #73 THIRD review round, non-blocking finding #2: the priority/
 * collision ALGORITHM itself now lives in core (`ff_map_place_labels`),
 * unit-tested directly against real Lost Lands coordinates — this
 * collector is the thin LVGL-side glue that builds its input and draws
 * its output, so this file has exactly ONE implementation of the
 * algorithm to keep in sync with, not two. (The previous version of
 * this file kept its own static nudge/collide/record state here,
 * duplicating what became `ff_map_place_labels` — replaced outright
 * rather than left as a second, drift-prone copy.)
 *
 * Sized to `FF_APP_MAP_MAX_FEATURES` feature labels + YOU + rally, which
 * is exactly `FF_MAP_LABEL_MAX_ITEMS`'s own justification
 * (core/include/ff_map.h) — a static_assert below pins that relationship
 * so a future cap change on either side fails the build instead of
 * silently truncating.
 */
typedef struct {
    ff_map_label_request_t req[FF_MAP_LABEL_MAX_ITEMS];
    char const *text[FF_MAP_LABEL_MAX_ITEMS];
    uint32_t color[FF_MAP_LABEL_MAX_ITEMS];
    int count;
} map_label_collector_t;

_Static_assert(FF_APP_MAP_MAX_FEATURES + 2 <= FF_MAP_LABEL_MAX_ITEMS,
               "map_label_collector_t: every feature + YOU + rally must fit FF_MAP_LABEL_MAX_ITEMS");

static void map_label_collector_reset(map_label_collector_t *lc)
{
    lc->count = 0;
}

/**
 * map_label_half_extents — the one LVGL-dependent measurement
 * `ff_map_place_labels`'s circle-bounds check (core/include/ff_map.h,
 * issue #77's fold-in) needs but can't take itself (core stays LVGL-free
 * — CLAUDE.md). Single-line, unwrapped (`LV_TEXT_FLAG_EXPAND` ignores
 * `max_width`) at `FF_THEME_FONT_LABEL` with no extra letter/line
 * spacing — exactly what `map_make_label` below actually draws (a plain
 * `lv_label_create` with only that font set), so this is a measurement of
 * the REAL rendered size, not an estimate. Returns half-width/half-height
 * in px, matching `lv_obj_align(..., LV_ALIGN_CENTER, dx, dy)`'s own
 * "centered on (dx, dy)" convention.
 */
static void map_label_half_extents(char const *text, float *out_half_w, float *out_half_h)
{
    lv_point_t size;
    lv_text_get_size(&size, text, FF_THEME_FONT_LABEL, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
    *out_half_w = (float)size.x / 2.0f;
    *out_half_h = (float)size.y / 2.0f;
}

/**
 * map_label_collector_add — queues one label request. `text` is NOT
 * copied (same "borrowed for the call" convention `ff_intent_t` uses) —
 * every caller in this file passes either a string literal ("YOU") or a
 * pointer into the `ff_app_map_t const *map` this whole render is
 * building from, which outlives `map_label_collector_flush` below (both
 * are only ever read within the same `ff_scr_map_build` call).
 *
 * Silently drops the request past `FF_MAP_LABEL_MAX_ITEMS` rather than
 * overflowing — defensive only: the `_Static_assert` above guarantees
 * this file's own call sites (at most `FF_APP_MAP_MAX_FEATURES` feature
 * labels + YOU + rally) never reach it.
 */
static void map_label_collector_add(map_label_collector_t *lc, float x, float y, ff_map_label_priority_t priority,
                                     char const *text, uint32_t color)
{
    if (lc->count >= FF_MAP_LABEL_MAX_ITEMS) {
        return;
    }
    float half_w, half_h;
    map_label_half_extents(text, &half_w, &half_h);
    lc->req[lc->count].x = x;
    lc->req[lc->count].y = y;
    lc->req[lc->count].priority = priority;
    lc->req[lc->count].half_w = half_w;
    lc->req[lc->count].half_h = half_h;
    lc->text[lc->count] = text;
    lc->color[lc->count] = color;
    lc->count++;
}

/** map_label_collector_flush — resolves every queued request through
 * `ff_map_place_labels` and draws whichever ones came back placed.
 * Requests MUST already be queued with every HIGH-priority one before
 * every LOW-priority one — `ff_map_place_labels`'s own documented caller
 * contract — which this file's single call site in `ff_scr_map_build`
 * satisfies by queuing order (see that function's own comment).
 *
 * `FF_MAP_CIRCLE_RADIUS_PX` is passed as the circle-bounds radius (issue
 * #77's fold-in — see this file's header comment): every result is
 * either dropped (LOW) or pulled inward (HIGH) before it ever reaches
 * `map_make_label`, so nothing drawn here can render past the round
 * glass uncropped. */
static void map_label_collector_flush(map_label_collector_t const *lc, lv_obj_t *parent)
{
    ff_map_label_result_t results[FF_MAP_LABEL_MAX_ITEMS];
    int const n = ff_map_place_labels(lc->req, lc->count, FF_MAP_LABEL_MIN_SEP_PX, FF_MAP_LABEL_MAX_NUDGE_TRIES,
                                       FF_MAP_CIRCLE_RADIUS_PX, results);
    for (int i = 0; i < n; i++) {
        if (!results[i].placed) {
            continue;
        }
        map_make_label(parent, lc->text[i], lc->color[i], results[i].x, results[i].y);
    }
}

/* ---------------------------------------------------------------------
 * lv_line / triangle static storage pools — same "caller cleans before
 * every rebuild" invariant as scr_radar.c's own pools (see that file's
 * header comment for the full rationale, issue #17). Sized for this
 * face's worst case: every feature a closed FF_APP_MAP_MAX_POLY_PTS-gon
 * (stroke = one segment per edge, fill = n-2 triangles), across
 * FF_APP_MAP_MAX_FEATURES features, plus a handful for the YOU arrow and
 * rally pin.
 * ------------------------------------------------------------------- */
#define FF_SCR_MAP_MAX_LINE_SEGMENTS (FF_APP_MAP_MAX_FEATURES * FF_APP_MAP_MAX_POLY_PTS + 8)
#define FF_SCR_MAP_MAX_TRIANGLES (FF_APP_MAP_MAX_FEATURES * FF_APP_MAP_MAX_POLY_PTS + 2)

static lv_point_precise_t s_line_pts[FF_SCR_MAP_MAX_LINE_SEGMENTS][2];
static int s_line_pt_next;

static lv_point_precise_t *map_alloc_line_pts(void)
{
    if (s_line_pt_next >= FF_SCR_MAP_MAX_LINE_SEGMENTS) {
        s_line_pt_next = 0; /* defensive wrap: never index out of bounds */
    }
    return s_line_pts[s_line_pt_next++];
}

typedef struct {
    lv_point_precise_t p[3];
    lv_color_t color;
    lv_opa_t opa;
} map_tri_desc_t;
static map_tri_desc_t s_tri_descs[FF_SCR_MAP_MAX_TRIANGLES];
static int s_tri_desc_next;

static map_tri_desc_t *map_alloc_tri_desc(void)
{
    if (s_tri_desc_next >= FF_SCR_MAP_MAX_TRIANGLES) {
        s_tri_desc_next = 0;
    }
    return &s_tri_descs[s_tri_desc_next++];
}

static void map_reset_pools(void)
{
    s_line_pt_next = 0;
    s_tri_desc_next = 0;
}

/* ---------------------------------------------------------------------
 * Draw primitives — center-relative (puck center = (0,0)), same
 * convention/positioning trick as scr_radar.c's radar_draw_segment /
 * radar_draw_filled_triangle (see those for the "why pin an
 * explicit-full-puck-size object at (0,0)" rationale; not repeated here).
 * ------------------------------------------------------------------- */

static void map_draw_segment(lv_obj_t *parent, float from_dx, float from_dy, float to_dx, float to_dy,
                              uint32_t color_hex, lv_opa_t opa, int32_t width)
{
    int32_t const half = FF_THEME_PUCK_PX / 2;

    lv_point_precise_t *pts = map_alloc_line_pts();
    pts[0].x = half + (int32_t)from_dx;
    pts[0].y = half + (int32_t)from_dy;
    pts[1].x = half + (int32_t)to_dx;
    pts[1].y = half + (int32_t)to_dy;

    lv_obj_t *line = lv_line_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_set_pos(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE); /* tap-anywhere must resolve to the puck, not chrome */
    lv_line_set_points(line, pts, 2);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(color_hex), 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_set_style_line_opa(line, opa, 0);
}

static void map_triangle_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    map_tri_desc_t *td = (map_tri_desc_t *)lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);

    lv_area_t area;
    lv_obj_get_coords(obj, &area);

    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    for (int i = 0; i < 3; i++) {
        dsc.p[i].x = td->p[i].x + area.x1;
        dsc.p[i].y = td->p[i].y + area.y1;
    }
    dsc.color = td->color;
    dsc.opa = td->opa;
    lv_draw_triangle(layer, &dsc);
}

static void map_draw_filled_triangle(lv_obj_t *parent, float p0x, float p0y, float p1x, float p1y, float p2x,
                                      float p2y, uint32_t color_hex, lv_opa_t opa)
{
    int32_t const half = FF_THEME_PUCK_PX / 2;

    map_tri_desc_t *td = map_alloc_tri_desc();
    td->p[0].x = half + (int32_t)p0x;
    td->p[0].y = half + (int32_t)p0y;
    td->p[1].x = half + (int32_t)p1x;
    td->p[1].y = half + (int32_t)p1y;
    td->p[2].x = half + (int32_t)p2x;
    td->p[2].y = half + (int32_t)p2y;
    td->color = lv_color_hex(color_hex);
    td->opa = opa;

    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, map_triangle_draw_cb, LV_EVENT_DRAW_MAIN, td);
}

static lv_obj_t *map_make_label(lv_obj_t *parent, char const *text, uint32_t color_hex, float dx, float dy)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, FF_THEME_FONT_LABEL, 0); /* >= 10px equivalent, per spec */
    lv_obj_set_style_text_color(lbl, lv_color_hex(color_hex), 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, (int32_t)dx, (int32_t)dy);
    return lbl;
}

static lv_obj_t *map_make_chip(lv_obj_t *parent, char const *text, uint32_t bg_hex, uint32_t fg_hex, float dx,
                                float dy)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_set_style_bg_color(chip, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(chip, 12, 0);
    lv_obj_set_style_pad_right(chip, 12, 0);
    lv_obj_set_style_pad_top(chip, 5, 0);
    lv_obj_set_style_pad_bottom(chip, 5, 0);
    lv_obj_set_width(chip, LV_SIZE_CONTENT);
    lv_obj_set_height(chip, LV_SIZE_CONTENT);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE); /* an indicator, not a control — tap-anywhere still hits the puck */
    lv_obj_align(chip, LV_ALIGN_CENTER, (int32_t)dx, (int32_t)dy);

    lv_obj_t *lbl = lv_label_create(chip);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(fg_hex), 0);
    lv_obj_center(lbl);
    return chip;
}

/* ---------------------------------------------------------------------
 * Feature-kind color (S09) — see ff_theme.h's Map-kind-colors block for
 * the palette and its provenance note.
 * ------------------------------------------------------------------- */

static uint32_t map_kind_color(ff_app_map_feature_t const *f)
{
    if (f->kind == FF_APP_MAP_KIND_STAGE) {
        return f->color_valid ? f->color_rgb : FF_THEME_COLOR_MUTED;
    }
    switch (f->kind) {
        case FF_APP_MAP_KIND_CAMPING:  return FF_THEME_MAP_CAMPING;
        case FF_APP_MAP_KIND_WATER:    return FF_THEME_MAP_WATER;
        case FF_APP_MAP_KIND_PATH:     return FF_THEME_MAP_PATH;
        case FF_APP_MAP_KIND_ENTRANCE: return FF_THEME_MAP_ENTRANCE;
        case FF_APP_MAP_KIND_VENDOR:   return FF_THEME_MAP_VENDOR;
        case FF_APP_MAP_KIND_MEDICAL:  return FF_THEME_MAP_MEDICAL;
        case FF_APP_MAP_KIND_POI:      return FF_THEME_MAP_POI;
        default:                       return FF_THEME_MAP_UNKNOWN;
    }
}

/* ---------------------------------------------------------------------
 * Feature rendering.
 * ------------------------------------------------------------------- */

static void map_draw_polygon(lv_obj_t *parent, float const pts_px[][2], int n, uint32_t color_hex)
{
    /* Concave-safe fill via ear clipping — see this file's header
     * comment for the full rationale and the stroke-only fallback
     * below. `n <= FF_APP_MAP_MAX_POLY_PTS` always (the only caller
     * projects a feature's own points, already capped that way), so
     * `n - 2` triangles always fit in a buffer sized to the same cap. */
    uint8_t tris[FF_APP_MAP_MAX_POLY_PTS][3];
    int const n_tris = ff_map_triangulate(pts_px, n, tris, FF_APP_MAP_MAX_POLY_PTS);

    if (n_tris > 0) {
        for (int i = 0; i < n_tris; i++) {
            uint8_t const a = tris[i][0], b = tris[i][1], c = tris[i][2];
            map_draw_filled_triangle(parent, pts_px[a][0], pts_px[a][1], pts_px[b][0], pts_px[b][1], pts_px[c][0],
                                      pts_px[c][1], color_hex, FF_MAP_FILL_OPA);
        }
    }
    /* n_tris < 0: ff_map_triangulate couldn't safely fill this polygon
     * (degenerate/self-intersecting input — never expected from real
     * simple festival geometry, but guarded rather than assumed). A
     * wrong fill is worse than no fill (S09-map-face.md's Amendments):
     * the outline still draws below either way, honestly, with no fill
     * claim at all when triangulation failed. */
    for (int i = 0; i < n; i++) {
        int const j = (i + 1) % n;
        map_draw_segment(parent, pts_px[i][0], pts_px[i][1], pts_px[j][0], pts_px[j][1], color_hex, LV_OPA_COVER,
                          FF_MAP_STROKE_PX);
    }
}

/**
 * map_feature_anchor_en — the ONE east/north point a feature contributes
 * to the camera fit (see this file's header comment on why anchors, not
 * full vertex extents) — and, doubling as the point its label centers
 * on, since a feature's "representative point" is the same concept
 * either way. The single point for a 1-point feature; the vertex
 * CENTROID for 2-or-more (an affine map projects the centroid of a set
 * to the centroid of the projected set, so computing this once in
 * east/north meters and projecting it is exactly equivalent to
 * projecting every point and averaging in px — just cheaper). Returns 0
 * (writes nothing) for a 0-point feature — nothing to anchor.
 */
static int map_feature_anchor_en(ff_app_map_feature_t const *f, float *out_e, float *out_n)
{
    if (f->n_pts == 0) return 0;
    if (f->n_pts == 1) {
        *out_e = f->pts_en[0][0];
        *out_n = f->pts_en[0][1];
        return 1;
    }
    int const n = (f->n_pts < FF_APP_MAP_MAX_POLY_PTS) ? f->n_pts : FF_APP_MAP_MAX_POLY_PTS;
    float sum_e = 0.0f, sum_n = 0.0f;
    for (int i = 0; i < n; i++) {
        sum_e += f->pts_en[i][0];
        sum_n += f->pts_en[i][1];
    }
    *out_e = sum_e / (float)n;
    *out_n = sum_n / (float)n;
    return 1;
}

/**
 * map_draw_feature_shape — draws ONLY the shape (stub circle / stroked
 * line / filled+stroked polygon) for one feature, per `render_kind`.
 * NEVER draws or touches a label — label placement is a separate pass
 * over every feature (see `ff_scr_map_build`), because whether a LABEL
 * draws depends on priority + what ELSE has already been placed, which
 * this function has no visibility into and shouldn't need. A feature's
 * shape, by contrast, is unconditional: it never depends on anything
 * else on the map (PR #73 second review round — labels can be dropped
 * on collision, shapes never are).
 */
static void map_draw_feature_shape(lv_obj_t *parent, ff_map_xform_t const *xform, ff_app_map_feature_t const *f,
                                    ff_map_render_kind_t render_kind)
{
    uint32_t const color = map_kind_color(f);

    switch (render_kind) {
    case FF_MAP_RENDER_STAGE_STUB: {
        float anchor_e, anchor_n, cx, cy;
        (void)map_feature_anchor_en(f, &anchor_e, &anchor_n);
        ff_map_project(xform, anchor_e, anchor_n, &cx, &cy);
        float const r_px = FF_MAP_STAGE_STUB_RADIUS_M * xform->scale_px_per_m;
        lv_obj_t *stub = lv_obj_create(parent);
        lv_obj_remove_style_all(stub);
        lv_obj_set_size(stub, (int32_t)(r_px * 2.0f), (int32_t)(r_px * 2.0f));
        lv_obj_set_style_radius(stub, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(stub, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(stub, FF_MAP_FILL_OPA, 0);
        lv_obj_set_style_border_width(stub, FF_MAP_STROKE_PX, 0);
        lv_obj_set_style_border_color(stub, lv_color_hex(color), 0);
        lv_obj_set_style_border_opa(stub, LV_OPA_COVER, 0);
        lv_obj_clear_flag(stub, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(stub, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(stub, LV_ALIGN_CENTER, (int32_t)cx, (int32_t)cy);
        return;
    }
    case FF_MAP_RENDER_LABEL_ONLY:
        return; /* no shape — see this file's header comment */
    case FF_MAP_RENDER_LINE: {
        /* Issue #75: clamp each projected endpoint to the round glass
         * BEFORE drawing — see this file's header comment and
         * ff_map_clip_point_to_circle's own doc comment for why this
         * replaces lv_obj_set_style_clip_corner without the hang. */
        float p0x, p0y, p1x, p1y;
        ff_map_project(xform, f->pts_en[0][0], f->pts_en[0][1], &p0x, &p0y);
        ff_map_project(xform, f->pts_en[1][0], f->pts_en[1][1], &p1x, &p1y);
        ff_map_clip_point_to_circle(p0x, p0y, FF_MAP_CIRCLE_RADIUS_PX, &p0x, &p0y);
        ff_map_clip_point_to_circle(p1x, p1y, FF_MAP_CIRCLE_RADIUS_PX, &p1x, &p1y);
        map_draw_segment(parent, p0x, p0y, p1x, p1y, color, LV_OPA_COVER, FF_MAP_STROKE_PX);
        return;
    }
    case FF_MAP_RENDER_POLYGON: {
        /* Issue #75: clamp every projected vertex to the round glass
         * before triangulating/stroking — a disk is convex, so this
         * alone keeps the WHOLE fill and outline inside it (see this
         * file's header comment). */
        float pts_px[FF_APP_MAP_MAX_POLY_PTS][2];
        int const n = (f->n_pts < FF_APP_MAP_MAX_POLY_PTS) ? f->n_pts : FF_APP_MAP_MAX_POLY_PTS;
        for (int i = 0; i < n; i++) {
            float px, py;
            ff_map_project(xform, f->pts_en[i][0], f->pts_en[i][1], &px, &py);
            ff_map_clip_point_to_circle(px, py, FF_MAP_CIRCLE_RADIUS_PX, &pts_px[i][0], &pts_px[i][1]);
        }
        map_draw_polygon(parent, pts_px, n, color);
        return;
    }
    case FF_MAP_RENDER_OMIT:
    default:
        return; /* nothing to draw; kept so -Wswitch stays exhaustive */
    }
}

/**
 * map_collect_feature_label — queues ONE feature's label request (never
 * draws directly — see `map_label_collector_t`'s doc comment for why
 * every label on this map is resolved together, in one
 * `ff_map_place_labels` call, not feature-by-feature). No-op for
 * `FF_MAP_RENDER_OMIT` (nothing to anchor a label to).
 */
static void map_collect_feature_label(map_label_collector_t *lc, ff_map_xform_t const *xform,
                                       ff_app_map_feature_t const *f, ff_map_render_kind_t render_kind,
                                       ff_map_label_priority_t priority)
{
    if (render_kind == FF_MAP_RENDER_OMIT) {
        return;
    }

    float anchor_e = 0.0f, anchor_n = 0.0f;
    (void)map_feature_anchor_en(f, &anchor_e, &anchor_n); /* always succeeds: render_kind != OMIT implies n_pts >= 1 */
    float cx, cy;
    ff_map_project(xform, anchor_e, anchor_n, &cx, &cy);
    map_label_collector_add(lc, cx, cy, priority, f->label, FF_THEME_COLOR_INK);
}

/* ---------------------------------------------------------------------
 * Crew dot rendering — reuses Radar's stale/solid ring idiom (a hollow
 * outline ring reads as "not current" the same way it already does on
 * the radar face's own crew ring; see ff_radar_dot_t's doc comment,
 * which names this exact treatment "the dashed/ghost ring-dot
 * treatment"). `place` is carried but has no distinct map render yet —
 * see ff_app_map_crew_t's own doc comment for why that's a recorded gap,
 * not an oversight.
 * ------------------------------------------------------------------- */

static void map_draw_crew(lv_obj_t *parent, ff_map_xform_t const *xform, ff_app_map_crew_t const *c, bool colorblind)
{
    if (!c->has_pos) {
        return;
    }
    float cx, cy;
    ff_map_project(xform, c->east_m, c->north_m, &cx, &cy);
    uint32_t const color = ff_theme_crew_color(c->color_idx, colorblind);

    if (c->imprecise) {
        /* Issue #47, applied to the map: a degraded-precision fix cannot
         * honestly support a crisp pin-point ring — see
         * ff_app_map_crew_t's own doc comment. A larger, hollow, no-
         * initial ring stands in for "somewhere around here", never the
         * normal 18px pin-point dot. */
        lv_obj_t *ring = lv_obj_create(parent);
        lv_obj_remove_style_all(ring);
        lv_obj_set_size(ring, (int32_t)FF_MAP_IMPRECISE_RING_PX, (int32_t)FF_MAP_IMPRECISE_RING_PX);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 2, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(color), 0);
        lv_obj_set_style_border_opa(ring, LV_OPA_60, 0);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(ring, LV_ALIGN_CENTER, (int32_t)cx, (int32_t)cy);
        return;
    }

    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, (int32_t)FF_MAP_CREW_RING_PX, (int32_t)FF_MAP_CREW_RING_PX);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);

    if (c->stale) {
        lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(dot, 2, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(color), 0);
        lv_obj_set_style_border_opa(dot, LV_OPA_70, 0);
    } else {
        lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    }
    lv_obj_align(dot, LV_ALIGN_CENTER, (int32_t)cx, (int32_t)cy);

    lv_obj_t *lbl = lv_label_create(dot);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    char ch[2] = {c->initial, '\0'};
    lv_label_set_text(lbl, c->initial != '\0' ? ch : "");
    lv_obj_set_style_text_font(lbl, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(c->stale ? color : FF_THEME_COLOR_BG), 0);
    lv_obj_center(lbl);
}

/* ---------------------------------------------------------------------
 * Rally pin — amber, per spec.
 * ------------------------------------------------------------------- */

/**
 * map_draw_rally / map_collect_rally_label — split in two (PR #73 THIRD
 * review round, BLOCKING finding #1): the pin always draws unconditionally
 * (a marker, like any feature shape, is never dropped), but the LABEL
 * must join the SAME priority/collision system every feature label uses
 * — rally is documented HIGH priority (spec Amendment, this file's own
 * header comment on the declutter mechanism) but a bare `map_make_label`
 * call never actually entered the collision system, so it could neither
 * be nudged off a real collision nor be seen by anything placed after
 * it. `ff_scr_map_build` calls `map_draw_rally` for the pin unconditionally
 * and `map_collect_rally_label` SEPARATELY to QUEUE the label (drawn
 * later, by `map_label_collector_flush`, alongside every other label) —
 * see that function's own comment for the exact ordering and why.
 */
static void map_draw_rally(lv_obj_t *parent, ff_map_xform_t const *xform, ff_app_map_t const *map)
{
    if (!map->has_rally) {
        return;
    }
    float cx, cy;
    ff_map_project(xform, map->rally_east_m, map->rally_north_m, &cx, &cy);

    lv_obj_t *pin = lv_obj_create(parent);
    lv_obj_remove_style_all(pin);
    lv_obj_set_size(pin, (int32_t)(FF_MAP_RALLY_R_PX * 2.0f), (int32_t)(FF_MAP_RALLY_R_PX * 2.0f));
    lv_obj_set_style_radius(pin, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pin, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_bg_opa(pin, LV_OPA_COVER, 0);
    lv_obj_clear_flag(pin, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(pin, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(pin, LV_ALIGN_CENTER, (int32_t)cx, (int32_t)cy);
}

static void map_collect_rally_label(map_label_collector_t *lc, ff_map_xform_t const *xform, ff_app_map_t const *map)
{
    if (!map->has_rally) {
        return;
    }
    float cx, cy;
    ff_map_project(xform, map->rally_east_m, map->rally_north_m, &cx, &cy);
    float const lx = cx, ly = cy - FF_MAP_RALLY_R_PX - 12.0f;
    map_label_collector_add(lc, lx, ly, FF_MAP_LABEL_PRIORITY_HIGH, map->rally_label, FF_THEME_COLOR_AMBER);
}

/* ---------------------------------------------------------------------
 * YOU arrow — rotated by heading; hidden + "NO FIX" chip when no fix
 * (S09 AC5).
 * ------------------------------------------------------------------- */

/* Rotates (x, y) by `heading_deg` CLOCKWISE ON SCREEN (0 = up/north,
 * increasing clockwise — the same compass convention every other
 * heading-driven element in this codebase uses, e.g. ff_geo_arrow_deg).
 * Applying the textbook counter-clockwise rotation matrix directly to
 * screen (y-DOWN) coordinates produces exactly this: the y-flip between
 * math convention and screen convention inverts the matrix's visual
 * sense for free. */
static void map_rotate(float x, float y, float heading_deg, float *out_x, float *out_y)
{
    float const rad = heading_deg * (float)(3.14159265358979323846 / 180.0);
    float const c = cosf(rad);
    float const s = sinf(rad);
    *out_x = x * c - y * s;
    *out_y = x * s + y * c;
}

/**
 * map_draw_you / map_collect_you_label — split in two (PR #73 THIRD
 * review round, BLOCKING finding #1): same bug and same fix as
 * `map_draw_rally`/`map_collect_rally_label` above. The arrow (or "NO
 * FIX" chip) always draws unconditionally; the "YOU" label — documented
 * HIGH priority — must join the SAME collision system every feature
 * label uses, so it can be nudged off a real collision and so later
 * labels see it as already placed. Verified failing before this split:
 * on the real pack (`map_real_lost_lands.json`, YOU at heading 60°),
 * YOU's label landed ~39px from the "Wompy Woods" stage label — under
 * this file's own 48px `FF_MAP_LABEL_MIN_SEP_PX` — because neither one
 * knew the other existed (pinned for regression as
 * `S09_place_labels_you_nudges_off_real_wompy_woods_collision`,
 * core/tests/test_map.c, against these exact real coordinates).
 * `ff_scr_map_build` calls `map_draw_you` for the arrow/chip and
 * `map_collect_you_label` SEPARATELY to QUEUE the label.
 */
static void map_draw_you(lv_obj_t *parent, ff_map_xform_t const *xform, ff_app_map_t const *map)
{
    bool const show_arrow = map->you_has_pos && map->you_heading_valid;
    if (!show_arrow) {
        /* Hidden + "NO FIX" chip (AC5) — a fixed chip, not tied to any
         * position (there may be none to anchor it to), and not part of
         * the label-collision system either: it's fixed-position status
         * chrome, not a feature/YOU label competing for map space. */
        map_make_chip(parent, "NO FIX", FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_MUTED, 0.0f,
                       FF_MAP_CIRCLE_RADIUS_PX - FF_MAP_MARGIN_PX - 20.0f);
        return;
    }

    float cx, cy;
    ff_map_project(xform, map->you_east_m, map->you_north_m, &cx, &cy);

    float tip_x, tip_y, left_x, left_y, right_x, right_y;
    map_rotate(0.0f, -FF_MAP_YOU_ARROW_LEN_PX, map->you_heading_deg, &tip_x, &tip_y);
    map_rotate(-FF_MAP_YOU_ARROW_WIDTH_PX / 2.0f, FF_MAP_YOU_ARROW_LEN_PX * 0.4f, map->you_heading_deg, &left_x,
               &left_y);
    map_rotate(FF_MAP_YOU_ARROW_WIDTH_PX / 2.0f, FF_MAP_YOU_ARROW_LEN_PX * 0.4f, map->you_heading_deg, &right_x,
               &right_y);

    map_draw_filled_triangle(parent, cx + tip_x, cy + tip_y, cx + left_x, cy + left_y, cx + right_x, cy + right_y,
                              FF_THEME_COLOR_INK, LV_OPA_COVER);
}

static void map_collect_you_label(map_label_collector_t *lc, ff_map_xform_t const *xform, ff_app_map_t const *map)
{
    if (!(map->you_has_pos && map->you_heading_valid)) {
        return; /* no arrow drawn (NO FIX chip instead) -> no "YOU" label either */
    }
    float cx, cy;
    ff_map_project(xform, map->you_east_m, map->you_north_m, &cx, &cy);
    float const lx = cx, ly = cy + FF_MAP_YOU_ARROW_LEN_PX * 0.4f + 14.0f;
    map_label_collector_add(lc, lx, ly, FF_MAP_LABEL_PRIORITY_HIGH, "YOU", FF_THEME_COLOR_INK);
}

/* ---------------------------------------------------------------------
 * Truncation indicator (PR #73 review finding #1) — an honest "this
 * view is known incomplete" signal, matching fixture.c's fail-loud
 * convention on the same cap rather than silently presenting a
 * truncated view as the whole map. Amber (FF_THEME_COLOR_STALE_AMBER),
 * the same "something needs your attention" alert color scr_radar.c
 * already uses for mesh-loss/low-battery — this is that same category
 * of fact, not a neutral status line.
 * ------------------------------------------------------------------- */

static void map_draw_truncated_indicator(lv_obj_t *parent, ff_app_map_t const *map)
{
    if (!map->truncated) {
        return;
    }
    char text[24];
    if (map->features_omitted > 0) {
        snprintf(text, sizeof(text), "+%u MORE", (unsigned)map->features_omitted);
    } else {
        /* Every feature was kept, but at least one kept feature's own
         * polygon lost points (ff_shell.c's shell_project_map doc
         * comment) — a real, distinct kind of incompleteness with
         * nothing to count, so it gets its own honest wording rather
         * than a fabricated "+0 MORE". */
        snprintf(text, sizeof(text), "MAP INCOMPLETE");
    }
    /* Top of the puck — mirrors the "NO FIX" chip's bottom placement
     * (map_draw_you) so the two honest-incompleteness signals never
     * collide. */
    map_make_chip(parent, text, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_STALE_AMBER, 0.0f,
                  -(FF_MAP_CIRCLE_RADIUS_PX - FF_MAP_MARGIN_PX - 20.0f));
}

/* ---------------------------------------------------------------------
 * Entry point.
 * ------------------------------------------------------------------- */

void ff_scr_map_build(lv_obj_t *parent, ff_app_map_t const *map, bool colorblind)
{
    if (parent == NULL || map == NULL) {
        return;
    }

    map_reset_pools();
    map_label_collector_t label_collector;
    map_label_collector_reset(&label_collector);

    lv_obj_t *puck = lv_obj_create(parent);
    lv_obj_remove_style_all(puck);
    lv_obj_set_size(puck, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_align(puck, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(puck, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puck, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(puck, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(puck, 0, 0);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_SCROLLABLE);
    /* Carousel tile now: non-clickable, and its children (below) all
     * clear CLICKABLE too, so a horizontal drag anywhere on Map bubbles
     * up as the GESTURE the nav tileview decodes into a swipe — and a
     * long-press bubbles to the nav puck (jump-to-Settings) exactly as on
     * the other tiles. (There is no tap-to-go-back any more — Map is left
     * by swiping, like every other face.) */
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(puck, LV_OBJ_FLAG_EVENT_BUBBLE);
    /* No `lv_obj_set_style_clip_corner` here — deliberately (issue #75:
     * it reliably hangs `ffsim --headless` at this file's draw-object
     * count). Containment is geometric instead: every projected
     * polygon/line vertex is clamped to `FF_MAP_CIRCLE_RADIUS_PX` in
     * `map_draw_feature_shape`, and every label's bounding box is kept
     * inside the same circle by `ff_map_place_labels` (dropped if LOW,
     * pulled inward if HIGH) in `map_label_collector_flush` — see this
     * file's header comment for the full rationale. */

    /* Fixed-fit camera (S09 slice a, refined by PR #73 review finding
     * #3): ONE anchor point per feature — not every vertex of every
     * feature — fit into the circle with the spec's own margin. See
     * this file's header comment for why. */
    float bbox_pts[FF_APP_MAP_MAX_FEATURES][2];
    int n_bbox = 0;
    for (uint8_t i = 0; i < map->n_features; i++) {
        float e, n;
        if (map_feature_anchor_en(&map->features[i], &e, &n)) {
            bbox_pts[n_bbox][0] = e;
            bbox_pts[n_bbox][1] = n;
            n_bbox++;
        }
    }
    ff_map_xform_t xform;
    ff_map_xform_fit(&xform, bbox_pts, n_bbox, FF_MAP_CIRCLE_RADIUS_PX, FF_MAP_MARGIN_PX);

    /* Shapes/markers, then ONE batched label resolution, then crew/
     * chrome — deliberately in this order (PR #73 second AND third
     * review rounds — priority-based label collision resolution, see
     * this file's header comment on `map_label_collector_t` and
     * `core/include/ff_map.h`'s `ff_map_place_labels`):
     *   1. every feature's SHAPE, unconditionally — never dropped, and
     *      drawing every shape before any label means a later feature's
     *      polygon fill can never paint over an earlier feature's text
     *      (LVGL draws children in creation order). Rally's pin and
     *      YOU's arrow (or "NO FIX" chip) are markers in the same sense
     *      — always drawn, never part of the collision system — so they
     *      join this same early "never dropped" group.
     *   2. QUEUE every label this render wants, in ONE collector, HIGH
     *      priority first: every HIGH-priority feature label (stages,
     *      single-point landmarks), then YOU's and rally's labels —
     *      third review round, BLOCKING: both are documented HIGH
     *      priority ("stages …, every single-point … feature …, and
     *      YOU" / the spec Amendment) but a bare `map_make_label` call
     *      never actually joined the collision system, so YOU could
     *      (and did, on the real pack) land directly on top of a stage
     *      label with neither one aware of the other — then every
     *      LOW-priority feature label (non-stage area polygons). This
     *      exact ordering is `ff_map_place_labels`'s own documented
     *      caller contract (every HIGH entry before every LOW one).
     *   3. ONE call to `ff_map_place_labels` resolves the whole queue —
     *      HIGH entries nudge off collisions and are always drawn, LOW
     *      entries drop (no draw) on collision with anything already
     *      placed — and `map_label_collector_flush` draws whichever
     *      results came back placed.
     *   4. crew dots and the truncation indicator — outside the label
     *      collision system entirely (crew uses small initials, not
     *      full-word labels; the indicator is fixed-position chrome). */
    for (uint8_t i = 0; i < map->n_features; i++) {
        ff_app_map_feature_t const *f = &map->features[i];
        ff_map_render_kind_t const rk = ff_map_feature_render_kind(f->n_pts, f->kind == FF_APP_MAP_KIND_STAGE);
        map_draw_feature_shape(puck, &xform, f, rk);
    }
    map_draw_rally(puck, &xform, map);
    map_draw_you(puck, &xform, map);

    for (uint8_t i = 0; i < map->n_features; i++) {
        ff_app_map_feature_t const *f = &map->features[i];
        ff_map_render_kind_t const rk = ff_map_feature_render_kind(f->n_pts, f->kind == FF_APP_MAP_KIND_STAGE);
        ff_map_label_priority_t const pr = ff_map_feature_label_priority(f->n_pts, f->kind == FF_APP_MAP_KIND_STAGE);
        if (pr == FF_MAP_LABEL_PRIORITY_HIGH) {
            map_collect_feature_label(&label_collector, &xform, f, rk, pr);
        }
    }
    map_collect_you_label(&label_collector, &xform, map);
    map_collect_rally_label(&label_collector, &xform, map);

    for (uint8_t i = 0; i < map->n_features; i++) {
        ff_app_map_feature_t const *f = &map->features[i];
        ff_map_render_kind_t const rk = ff_map_feature_render_kind(f->n_pts, f->kind == FF_APP_MAP_KIND_STAGE);
        ff_map_label_priority_t const pr = ff_map_feature_label_priority(f->n_pts, f->kind == FF_APP_MAP_KIND_STAGE);
        if (pr == FF_MAP_LABEL_PRIORITY_LOW) {
            map_collect_feature_label(&label_collector, &xform, f, rk, pr);
        }
    }

    map_label_collector_flush(&label_collector, puck);

    for (uint8_t i = 0; i < map->n_crew; i++) {
        map_draw_crew(puck, &xform, &map->crew[i], colorblind);
    }

    map_draw_truncated_indicator(puck, map);
}
