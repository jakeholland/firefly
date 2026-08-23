/**
 * scr_radar.c — see scr_radar.h. Pure render: ff_radar_view_t -> LVGL
 * objects. No domain logic (CLAUDE.md) — every branch below is "how to
 * draw mode X", never "should this be mode X".
 */
#include "scr_radar.h"

#include <math.h>
#include <stdio.h>

#include "ff_theme.h"

#define FF_SCR_RADAR_PI 3.14159265358979323846f

/* ---------------------------------------------------------------------
 * lv_line point storage.
 *
 * lv_line_set_points() keeps the POINTER it's given, not a copy — the
 * array must outlive the lv_line object. This file only ever builds one
 * radar screen per process TODAY (targets/sim/main.c calls
 * ff_scr_radar_build/ff_scr_nav_build exactly once per run/window
 * session — headless mode renders a single frame and exits; window mode
 * builds the screen once and lets LVGL's own timer loop repaint that same
 * static tree), so this small static pool — reset at the top of every
 * ff_scr_radar_build() call — is safe for now and needs no lifetime
 * bookkeeping beyond that.
 *
 * THIS BREAKS THE MOMENT ANYTHING RE-RENDERS AT 10 HZ (S06's own spec'd
 * update rate for live crew data) OR TEARS DOWN/REBUILDS A SCREEN ON FACE
 * SWITCH: resetting `s_line_pt_next` on a second call overwrites points a
 * still-alive `lv_line` from the *first* call still points to — silent
 * corruption, not a crash. Nothing in app/ or targets/sim/ currently
 * calls lv_obj_del/lv_obj_clean either, so there is no teardown path at
 * all yet. Tracked in https://github.com/jakeholland/firefly/issues/17
 * (likely fix: an update-in-place model — create these objects once,
 * mutate their properties at 10 Hz — plus per-object point storage,
 * rather than rebuild-and-teardown every tick); out of scope for this PR,
 * which only ever builds a screen once per process. See this same note
 * on the triangle-descriptor pool below (same hazard, same tracking
 * issue).
 * ------------------------------------------------------------------- */
#define FF_SCR_RADAR_MAX_LINE_SEGMENTS 16
static lv_point_precise_t s_line_pts[FF_SCR_RADAR_MAX_LINE_SEGMENTS][2];
static int s_line_pt_next;

static lv_point_precise_t *radar_alloc_line_pts(void)
{
    if (s_line_pt_next >= FF_SCR_RADAR_MAX_LINE_SEGMENTS) {
        s_line_pt_next = 0; /* defensive wrap: never index out of bounds */
    }
    return s_line_pts[s_line_pt_next++];
}

/* Screen direction convention shared with ff_radar_compute/ff_geo: 0 deg
 * = straight "up" (ahead), clockwise — same as ff_geo_bearing_deg's and
 * ff_geo_arrow_deg's documented convention. */
static void radar_deg_to_offset(float deg, float radius_px, float *dx, float *dy)
{
    float rad = deg * (FF_SCR_RADAR_PI / 180.0f);
    *dx = radius_px * sinf(rad);
    *dy = -radius_px * cosf(rad);
}

/* ---------------------------------------------------------------------
 * Small shared builders.
 * ------------------------------------------------------------------- */

static lv_obj_t *radar_make_chip(lv_obj_t *parent, char const *text, uint32_t bg_hex, uint32_t fg_hex, int32_t dy)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_set_style_bg_color(chip, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(chip, 14, 0);
    lv_obj_set_style_pad_right(chip, 14, 0);
    lv_obj_set_style_pad_top(chip, 6, 0);
    lv_obj_set_style_pad_bottom(chip, 6, 0);
    lv_obj_set_width(chip, LV_SIZE_CONTENT);
    lv_obj_set_height(chip, LV_SIZE_CONTENT);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE); /* an indicator, not a control */
    lv_obj_align(chip, LV_ALIGN_CENTER, 0, dy);

    lv_obj_t *label = lv_label_create(chip);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(fg_hex), 0);
    lv_obj_center(label);

    return chip;
}

/* A translucent ring hugging the puck's own edge — the STALE/LOST "rim
 * tint" (S06: "amber rim tint"). Drawn here (not by the shell, which owns
 * the puck object) so this file stays a pure function of (parent, radar)
 * with no dependency on the shell's internal object tree. */
