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
 * radar screen per process (targets/sim/main.c loads at most one fixture
 * per run/window session), so a small static pool, reset at the top of
 * every ff_scr_radar_build() call, is enough and needs no lifetime
 * bookkeeping beyond that.
 * ------------------------------------------------------------------- */
#define FF_SCR_RADAR_MAX_LINE_SEGMENTS 8
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
    lv_obj_set_style_text_color(mesh_lbl, lv_color_hex(r->mesh_ok ? FF_THEME_COLOR_LIVE_GREEN : FF_THEME_COLOR_DIM), 0);
    lv_obj_align(mesh_lbl, LV_ALIGN_CENTER, 0, -195);

    lv_obj_t *batt_lbl = lv_label_create(parent);
    if (r->batt_pct < 0) {
        snprintf(buf, sizeof(buf), "--%%");
    } else {
        snprintf(buf, sizeof(buf), "%d%%", (int)r->batt_pct);
    }
    lv_label_set_text(batt_lbl, buf);
    lv_obj_set_style_text_font(batt_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(batt_lbl, lv_color_hex(r->batt_pct >= 0 && r->batt_pct <= 15 ? FF_THEME_COLOR_STALE_AMBER
                                                                                              : FF_THEME_COLOR_MUTED),
                                 0);
    lv_obj_align(batt_lbl, LV_ALIGN_CENTER, 78, -195);
}

/* Crew ring: every dot ff_radar_compute produced, heading-relative,
 * dashed (outline-only) when that member's own freshness isn't LIVE.
 * Independent of `mode` (see ff_radar.h: dots are computed whenever a
 * bearing frame exists, regardless of the *selected* member's state). */
