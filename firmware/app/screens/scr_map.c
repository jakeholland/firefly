/**
 * scr_map.c — see scr_map.h. Pure render: ff_app_map_t -> LVGL objects.
 * No domain logic (CLAUDE.md) — every branch below is "how to draw
 * feature/dot X", never "should this feature/dot exist" (that call was
 * already made upstream, by fixture.c on a golden or ff_shell.c's
 * shell_project_map on a live tick).
 *
 * ## Untraced-feature render policy (S09 spec, AC3)
 * A feature's `n_pts` drives what gets drawn, and nothing here invents a
 * shape past what the data states (CLAUDE.md: "never fake... positions"):
 *   - `n_pts >= 3`: a real polygon. Filled (13% alpha) + stroked, in the
 *     feature's kind/stage color, label centered at the centroid.
 *   - `n_pts == 2`: no fillable area — a plain stroked line segment
 *     between the two points, label at the midpoint. (Not in the spec's
 *     own worked examples; a defensible minimal-invention reading of "no
 *     invented geometry" for a 2-point path-like feature, flagged per
 *     AGENTS.md.)
 *   - `n_pts == 1`: untraced (no polygon), but a real point exists. A
 *     STAGE renders the spec's own named stub — a labeled 30m circle at
 *     that point. Any OTHER kind with just one point gets a label only,
 *     anchored at that point (no shape) — the spec's stub treatment is
 *     stated for stages specifically ("except stages... render as
 *     labeled 30m circles... only if they carry a point"), and drawing
 *     an invented shape for every other kind's lone point would be
 *     exactly the fabricated geometry CLAUDE.md rules out. Flagged as an
 *     interpretation call, same as the n_pts==2 case above.
 *   - `n_pts == 0`: no polygon AND no point — nothing to honestly anchor
 *     even a label to. Omitted entirely ("otherwise omitted", per spec).
 *
 * ## Polygon fill — no LVGL primitive, same technique scr_radar.c uses
 * LVGL has no filled-polygon widget (scr_radar.c's own header comment on
 * its arrowhead triangle). This file reuses the identical low-level
 * `lv_draw_triangle()` callback technique, fan-triangulated from each
 * polygon's own first vertex. That is an exact fill only for a CONVEX (or
 * star-shaped w.r.t. vertex 0) polygon; a genuinely concave festival
 * footprint could show a fan seam. Accepted for v1 — real traced
 * geometry is being surveyed in a separate, parallel effort (fest-almanac)
 * and this repo has no concave synthetic fixture to design against yet.
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
#include <stdio.h>
#include <string.h>

#include "ff_intent.h" /* the emit seam — see map_tap_back_cb */
#include "ff_map.h"    /* core/include — the shared fixed-fit camera transform */
#include "ff_theme.h"

/* ---------------------------------------------------------------------
 * Layout constants.
 *
 * FF_MAP_CIRCLE_RADIUS_PX/FF_MAP_MARGIN_PX are the spec's OWN literal
 * numbers ("the 412 circle... 24 px margin") taken verbatim, the same
 * convention docs/specs/S06-radar-face.md's arrow/ring numbers (140px,
 * 185px) were transcribed under even though the sim's actual round glass
 * is FF_THEME_PUCK_PX (440px), not literally 412 — see
 * app/screens/radar_layout.h's own constants for the precedent. The
 * fitted content therefore sits with a bit of extra clearance inside the
 * sim's slightly larger puck, never clipped by it.
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
    /* Fan fill from vertex 0 — see this file's header comment on why
     * (no LVGL polygon primitive) and its limit (convex/star-shaped
     * only). */
    for (int i = 1; i + 1 < n; i++) {
        map_draw_filled_triangle(parent, pts_px[0][0], pts_px[0][1], pts_px[i][0], pts_px[i][1], pts_px[i + 1][0],
                                  pts_px[i + 1][1], color_hex, FF_MAP_FILL_OPA);
    }
    for (int i = 0; i < n; i++) {
        int const j = (i + 1) % n;
        map_draw_segment(parent, pts_px[i][0], pts_px[i][1], pts_px[j][0], pts_px[j][1], color_hex, LV_OPA_COVER,
                          FF_MAP_STROKE_PX);
    }
}