static void radar_build_rim_tint(lv_obj_t *parent, uint32_t color_hex, lv_opa_t opa)
{
    lv_obj_t *rim = lv_obj_create(parent);
    lv_obj_remove_style_all(rim);
    lv_obj_set_size(rim, FF_THEME_PUCK_PX - 4, FF_THEME_PUCK_PX - 4);
    lv_obj_set_style_radius(rim, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(rim, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rim, 3, 0);
    lv_obj_set_style_border_color(rim, lv_color_hex(color_hex), 0);
    lv_obj_set_style_border_opa(rim, opa, 0);
    lv_obj_clear_flag(rim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(rim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(rim);
}

/* Status bar: clock / mesh / battery. Cross-mode chrome, not part of the
 * mode-specific truth table — always rendered from whatever the caller
 * populated in *r (ff_radar_compute never touches these three fields;
 * see ff_radar.h's deviation note). */
static void radar_build_status_bar(lv_obj_t *parent, ff_radar_view_t const *r)
{
    char buf[24];

    lv_obj_t *clock_lbl = lv_label_create(parent);
    lv_label_set_text(clock_lbl, r->clock_str[0] != '\0' ? r->clock_str : "--:--");
    lv_obj_set_style_text_font(clock_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(clock_lbl, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_align(clock_lbl, LV_ALIGN_CENTER, -78, -195);

    lv_obj_t *mesh_lbl = lv_label_create(parent);
    lv_label_set_text(mesh_lbl, r->mesh_ok ? "MESH" : "NO MESH");
    lv_obj_set_style_text_font(mesh_lbl, FF_THEME_FONT_LABEL, 0);
    /* UX review (non-blocking finding #6): losing the mesh radio breaks
     * the whole point of the puck (finding friends) — it must read at
     * least as alarming as a low battery, not as flat grey chrome. Same
     * alert color as the low-battery case below. */
    lv_obj_set_style_text_color(mesh_lbl, lv_color_hex(r->mesh_ok ? FF_THEME_COLOR_LIVE_GREEN : FF_THEME_COLOR_STALE_AMBER),
                                 0);
    lv_obj_align(mesh_lbl, LV_ALIGN_CENTER, 0, -195);

    lv_obj_t *batt_lbl = lv_label_create(parent);
    if (r->batt_pct < 0) {
        snprintf(buf, sizeof(buf), "--%%");
    } else {
        snprintf(buf, sizeof(buf), "%d%%", (int)r->batt_pct);
    }
    lv_label_set_text(batt_lbl, buf);
    lv_obj_set_style_text_font(batt_lbl, FF_THEME_FONT_LABEL, 0);
    bool batt_low = (r->batt_pct >= 0 && r->batt_pct <= FF_THEME_BATT_LOW_PCT);
    lv_obj_set_style_text_color(batt_lbl,
                                 lv_color_hex(batt_low ? FF_THEME_COLOR_STALE_AMBER : FF_THEME_COLOR_MUTED), 0);
    lv_obj_align(batt_lbl, LV_ALIGN_CENTER, 78, -195);
}

/* ---------------------------------------------------------------------
 * Chrome keep-out policy (UX review finding #3).
 *
 * Ring dots are positioned by heading-relative bearing, so ANY fixed
 * chrome (chips, buttons, the name/distance block) can collide with a
 * dot at some heading — confirmed in review at three different spots
 * (FLARE button vs. a dot, the STALE chip vs. a dot, the CLOSE ring vs.
 * the name). Rather than one-off nudge each report, every mode declares
 * its fixed chrome as a short list of rectangles (center-relative,
 * generous rather than pixel-exact — better to push a dot a little
 * further than strictly required than to leave a real overlap); a dot
 * whose position falls inside (or within its own half-size of) any
 * rectangle is pushed OUT along whichever edge of that rectangle is
 * nearest — a standard axis-aligned minimum-translation push, not a
 * pure "reduce the radius toward center" pull-in.
 *
 * Pure radial pull-in was tried first and doesn't work in general: for a
 * bearing pointing roughly at a wide keep-out band (e.g. straight "south"
 * into the name/distance/chip stack, which spans nearly the puck's full
 * width), EVERY radius between the stack's near and far edge collides,
 * and the stack's far edge already sits close to the ring's own natural
 * radius — there is no inward radius left that both clears the stack and
 * stays a sane distance from center. Push-to-nearest-edge instead moves
 * the dot along whichever axis needs the LEAST displacement — which, for
 * a chip near the puck's lower edge, is usually "push it further out and
 * around", still comfortably inside the puck (clamped below by
 * FF_SCR_RADAR_DOT_MAX_CENTER_DIST_PX so a push can never place a dot
 * outside the puck's own circle). This is geometry, not domain logic —
 * no crew/position data is re-derived, only where an already-computed
 * dot is allowed to sit on screen.
 * ------------------------------------------------------------------- */
typedef struct {
    int32_t x1, y1, x2, y2;
} radar_rect_t;

#define FF_SCR_RADAR_MAX_KEEPOUT_RECTS 6
/* Furthest a dot's center may sit from the puck's own center — its own
 * half-size inside the puck radius, with a small extra margin — so a
 * collision push can never place any part of a dot outside the puck's
 * circular silhouette. */
#define FF_SCR_RADAR_DOT_MAX_CENTER_DIST_PX \
    ((float)FF_THEME_PUCK_RADIUS_PX - (float)FF_THEME_DOT_PX / 2.0f - 2.0f)

/* Pushes (dx,dy) out of every rectangle in `rects` it currently falls
 * inside (inflated by the dot's own half-size), along the nearest edge,
 * iterated a few passes to settle multi-rectangle cases; then clamps the
 * result to stay within the puck. */
static void radar_resolve_dot_collision(radar_rect_t const *rects, int n_rects, float *dx, float *dy)
{
    float half = (float)FF_THEME_DOT_PX / 2.0f;
    const float eps = 1.0f;

    for (int pass = 0; pass < 4; pass++) {
        bool moved = false;
        for (int i = 0; i < n_rects; i++) {
            float x1 = (float)rects[i].x1 - half;
            float x2 = (float)rects[i].x2 + half;
            float y1 = (float)rects[i].y1 - half;
            float y2 = (float)rects[i].y2 + half;
            if (*dx < x1 || *dx > x2 || *dy < y1 || *dy > y2) {
                continue; /* not inside this (inflated) rectangle */
            }

            float d_left = *dx - x1;
            float d_right = x2 - *dx;
            float d_top = *dy - y1;
            float d_bottom = y2 - *dy;
            float m = d_left;
            int dir = 0; /* 0=left 1=right 2=top 3=bottom */
            if (d_right < m) {
                m = d_right;
                dir = 1;
            }
            if (d_top < m) {
                m = d_top;
                dir = 2;
            }
            if (d_bottom < m) {
                m = d_bottom;
                dir = 3;
            }
            switch (dir) {
            case 0:
                *dx = x1 - eps;
                break;
            case 1:
                *dx = x2 + eps;
                break;
            case 2:
                *dy = y1 - eps;
                break;
            default:
                *dy = y2 + eps;
                break;
            }
            moved = true;
        }
        if (!moved) {
            break;
        }
    }

    float dist = sqrtf((*dx) * (*dx) + (*dy) * (*dy));
    if (dist > FF_SCR_RADAR_DOT_MAX_CENTER_DIST_PX) {
        float scale = FF_SCR_RADAR_DOT_MAX_CENTER_DIST_PX / dist;
        *dx *= scale;
        *dy *= scale;
    }
}

/* Crew ring: every dot ff_radar_compute produced, heading-relative,
 * dashed (outline-only) when that member's own freshness isn't LIVE.
 * Independent of `mode` (see ff_radar.h: dots are computed whenever a
 * bearing frame exists, regardless of the *selected* member's state).
 * `keepout`/`n_keepout` is this mode's fixed-chrome rectangle list (see
 * the keep-out policy comment above) — pass NULL/0 for none. */
static void radar_build_dots(lv_obj_t *parent, ff_radar_view_t const *r, radar_rect_t const *keepout, int n_keepout)
{
    for (uint8_t i = 0; i < r->n_dots; i++) {
        ff_radar_dot_t const *d = &r->dots[i];
        float dx, dy;
        radar_deg_to_offset(d->ring_deg, (float)FF_THEME_RING_RADIUS_PX, &dx, &dy);
        radar_resolve_dot_collision(keepout, n_keepout, &dx, &dy);

        lv_obj_t *dot = lv_obj_create(parent);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, FF_THEME_DOT_PX, FF_THEME_DOT_PX);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE); /* indicator only in this slice */

        uint32_t crew_hex = ff_theme_crew_color(d->color_idx);
        if (d->stale) {
            /* Outline-only "ghost" — readable as "not current" without
             * needing to read any text (ux-raver honesty-read check). */
            lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(dot, 2, 0);
            lv_obj_set_style_border_color(dot, lv_color_hex(crew_hex), 0);
            lv_obj_set_style_border_opa(dot, LV_OPA_70, 0);
        } else {
            lv_obj_set_style_bg_color(dot, lv_color_hex(crew_hex), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        }
        lv_obj_align(dot, LV_ALIGN_CENTER, (int32_t)dx, (int32_t)dy);

        if (d->initial != '\0') {
            char ch[2] = {d->initial, '\0'};
            lv_obj_t *label = lv_label_create(dot);
            lv_label_set_text(label, ch);
            lv_obj_set_style_text_font(label, FF_THEME_FONT_LABEL, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(d->stale ? crew_hex : FF_THEME_COLOR_BG), 0);
            lv_obj_center(label);
        }
    }
}

/* One straight lv_line segment from (from_dx,from_dy) to (to_dx,to_dy),
 * both expressed relative to the puck's own center, at `opa` opacity.
 *
 * lv_line draws each point at `object_top_left + point` (see
 * lvgl/src/widgets/line/lv_line.c's LV_EVENT_DRAW_MAIN handler) — it does
 * NOT auto-center a bounding box of arbitrary/negative points the way an
 * `lv_obj_align`'d fixed-size widget would. So every line object here is
 * given an EXPLICIT size equal to the whole puck and pinned to (0,0)
 * (`parent`'s own top-left, which — since `parent` is a zero-padding
 * tileview tile sized to the puck, see scr_nav.c — coincides with the
 * puck's top-left); every point is then offset by the puck's half-size so
 * "center-relative (0,0)" lands exactly on the puck's visual center. */
static void radar_draw_segment(lv_obj_t *parent, float from_dx, float from_dy, float to_dx, float to_dy,
                                uint32_t color_hex, lv_opa_t opa, int32_t width)
{
    const int32_t half = FF_THEME_PUCK_PX / 2;

    lv_point_precise_t *pts = radar_alloc_line_pts();
    pts[0].x = half + (int32_t)from_dx;
    pts[0].y = half + (int32_t)from_dy;
    pts[1].x = half + (int32_t)to_dx;
    pts[1].y = half + (int32_t)to_dy;

    lv_obj_t *line = lv_line_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_set_pos(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_line_set_points(line, pts, 2);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(color_hex), 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_set_style_line_opa(line, opa, 0);
}

/* ---------------------------------------------------------------------
 * Filled/outline triangle (arrowhead).
 *
 * LVGL has no filled-polygon widget, so this uses a plain lv_obj plus a
 * custom LV_EVENT_DRAW_MAIN callback that calls the low-level
 * lv_draw_triangle() API directly (lvgl.h -> core/lv_obj.h ->
 * lv_obj_draw.h -> draw/lv_draw_triangle.h — already transitively
 * visible, no extra include needed). Same static-storage-pool lifetime
 * pattern, and the SAME re-render/teardown hazard, as the line-point pool
 * above — see that comment and https://github.com/jakeholland/firefly/issues/17.
 * ------------------------------------------------------------------- */
#define FF_SCR_RADAR_MAX_TRIANGLES 2
typedef struct {
    lv_point_precise_t p[3];
    lv_color_t color;
    lv_opa_t opa;
} radar_tri_desc_t;
static radar_tri_desc_t s_tri_descs[FF_SCR_RADAR_MAX_TRIANGLES];
static int s_tri_desc_next;

static radar_tri_desc_t *radar_alloc_tri_desc(void)
{
    if (s_tri_desc_next >= FF_SCR_RADAR_MAX_TRIANGLES) {
        s_tri_desc_next = 0; /* defensive wrap: never index out of bounds */
    }
    return &s_tri_descs[s_tri_desc_next++];
}

static void radar_triangle_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    radar_tri_desc_t *td = (radar_tri_desc_t *)lv_event_get_user_data(e);
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

/* A filled triangle at (p0,p1,p2), each expressed relative to the puck's
 * own center — same convention and same "explicit full-puck-size object
 * pinned at (0,0)" positioning trick as radar_draw_segment (see its doc
 * comment for why: this repo's chrome objects don't rely on LVGL
 * auto-centering an arbitrary/negative-coordinate bounding box). */
static void radar_draw_filled_triangle(lv_obj_t *parent, float p0x, float p0y, float p1x, float p1y, float p2x,
                                        float p2y, uint32_t color_hex, lv_opa_t opa)
{
    const int32_t half = FF_THEME_PUCK_PX / 2;

    radar_tri_desc_t *td = radar_alloc_tri_desc();
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
    lv_obj_add_event_cb(obj, radar_triangle_draw_cb, LV_EVENT_DRAW_MAIN, td);
}

/* Outline-only triangle: three thin segments, no fill — the LOST "ghost"
 * arrowhead (PR #16 UX review finding #1's ruling: "arrow reduced to a
 * faint outline-only ghost (no fill)"). A shape distinct in KIND from
 * LIVE/STALE's solid filled head, not just a dimmer copy of the same
 * silhouette — that's the point: STALE and LOST must read as different
 * screens, not different opacities of the same screen. */
static void radar_draw_outline_triangle(lv_obj_t *parent, float p0x, float p0y, float p1x, float p1y, float p2x,
                                         float p2y, uint32_t color_hex, lv_opa_t opa)
{
    radar_draw_segment(parent, p0x, p0y, p1x, p1y, color_hex, opa, 3);
    radar_draw_segment(parent, p1x, p1y, p2x, p2y, color_hex, opa, 3);
    radar_draw_segment(parent, p2x, p2y, p0x, p0y, color_hex, opa, 3);
}

typedef enum {
    RADAR_ARROW_SOLID,  /* LIVE: solid tail + filled head, full opacity */
    RADAR_ARROW_DASHED, /* STALE: dashed tail + filled head, reduced opacity */
    RADAR_ARROW_GHOST,  /* LOST (real fix): faint dashed tail + OUTLINE-ONLY head */
} radar_arrow_style_t;

/* Tapered arrow — PR #16 UX review finding #2: a shaft+circle "doesn't
 * read as an arrow" ("a ball doesn't have a pointy end"). Replaced with a
 * real kite/chevron: a thin tail from puck-center out to the head's base,
 * then a triangular head from that base to the tip at `deg`. `style`
 * controls the tail's dash pattern and whether the head is filled or
 * outline-only (see radar_arrow_style_t). Direction convention matches
 * radar_deg_to_offset (0 deg = up/ahead, clockwise). */
static void radar_draw_arrow(lv_obj_t *parent, float deg, uint32_t color_hex, lv_opa_t opa, radar_arrow_style_t style)
{
    float rad = deg * (FF_SCR_RADAR_PI / 180.0f);
    float fx = sinf(rad);  /* forward unit vector (toward the tip) */
    float fy = -cosf(rad);
    float px = cosf(rad); /* perpendicular unit vector (the head's base spread) */
    float py = sinf(rad);

    float tip_r = (float)FF_THEME_ARROW_LEN_PX;
    float base_r = tip_r - (float)FF_THEME_ARROW_HEAD_LEN_PX;
    float half_w = (float)FF_THEME_ARROW_HEAD_WIDTH_PX / 2.0f;

    float base_x = fx * base_r, base_y = fy * base_r;
    float tip_x = fx * tip_r, tip_y = fy * tip_r;
    float left_x = base_x - px * half_w, left_y = base_y - py * half_w;
    float right_x = base_x + px * half_w, right_y = base_y + py * half_w;

    if (style == RADAR_ARROW_SOLID) {
        radar_draw_segment(parent, 0.0f, 0.0f, base_x, base_y, color_hex, opa, 6);
    } else {
        enum { N_DASHES = 3 };
        for (int i = 0; i < N_DASHES; i++) {
            float t0 = ((float)i + 0.15f) / (float)N_DASHES;
            float t1 = ((float)i + 0.65f) / (float)N_DASHES;
            radar_draw_segment(parent, base_x * t0, base_y * t0, base_x * t1, base_y * t1, color_hex, opa, 5);
        }
    }

    if (style == RADAR_ARROW_GHOST) {
        radar_draw_outline_triangle(parent, tip_x, tip_y, left_x, left_y, right_x, right_y, color_hex, opa);
    } else {
        radar_draw_filled_triangle(parent, tip_x, tip_y, left_x, left_y, right_x, right_y, color_hex, opa);
    }
}

static lv_obj_t *radar_build_name_label(lv_obj_t *parent, char const *name, int32_t dy)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, (name != NULL && name[0] != '\0') ? name : "(unnamed)");
    lv_obj_set_style_text_font(label, FF_THEME_FONT_NAME, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, dy);
    return label;
}

static lv_obj_t *radar_build_distance_label(lv_obj_t *parent, char const *dist_str, int32_t dy)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, (dist_str != NULL && dist_str[0] != '\0') ? dist_str : "-- m");
    lv_obj_set_style_text_font(label, FF_THEME_FONT_DISTANCE, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, dy);
    return label;
}

/* ---------------------------------------------------------------------
 * Per-mode renderers.
 *
 * Vertical positions are named constants, not inline magic numbers,
 * specifically so `ff_scr_radar_build`'s keep-out rectangles (which must
 * describe the SAME regions these functions actually draw into) are
 * built from the same source of truth instead of independently-guessed
 * duplicate literals that could silently drift apart (UX review finding
 * #3: "implement a keep-out layout rule, not one-off nudges").
 * ------------------------------------------------------------------- */

/* LIVE / STALE / LOST-with-a-real-fix share one vertical stack. */
#define RADAR_STACK_NAME_DY 60
#define RADAR_STACK_DIST_DY 100
#define RADAR_STACK_CHIP_DY 148

/* LOST, never fixed. */
#define RADAR_NEVER_HEADLINE_DY (-10)
#define RADAR_NEVER_NAME_DY 40
#define RADAR_NEVER_SUB_DY 90

/* NOFIX. */
#define RADAR_NOFIX_HEADLINE_DY (-10)
#define RADAR_NOFIX_SUB_DY 40
#define RADAR_NOFIX_CHIP_DY 80

/* NOSEL. */
#define RADAR_NOSEL_HEADLINE_DY (-10)
#define RADAR_NOSEL_SUB_DY 40

/* CLOSE — restructured per UX review findings #3(a)/(b): the FLARE
 * button no longer overlaps a ring dot at any bearing (handled by the
 * generic keep-out mechanism below, not a one-off nudge), and the name
 * label is pushed below the outer ring's radius instead of sitting
 * inside it. */
#define RADAR_CLOSE_RING_CY (-55)
#define RADAR_CLOSE_NAME_DY 59
#define RADAR_CLOSE_CHIP_DY 98
#define RADAR_CLOSE_FLARE_DY 147

static void radar_render_live(lv_obj_t *parent, ff_radar_view_t const *r)
{
    radar_draw_arrow(parent, r->arrow_deg, FF_THEME_COLOR_AMBER, LV_OPA_COVER, RADAR_ARROW_SOLID);
    radar_build_name_label(parent, r->name, RADAR_STACK_NAME_DY);
    radar_build_distance_label(parent, r->dist_str, RADAR_STACK_DIST_DY);
    radar_make_chip(parent, "LIVE", FF_THEME_COLOR_LIVE_GREEN, FF_THEME_COLOR_BG, RADAR_STACK_CHIP_DY);
}

static void radar_render_stale(lv_obj_t *parent, ff_radar_view_t const *r)
{
    radar_build_rim_tint(parent, FF_THEME_COLOR_STALE_AMBER, LV_OPA_50);
    /* S06 spec: "dashed arrow at 28% opacity". LV_OPA values are 0-255;
     * 28% of 255 rounds to 71. */
    radar_draw_arrow(parent, r->arrow_deg, FF_THEME_COLOR_STALE_AMBER, 71, RADAR_ARROW_DASHED);
    radar_build_name_label(parent, r->name, RADAR_STACK_NAME_DY);
    radar_build_distance_label(parent, r->dist_str, RADAR_STACK_DIST_DY);

    char chip_text[40];
    snprintf(chip_text, sizeof(chip_text), "LAST SEEN %s", r->age_str);
    radar_make_chip(parent, chip_text, FF_THEME_COLOR_STALE_AMBER, FF_THEME_COLOR_BG, RADAR_STACK_CHIP_DY);
}

static void radar_render_lost(lv_obj_t *parent, ff_radar_view_t const *r)
{
    /* RENDERER CONTRACT (ff_radar.h): mode == RADAR_LOST alone doesn't
     * distinguish "genuinely old fix" from "never fixed" — key off
     * age_str, not mode, per the header's explicit instruction. */
    bool never_fixed = (r->age_str[0] == '\0');

    if (!never_fixed) {
        /* ORCHESTRATOR RULING (PR #16 UX review finding #1, follow-up):
         * LOST must differ from STALE in KIND, not degree — at a
         * 2-second strobe-lit glance it has to read as a different
         * screen, not a dimmer one. "STALE says this is a few minutes
         * old, LOST says do not trust this." Four structural differences
         * from STALE, not one opacity tweak:
         *   1. NO rim tint at all (STALE gets one; LOST gets none).
         *   2. The arrowhead is OUTLINE-ONLY (RADAR_ARROW_GHOST) — an
         *      unfilled sketch, not a dimmer copy of STALE's filled
         *      triangle. A different silhouette, not just a different
         *      opacity.
         *   3. Distance is "~"-prefixed (like CLOSE's "~15 m") to signal
         *      imprecision — this position is old enough we're no longer
         *      claiming it's exact.
         *   4. The chip is a dark/muted pill ("give up on" language),
         *      not STALE's bright amber pill ("aging but plausible").
         */
        /* ~22% opacity: dimmer than STALE's 28% (71/255). The outline-
         * only shape above already carries most of the "different kind"
         * signal; opacity is a secondary cue, not the primary one this
         * time (that was the whole bug last round). */
        radar_draw_arrow(parent, r->arrow_deg, FF_THEME_COLOR_MUTED, 56, RADAR_ARROW_GHOST);
        radar_build_name_label(parent, r->name, RADAR_STACK_NAME_DY);

        char lost_dist[24];
        snprintf(lost_dist, sizeof(lost_dist), "~%s", (r->dist_str[0] != '\0') ? r->dist_str : "?");
        radar_build_distance_label(parent, lost_dist, RADAR_STACK_DIST_DY);

        char chip_text[40];
        snprintf(chip_text, sizeof(chip_text), "LAST SEEN %s", r->age_str);
        radar_make_chip(parent, chip_text, FF_THEME_COLOR_DIM, FF_THEME_COLOR_INK, RADAR_STACK_CHIP_DY);
    } else {
        /* Never had a fix at all: arrow_valid is already false (nothing
         * to point at, honestly) — no arrow, no rim tint, no invented
         * distance. A distinct headline so this is never confused with a
         * "LAST SEEN" reading (CLAUDE.md honesty rule; PR #13 review
         * finding #2). */
        lv_obj_t *headline = lv_label_create(parent);
        lv_label_set_text(headline, "NO FIX YET");
        lv_obj_set_style_text_font(headline, FF_THEME_FONT_HEADLINE, 0);
        lv_obj_set_style_text_color(headline, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
        lv_obj_align(headline, LV_ALIGN_CENTER, 0, RADAR_NEVER_HEADLINE_DY);

        radar_build_name_label(parent, r->name, RADAR_NEVER_NAME_DY);

        lv_obj_t *sub = lv_label_create(parent);
        lv_label_set_text(sub, "Waiting for their first GPS fix");
        lv_obj_set_style_text_font(sub, FF_THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_color(sub, lv_color_hex(FF_THEME_COLOR_DIM), 0);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, RADAR_NEVER_SUB_DY);
    }
}

/* TODO(S10): wire to the real flare-send flow once it exists. This PR
 * only reserves the callback hook, per docs/specs/S06-radar-face.md
 * slice d ("FLARE button... fires S10 callback"). */
static void radar_flare_stub_cb(lv_event_t *e)
{
    (void)e;
}

/* lv_anim_exec_xcb_t is `void(*)(void*, int32_t)`; lv_obj_set_style_opa
 * takes a 3rd (selector) argument, so it can't be used directly as an
 * exec callback (a raw function-pointer cast that drops an argument is
 * undefined behavior) — this thin wrapper supplies LV_PART_MAIN. */
static void radar_anim_set_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void radar_render_close(lv_obj_t *parent, ff_radar_view_t const *r)
{
    /* Three pulsing rings (S06: "LVGL anim, 1.2 s period"). Headless
     * single-frame capture never runs the animation timer, so every
     * golden deterministically shows animation-start state — see this
     * function's anim setup below. Radii/center shrunk and shifted up
     * from the original slice (UX review finding #4): the outer ring
     * used to reach past RADAR_STACK_NAME_DY's old position and cut
     * through the name text every pulse. */
    static const int32_t ring_radii[3] = {38, 64, 90};
    static const lv_opa_t ring_opa[3] = {LV_OPA_80, LV_OPA_50, LV_OPA_20};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *ring = lv_obj_create(parent);
        lv_obj_remove_style_all(ring);
        lv_obj_set_size(ring, ring_radii[i] * 2, ring_radii[i] * 2);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 3, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(FF_THEME_COLOR_LIVE_GREEN), 0);
        lv_obj_set_style_border_opa(ring, ring_opa[i], 0);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(ring, LV_ALIGN_CENTER, 0, RADAR_CLOSE_RING_CY);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ring);
        lv_anim_set_exec_cb(&a, radar_anim_set_opa_cb);
        lv_anim_set_values(&a, ring_opa[i], 0);
        lv_anim_set_duration(&a, 1200); /* S06 spec: "LVGL anim, 1.2 s period" */
        lv_anim_set_reverse_duration(&a, 1200);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_delay(&a, (uint32_t)(i * 150));
        lv_anim_start(&a);
    }

    char big_dist[24];
    snprintf(big_dist, sizeof(big_dist), "~%s", (r->dist_str[0] != '\0') ? r->dist_str : "?");
    radar_build_distance_label(parent, big_dist, RADAR_CLOSE_RING_CY);

    radar_build_name_label(parent, r->name, RADAR_CLOSE_NAME_DY);

    char const *trend_text = "STEADY";
    uint32_t trend_color = FF_THEME_COLOR_MUTED;
    if (r->trend > 0) {
        trend_text = "GETTING CLOSER";
        trend_color = FF_THEME_COLOR_LIVE_GREEN;
    } else if (r->trend < 0) {
        trend_text = "GETTING FARTHER";
        trend_color = FF_THEME_COLOR_STALE_AMBER;
    }
    radar_make_chip(parent, trend_text, trend_color, FF_THEME_COLOR_BG, RADAR_CLOSE_CHIP_DY);

    /* FLARE button: S06 spec "48 px high, full hit area" — also clears
     * docs/review/ux-raver.md's >=44px tap-target floor with margin. */
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 200, FF_THEME_FLARE_BTN_H_PX);
    lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, RADAR_CLOSE_FLARE_DY);
    lv_obj_add_event_cb(btn, radar_flare_stub_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "FLARE");
    lv_obj_set_style_text_font(btn_label, FF_THEME_FONT_NAME, 0);
    lv_obj_set_style_text_color(btn_label, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_center(btn_label);
}