static void radar_build_dots(lv_obj_t *parent, ff_radar_view_t const *r)
{
    for (uint8_t i = 0; i < r->n_dots; i++) {
        ff_radar_dot_t const *d = &r->dots[i];
        float dx, dy;
        radar_deg_to_offset(d->ring_deg, (float)FF_THEME_RING_RADIUS_PX, &dx, &dy);

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

/* Arrow shaft + head, from puck-center toward `deg`, `opa`-strength.
 * `dashed`: several short segments with gaps instead of one solid line
 * (STALE/LOST — a visibly different silhouette from LIVE's solid arrow
 * at a glance, not just a dimmer copy of it). */
static void radar_draw_arrow(lv_obj_t *parent, float deg, uint32_t color_hex, lv_opa_t opa, bool dashed)
{
    float tip_dx, tip_dy;
    radar_deg_to_offset(deg, (float)FF_THEME_ARROW_LEN_PX, &tip_dx, &tip_dy);

    if (!dashed) {
        radar_draw_segment(parent, 0.0f, 0.0f, tip_dx, tip_dy, color_hex, opa, 10);
    } else {
        enum { N_DASHES = 4 };
        for (int i = 0; i < N_DASHES; i++) {
            float t0 = ((float)i + 0.15f) / (float)N_DASHES;
            float t1 = ((float)i + 0.65f) / (float)N_DASHES;
            radar_draw_segment(parent, tip_dx * t0, tip_dy * t0, tip_dx * t1, tip_dy * t1, color_hex, opa, 8);
        }
    }

    /* Head: a filled circle at the tip — clearer at arm's length than a
     * thin line terminus alone (ux-raver 2-second test). */
    lv_obj_t *head = lv_obj_create(parent);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, FF_THEME_ARROW_HEAD_PX, FF_THEME_ARROW_HEAD_PX);
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(head, lv_color_hex(color_hex), 0);
    lv_obj_set_style_bg_opa(head, opa, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(head, LV_ALIGN_CENTER, (int32_t)tip_dx, (int32_t)tip_dy);
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
 * ------------------------------------------------------------------- */

static void radar_render_live(lv_obj_t *parent, ff_radar_view_t const *r)
{
    radar_draw_arrow(parent, r->arrow_deg, FF_THEME_COLOR_AMBER, LV_OPA_COVER, /*dashed=*/false);
    radar_build_name_label(parent, r->name, 60);
    radar_build_distance_label(parent, r->dist_str, 100);
    radar_make_chip(parent, "LIVE", FF_THEME_COLOR_LIVE_GREEN, FF_THEME_COLOR_BG, 148);
}

static void radar_render_stale(lv_obj_t *parent, ff_radar_view_t const *r)
{
    radar_build_rim_tint(parent, FF_THEME_COLOR_STALE_AMBER, LV_OPA_50);
    /* S06 spec: "dashed arrow at 28% opacity". LV_OPA values are 0-255;
     * 28% of 255 rounds to 71. */
    radar_draw_arrow(parent, r->arrow_deg, FF_THEME_COLOR_STALE_AMBER, 71, /*dashed=*/true);
    radar_build_name_label(parent, r->name, 60);
    radar_build_distance_label(parent, r->dist_str, 100);

    char chip_text[40];
    snprintf(chip_text, sizeof(chip_text), "LAST SEEN %s", r->age_str);
    radar_make_chip(parent, chip_text, FF_THEME_COLOR_STALE_AMBER, FF_THEME_COLOR_BG, 148);
}

static void radar_render_lost(lv_obj_t *parent, ff_radar_view_t const *r)
{
    /* RENDERER CONTRACT (ff_radar.h): mode == RADAR_LOST alone doesn't
     * distinguish "genuinely old fix" from "never fixed" — key off
     * age_str, not mode, per the header's explicit instruction. */
    bool never_fixed = (r->age_str[0] == '\0');

    if (!never_fixed) {
        /* Genuinely old fix: same dashed-arrow treatment as STALE but
         * dimmer and greyer — "same but stronger" staleness signal
         * (coordinator brief), not a brighter/bolder arrow. Muted, not
         * amber: amber still reads as "aging but plausible", grey reads
         * as "given up on". */
        radar_build_rim_tint(parent, FF_THEME_COLOR_MUTED, LV_OPA_40);
        /* Dimmer than STALE's 28% (still a visible, deliberate dashed
         * shape, not near-invisible — ux-raver's honesty-read check
         * wants "can I tell fresh from stale from lost", not "is there
         * anything here at all"). */
        radar_draw_arrow(parent, r->arrow_deg, FF_THEME_COLOR_MUTED, 56 /* ~22% */, /*dashed=*/true);
        radar_build_name_label(parent, r->name, 60);
        radar_build_distance_label(parent, r->dist_str, 100);

        char chip_text[40];
        snprintf(chip_text, sizeof(chip_text), "LAST SEEN %s", r->age_str);
        radar_make_chip(parent, chip_text, FF_THEME_COLOR_DIM, FF_THEME_COLOR_INK, 148);
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
        lv_obj_align(headline, LV_ALIGN_CENTER, 0, -10);

        radar_build_name_label(parent, r->name, 40);

        lv_obj_t *sub = lv_label_create(parent);
        lv_label_set_text(sub, "Waiting for their first GPS fix");
        lv_obj_set_style_text_font(sub, FF_THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_color(sub, lv_color_hex(FF_THEME_COLOR_DIM), 0);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, 90);
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
     * function's anim setup below. */
    static const int32_t ring_radii[3] = {56, 92, 128};
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
        lv_obj_align(ring, LV_ALIGN_CENTER, 0, -10);

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
    radar_build_distance_label(parent, big_dist, -10);

    radar_build_name_label(parent, r->name, 40);

    char const *trend_text = "STEADY";
    uint32_t trend_color = FF_THEME_COLOR_MUTED;
    if (r->trend > 0) {
        trend_text = "GETTING CLOSER";
        trend_color = FF_THEME_COLOR_LIVE_GREEN;
    } else if (r->trend < 0) {
        trend_text = "GETTING FARTHER";
        trend_color = FF_THEME_COLOR_STALE_AMBER;
    }
    radar_make_chip(parent, trend_text, trend_color, FF_THEME_COLOR_BG, 78);

    /* FLARE button: S06 spec "48 px high, full hit area" — also clears
     * docs/review/ux-raver.md's >=44px tap-target floor with margin. */
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 200, FF_THEME_FLARE_BTN_H_PX);
    lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 145);
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
    lv_obj_align(headline, LV_ALIGN_CENTER, 0, -10);

    if (r->name[0] != '\0') {
        char sub[40];
        snprintf(sub, sizeof(sub), "Looking for %s", r->name);
        lv_obj_t *sub_lbl = lv_label_create(parent);
        lv_label_set_text(sub_lbl, sub);
        lv_obj_set_style_text_font(sub_lbl, FF_THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_color(sub_lbl, lv_color_hex(FF_THEME_COLOR_DIM), 0);
        lv_obj_align(sub_lbl, LV_ALIGN_CENTER, 0, 40);
    }

    /* Honest extra: NOFIX means *my* fix/heading is unusable, but the
     * selected member's own last-known age doesn't depend on that (see
     * ff_radar.h's doc comment) — surface it if we have it, rather than
     * silently dropping data the compute layer went out of its way to
     * still report. */
    if (r->age_str[0] != '\0') {
        char chip_text[40];
        snprintf(chip_text, sizeof(chip_text), "LAST KNOWN %s", r->age_str);
        radar_make_chip(parent, chip_text, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_MUTED, 80);
    }
}

static void radar_render_nosel(lv_obj_t *parent)
{
    lv_obj_t *headline = lv_label_create(parent);
    lv_label_set_text(headline, "NO CREW SELECTED");
    lv_obj_set_style_text_font(headline, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(headline, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_align(headline, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *sub = lv_label_create(parent);
    lv_label_set_text(sub, "Pair a friend in Settings");
    lv_obj_set_style_text_font(sub, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 40);
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

    radar_build_status_bar(parent, radar);
    radar_build_dots(parent, radar);

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