static void map_draw_feature(lv_obj_t *parent, ff_map_xform_t const *xform, ff_app_map_feature_t const *f)
{
    uint32_t const color = map_kind_color(f);

    if (f->n_pts == 0) {
        return; /* no polygon, no point — nothing to honestly draw or label (spec: "otherwise omitted") */
    }

    if (f->n_pts == 1) {
        float cx, cy;
        ff_map_project(xform, f->pts_en[0][0], f->pts_en[0][1], &cx, &cy);

        if (f->kind == FF_APP_MAP_KIND_STAGE) {
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
        }
        /* Non-stage single-point feature: label only, no invented shape
         * — see this file's header comment. */
        map_make_label(parent, f->label, FF_THEME_COLOR_INK, cx, cy);
        return;
    }

    /* Project every point once. */
    float pts_px[FF_APP_MAP_MAX_POLY_PTS][2];
    int const n = (f->n_pts < FF_APP_MAP_MAX_POLY_PTS) ? f->n_pts : FF_APP_MAP_MAX_POLY_PTS;
    float sum_x = 0.0f, sum_y = 0.0f;
    for (int i = 0; i < n; i++) {
        ff_map_project(xform, f->pts_en[i][0], f->pts_en[i][1], &pts_px[i][0], &pts_px[i][1]);
        sum_x += pts_px[i][0];
        sum_y += pts_px[i][1];
    }
    float const cx = sum_x / (float)n;
    float const cy = sum_y / (float)n;

    if (n == 2) {
        map_draw_segment(parent, pts_px[0][0], pts_px[0][1], pts_px[1][0], pts_px[1][1], color, LV_OPA_COVER,
                          FF_MAP_STROKE_PX);
    } else {
        map_draw_polygon(parent, pts_px, n, color);
    }
    map_make_label(parent, f->label, FF_THEME_COLOR_INK, cx, cy);
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

static void map_draw_crew(lv_obj_t *parent, ff_map_xform_t const *xform, ff_app_map_crew_t const *c)
{
    if (!c->has_pos) {
        return;
    }
    float cx, cy;
    ff_map_project(xform, c->east_m, c->north_m, &cx, &cy);
    uint32_t const color = ff_theme_crew_color(c->color_idx);

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

    map_make_label(parent, map->rally_label, FF_THEME_COLOR_AMBER, cx, cy - FF_MAP_RALLY_R_PX - 12.0f);
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

static void map_draw_you(lv_obj_t *parent, ff_map_xform_t const *xform, ff_app_map_t const *map)
{
    bool const show_arrow = map->you_has_pos && map->you_heading_valid;
    if (!show_arrow) {
        /* Hidden + "NO FIX" chip (AC5) — a fixed chip, not tied to any
         * position (there may be none to anchor it to). */
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
    map_make_label(parent, "YOU", FF_THEME_COLOR_INK, cx, cy + FF_MAP_YOU_ARROW_LEN_PX * 0.4f + 14.0f);
}

/* ---------------------------------------------------------------------
 * Tap anywhere -> back to Radar (S09 spec).
 * ------------------------------------------------------------------- */

static void map_tap_back_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_BACK, .u = {0}};
    ff_intent_emit(&in);
}

/* ---------------------------------------------------------------------
 * Entry point.
 * ------------------------------------------------------------------- */

void ff_scr_map_build(ff_app_map_t const *map)
{
    if (map == NULL) {
        return;
    }

    map_reset_pools();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *puck = lv_obj_create(scr);
    lv_obj_remove_style_all(puck);
    lv_obj_set_size(puck, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_align(puck, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(puck, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puck, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(puck, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(puck, 0, 0);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_SCROLLABLE);
    /* Tap ANYWHERE -> back (S09 spec) — unlike every other full-screen
     * face in this codebase, the puck itself is the button; every child
     * this file draws clears LV_OBJ_FLAG_CLICKABLE so a tap always
     * resolves here rather than to whichever shape/label happens to be
     * underneath the finger. */
    lv_obj_add_flag(puck, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(puck, map_tap_back_cb, LV_EVENT_CLICKED, NULL);

    /* Fixed-fit camera (S09 slice a): bbox of every feature point across
     * the whole pack, fit into the circle with the spec's own margin. */
    float bbox_pts[FF_APP_MAP_MAX_FEATURES * FF_APP_MAP_MAX_POLY_PTS][2];
    int n_bbox = 0;
    for (uint8_t i = 0; i < map->n_features; i++) {
        ff_app_map_feature_t const *f = &map->features[i];
        for (uint8_t k = 0; k < f->n_pts && k < FF_APP_MAP_MAX_POLY_PTS; k++) {
            bbox_pts[n_bbox][0] = f->pts_en[k][0];
            bbox_pts[n_bbox][1] = f->pts_en[k][1];
            n_bbox++;
        }
    }
    ff_map_xform_t xform;
    ff_map_xform_fit(&xform, bbox_pts, n_bbox, FF_MAP_CIRCLE_RADIUS_PX, FF_MAP_MARGIN_PX);

    for (uint8_t i = 0; i < map->n_features; i++) {
        map_draw_feature(puck, &xform, &map->features[i]);
    }

    map_draw_rally(puck, &xform, map);

    for (uint8_t i = 0; i < map->n_crew; i++) {
        map_draw_crew(puck, &xform, &map->crew[i]);
    }

    map_draw_you(puck, &xform, map);
}