static void radar_render_nofix(lv_obj_t *parent, ff_radar_view_t const *r)
{
    lv_obj_t *headline = lv_label_create(parent);
    /* S06 spec literally writes "NO FIX · RADIO ONLY" (U+00B7 MIDDLE DOT),
     * but LVGL's built-in Montserrat bitmap fonts only cover the ASCII
     * printable range by default (no supplemental-Latin glyphs compiled
     * in) — that codepoint renders as a tofu/replacement box, not a dot.
     * Substituted with a plain hyphen, which reads the same way and is
     * guaranteed renderable without pulling in a wider (larger) font
     * subset for one punctuation mark. */
    lv_label_set_text(headline, "NO FIX - RADIO ONLY");
    lv_obj_set_style_text_font(headline, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(headline, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_width(headline, 320);
    lv_obj_set_style_text_align(headline, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(headline, LV_ALIGN_CENTER, 0, RADAR_NOFIX_HEADLINE_DY);

    if (r->name[0] != '\0') {
        char sub[40];
        snprintf(sub, sizeof(sub), "Looking for %s", r->name);
        lv_obj_t *sub_lbl = lv_label_create(parent);
        lv_label_set_text(sub_lbl, sub);
        lv_obj_set_style_text_font(sub_lbl, FF_THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_color(sub_lbl, lv_color_hex(FF_THEME_COLOR_DIM), 0);
        lv_obj_align(sub_lbl, LV_ALIGN_CENTER, 0, RADAR_NOFIX_SUB_DY);
    }

    /* Honest extra: NOFIX means *my* fix/heading is unusable, but the
     * selected member's own last-known age doesn't depend on that (see
     * ff_radar.h's doc comment) — surface it if we have it, rather than
     * silently dropping data the compute layer went out of its way to
     * still report. */
    if (r->age_str[0] != '\0') {
        char chip_text[40];
        snprintf(chip_text, sizeof(chip_text), "LAST KNOWN %s", r->age_str);
        radar_make_chip(parent, chip_text, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_MUTED, RADAR_NOFIX_CHIP_DY);
    }
}

static void radar_render_nosel(lv_obj_t *parent)
{
    lv_obj_t *headline = lv_label_create(parent);
    lv_label_set_text(headline, "NO CREW SELECTED");
    lv_obj_set_style_text_font(headline, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(headline, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_align(headline, LV_ALIGN_CENTER, 0, RADAR_NOSEL_HEADLINE_DY);

    lv_obj_t *sub = lv_label_create(parent);
    lv_label_set_text(sub, "Pair a friend in Settings");
    lv_obj_set_style_text_font(sub, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, RADAR_NOSEL_SUB_DY);
}

/* ---------------------------------------------------------------------
 * Entry point.
 * ------------------------------------------------------------------- */

void ff_scr_radar_build(lv_obj_t *parent, ff_radar_view_t const *radar)
{
    if (parent == NULL || radar == NULL) {
        return;
    }

    s_line_pt_next = 0;
    s_tri_desc_next = 0;

    radar_build_status_bar(parent, radar);

    /* Keep-out rectangles for this mode's fixed chrome, generous rather
     * than pixel-exact, built from the SAME named dy constants the
     * render functions below actually draw at (see the per-mode-renderer
     * section's top comment) — see radar_build_dots's doc comment for
     * the pull-in algorithm these feed. */
    bool never_fixed = (radar->mode == RADAR_LOST && radar->age_str[0] == '\0');
    radar_rect_t keepout[FF_SCR_RADAR_MAX_KEEPOUT_RECTS];
    int n_keepout = 0;
    /* Status bar (clock/mesh/battery, radar_build_status_bar, all at
     * dy=-195): cross-mode chrome, always present, so its keep-out
     * applies to every mode — a dot at ring_deg near 0 (straight "up")
     * would otherwise land right on top of it (caught while building
     * tests/fixtures/radar_close_collision.json, not by the reviewer —
     * one more worst-case bearing this fixture now exercises). */
    keepout[n_keepout++] = (radar_rect_t){-120, -212, 120, -178};
    /* Page-dot row (scr_nav.c's nav_build_page_dots, at dy=186 on the
     * puck): also cross-mode chrome, drawn by the shell on top of
     * whichever tile this screen renders into. Found while eyeballing
     * tests/fixtures/radar_close_collision.json's own worst-case
     * bearings — small (10px, purely navigational, not information) but
     * cheap to protect using the exact same mechanism as everything
     * else here. */
    keepout[n_keepout++] = (radar_rect_t){-70, 176, 70, 196};
    switch (radar->mode) {
    case RADAR_LIVE:
    case RADAR_STALE:
        /* name/distance/chip stack: RADAR_STACK_NAME_DY..RADAR_STACK_CHIP_DY,
         * generous margin on every side. */
        keepout[n_keepout++] = (radar_rect_t){-140, RADAR_STACK_NAME_DY - 20, 140, RADAR_STACK_CHIP_DY + 24};
        break;
    case RADAR_LOST:
        if (never_fixed) {
            keepout[n_keepout++] = (radar_rect_t){-170, RADAR_NEVER_HEADLINE_DY - 18, 170, RADAR_NEVER_SUB_DY + 15};
        } else {
            keepout[n_keepout++] = (radar_rect_t){-140, RADAR_STACK_NAME_DY - 20, 140, RADAR_STACK_CHIP_DY + 24};
        }
        break;
    case RADAR_CLOSE:
        /* Rings + big distance (rings reach RADAR_CLOSE_RING_CY +/- the
         * outer radius, 90). */
        keepout[n_keepout++] =
            (radar_rect_t){-100, RADAR_CLOSE_RING_CY - 95, 100, RADAR_CLOSE_RING_CY + 95};
        /* Name + trend chip. */
        keepout[n_keepout++] = (radar_rect_t){-110, RADAR_CLOSE_NAME_DY - 15, 110, RADAR_CLOSE_CHIP_DY + 20};
        /* FLARE button (this is the specific overlap UX review finding
         * #3(a) reported — "FLARE button no longer swallows the M dot"). */
        keepout[n_keepout++] = (radar_rect_t){-104, RADAR_CLOSE_FLARE_DY - 26, 104, RADAR_CLOSE_FLARE_DY + 26};
        break;
    case RADAR_NOFIX:
        keepout[n_keepout++] = (radar_rect_t){-170, RADAR_NOFIX_HEADLINE_DY - 18, 170, RADAR_NOFIX_CHIP_DY + 20};
        break;
    case RADAR_NOSEL:
    default:
        keepout[n_keepout++] = (radar_rect_t){-170, RADAR_NOSEL_HEADLINE_DY - 18, 170, RADAR_NOSEL_SUB_DY + 15};
        break;
    }

    radar_build_dots(parent, radar, keepout, n_keepout);

    switch (radar->mode) {
    case RADAR_LIVE:
        radar_render_live(parent, radar);
        break;
    case RADAR_STALE:
        radar_render_stale(parent, radar);
        break;
    case RADAR_LOST:
        radar_render_lost(parent, radar);
        break;
    case RADAR_CLOSE:
        radar_render_close(parent, radar);
        break;
    case RADAR_NOFIX:
        radar_render_nofix(parent, radar);
        break;
    case RADAR_NOSEL:
    default:
        radar_render_nosel(parent);
        break;
    }
}
