/**
 * scr_flare.c — see scr_flare.h.
 */
#include "scr_flare.h"

#include <stdio.h>

#include "ff_theme.h"
#include "flare_fmt.h"

/* ---------------------------------------------------------------------
 * Layout constants. Deliberately local to this file (not radar_layout.h)
 * — see scr_flare.h's ff_scr_flare_build_lock_chip doc comment for why
 * these never need to compete for space in that module's collision
 * registry.
 * ------------------------------------------------------------------- */

/* Kept well inside the puck's circular silhouette at this cy (the
 * largest ring's top edge must stay within FF_THEME_PUCK_RADIUS_PX of
 * center — LVGL doesn't clip children to a parent's rounded/circular
 * shape by default, so a ring sized past that boundary would visibly
 * poke outside the puck's drawn edge). */
#define FLARE_TAKEOVER_RING_CY (-120.0f)
static const int32_t FLARE_TAKEOVER_RING_RADII[3] = {26, 45, 62};
static const lv_opa_t FLARE_TAKEOVER_RING_OPA[3] = {LV_OPA_80, LV_OPA_50, LV_OPA_20};

#define FLARE_TAKEOVER_HEADLINE_DY (-62.0f)
#define FLARE_TAKEOVER_BEARING_DY  (-22.0f)
#define FLARE_TAKEOVER_EXPLAIN_DY  20.0f
#define FLARE_TAKEOVER_GO_DY       98.0f
#define FLARE_TAKEOVER_DISMISS_DY  160.0f
#define FLARE_TAKEOVER_BTN_W       190
/* >= FF_THEME_MIN_HIT_PX (44) with real margin — docs/review/ux-raver.md
 * checklist item 2, "fat thumb test": this screen shows up at 2 AM with
 * one thumb and possibly gloves, so both buttons get MORE than the bare
 * floor, not exactly it. */
#define FLARE_TAKEOVER_GO_BTN_H       56
#define FLARE_TAKEOVER_DISMISS_BTN_H  50

/* Kept clear of both NOSEL's "Pair a friend in Settings" sub-line
 * (RADAR_LAYOUT_NOSEL_SUB_DY == 40) above and the puck's own bottom edge
 * (FF_THEME_PUCK_RADIUS_PX == 220) below — verified against an actual
 * headless render, not just arithmetic, per this repo's screenshot-review
 * habit (see radar_layout.h's whole reason for existing). */
#define FLARE_SENDER_STATUS_DY    95.0f
#define FLARE_SENDER_COUNTDOWN_DY 135.0f
#define FLARE_SENDER_CANCEL_DY    178.0f
#define FLARE_SENDER_CANCEL_W     140
#define FLARE_SENDER_CANCEL_H     48

#define FLARE_LOCK_CHIP_DY (-165.0f) /* clear of RADAR_LAYOUT_STATUS_BAR_DY (-195) and every mode's top content */

/* ---------------------------------------------------------------------
 * Small shared builders (deliberately NOT shared with scr_radar.c's
 * near-identical private helpers — this file's own small, self-contained
 * copies, same "duplicated rather than cross-coupling two render files
 * over a couple dozen lines" tradeoff fixture_view.c's header comment
 * already documents for this codebase).
 * ------------------------------------------------------------------- */

static lv_obj_t *flare_make_chip(lv_obj_t *parent, char const *text, uint32_t bg_hex, uint32_t fg_hex, int32_t dy)
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
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(chip, LV_ALIGN_CENTER, 0, dy);

    lv_obj_t *label = lv_label_create(chip);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(fg_hex), 0);
    lv_obj_center(label);

    return chip;
}

static void flare_anim_set_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

/* Three pulsing rings, amber (the burst mark) — visually distinct from
 * scr_radar.c's CLOSE-mode rings (live-green) so a takeover never reads
 * as "I am close to someone", only "someone is flaring at me". Headless
 * single-frame capture never runs the animation timer (same note as
 * scr_radar.c's radar_render_close), so goldens deterministically show
 * animation-start state. */
static void flare_build_burst_mark(lv_obj_t *parent, float cy)
{
    for (int i = 0; i < 3; i++) {
        lv_obj_t *ring = lv_obj_create(parent);
        lv_obj_remove_style_all(ring);
        lv_obj_set_size(ring, FLARE_TAKEOVER_RING_RADII[i] * 2, FLARE_TAKEOVER_RING_RADII[i] * 2);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 3, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
        lv_obj_set_style_border_opa(ring, FLARE_TAKEOVER_RING_OPA[i], 0);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(ring, LV_ALIGN_CENTER, 0, (int32_t)cy);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ring);
        lv_anim_set_exec_cb(&a, flare_anim_set_opa_cb);
        lv_anim_set_values(&a, FLARE_TAKEOVER_RING_OPA[i], 0);
        lv_anim_set_duration(&a, 1200);
        lv_anim_set_reverse_duration(&a, 1200);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_delay(&a, (uint32_t)(i * 150));
        lv_anim_start(&a);
    }
}

/* A visually solid, distinctly-shaped pill button (never text-only — see
 * scr_flare.h's ff_scr_flare_build_takeover doc comment for why). `filled`
 * true: solid `bg_hex` fill (GO). `filled` false: outlined pill over the
 * surface color (DISMISS/CANCEL) — still a filled, bordered shape, not
 * bare text on the background. */
static lv_obj_t *flare_make_button(lv_obj_t *parent, char const *text, uint32_t bg_hex, uint32_t fg_hex, bool filled,
                                    int32_t w, int32_t h, int32_t dy, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    if (filled) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 3, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(bg_hex), 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    }
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, (int32_t)dy);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, FF_THEME_FONT_NAME, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(fg_hex), 0);
    lv_obj_center(label);

    return btn;
}

/* ---------------------------------------------------------------------
 * Button callbacks — each forwards to exactly one core entry point, no
 * branching (see this file's header comment).
 * ------------------------------------------------------------------- */

static void flare_go_cb(lv_event_t *e)
{
    ff_flare_t *rt = (ff_flare_t *)lv_event_get_user_data(e);
    if (rt != NULL) {
        (void)ff_flare_go(rt);
    }
}

static void flare_dismiss_takeover_cb(lv_event_t *e)
{
    ff_flare_t *rt = (ff_flare_t *)lv_event_get_user_data(e);
    if (rt != NULL) {
        (void)ff_flare_dismiss_takeover(rt);
    }
}

static void flare_cancel_send_cb(lv_event_t *e)
{
    ff_flare_t *rt = (ff_flare_t *)lv_event_get_user_data(e);
    if (rt != NULL) {
        (void)ff_flare_send_cancel(rt);
    }
}

/* ---------------------------------------------------------------------
 * Entry points.
 * ------------------------------------------------------------------- */

void ff_scr_flare_build_takeover(ff_app_flare_t const *flare, ff_flare_t *rt)
{
    if (flare == NULL || !flare->takeover_active) {
        return;
    }

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

    flare_build_burst_mark(puck, FLARE_TAKEOVER_RING_CY);

    char headline[40];
    ff_flare_fmt_headline(headline, sizeof(headline), flare->takeover_from_name);
    lv_obj_t *headline_lbl = lv_label_create(puck);
    lv_label_set_text(headline_lbl, headline);
    lv_obj_set_style_text_font(headline_lbl, FF_THEME_FONT_NAME, 0);
    lv_obj_set_style_text_color(headline_lbl, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_align(headline_lbl, LV_ALIGN_CENTER, 0, (int32_t)FLARE_TAKEOVER_HEADLINE_DY);

    /* Bearing/distance read: "NE - 320 m" (their compass bearing + honest
     * distance — dist_str is already "" when unknown, same convention as
     * ff_radar_view_t.dist_str, never fabricated here). A plain hyphen,
     * not U+00B7 MIDDLE DOT — same substitution scr_radar.c's
     * radar_render_nofix already documents (LVGL's built-in Montserrat
     * bitmap fonts don't cover that codepoint; it renders as tofu). */
    char bearing_line[40];
    char const *dist = (flare->takeover_dist_str[0] != '\0') ? flare->takeover_dist_str : "-- m";
    snprintf(bearing_line, sizeof(bearing_line), "%s - %s", ff_flare_fmt_compass8(flare->takeover_bearing_deg), dist);
    lv_obj_t *bearing_lbl = lv_label_create(puck);
    lv_label_set_text(bearing_lbl, bearing_line);
    lv_obj_set_style_text_font(bearing_lbl, FF_THEME_FONT_DISTANCE, 0);
    lv_obj_set_style_text_color(bearing_lbl, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_align(bearing_lbl, LV_ALIGN_CENTER, 0, (int32_t)FLARE_TAKEOVER_BEARING_DY);

    lv_obj_t *explain_lbl = lv_label_create(puck);
    lv_label_set_text(explain_lbl, "they lit their puck so you can spot them - arrow's locked on");
    lv_obj_set_style_text_font(explain_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(explain_lbl, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_set_width(explain_lbl, 300);
    lv_obj_set_style_text_align(explain_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(explain_lbl, LV_ALIGN_CENTER, 0, (int32_t)FLARE_TAKEOVER_EXPLAIN_DY);

    /* GO: solid amber fill — the primary, unmistakably-pressable action. */
    flare_make_button(puck, "GO", FF_THEME_COLOR_AMBER, FF_THEME_COLOR_BG, true, FLARE_TAKEOVER_BTN_W,
                       FLARE_TAKEOVER_GO_BTN_H, (int32_t)FLARE_TAKEOVER_GO_DY, flare_go_cb, rt);

    /* DISMISS: a distinct, visually solid bordered pill (surface fill +
     * amber border), NOT plain text on the background — the previous UX
     * review's exact finding on this screen's earlier pass. */
    flare_make_button(puck, "DISMISS", FF_THEME_COLOR_AMBER, FF_THEME_COLOR_INK, false, FLARE_TAKEOVER_BTN_W,
                       FLARE_TAKEOVER_DISMISS_BTN_H, (int32_t)FLARE_TAKEOVER_DISMISS_DY, flare_dismiss_takeover_cb,
                       rt);
}

void ff_scr_flare_build_sender_overlay(lv_obj_t *parent, ff_app_flare_t const *flare, ff_flare_t *rt)
{
    if (parent == NULL || flare == NULL || !flare->sending) {
        return;
    }

    /* Pulsing amber rim tint hugging the puck's own edge — same visual
     * language as scr_radar.c's STALE rim tint (amber ring around the
     * whole puck), but animated (STALE's is a static 50%-opacity ring;
     * this one pulses, per spec "own screen pulses amber") so a glance at
     * ANY face while sending reads unmistakably as "I am the one
     * flaring", not "someone/something near me is stale". */
    lv_obj_t *rim = lv_obj_create(parent);
    lv_obj_remove_style_all(rim);
    lv_obj_set_size(rim, FF_THEME_PUCK_PX - 4, FF_THEME_PUCK_PX - 4);
    lv_obj_set_style_radius(rim, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(rim, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rim, 5, 0);
    lv_obj_set_style_border_color(rim, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_border_opa(rim, LV_OPA_70, 0);
    lv_obj_clear_flag(rim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(rim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(rim);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, rim);
    lv_anim_set_exec_cb(&a, flare_anim_set_opa_cb);
    lv_anim_set_values(&a, LV_OPA_70, LV_OPA_20);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_reverse_duration(&a, 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    lv_obj_t *status_lbl = lv_label_create(parent);
    lv_label_set_text(status_lbl, "you are flaring - crew arrows locked on you");
    lv_obj_set_style_text_font(status_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(status_lbl, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_width(status_lbl, 280);
    lv_obj_set_style_text_align(status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status_lbl, LV_ALIGN_CENTER, 0, (int32_t)FLARE_SENDER_STATUS_DY);

    char countdown[16];
    ff_flare_fmt_countdown(countdown, sizeof(countdown), flare->send_expires_in_ms);
    char countdown_line[32];
    snprintf(countdown_line, sizeof(countdown_line), "ends in %s", countdown);
    flare_make_chip(parent, countdown_line, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_MUTED,
                     (int32_t)FLARE_SENDER_COUNTDOWN_DY);

    /* CANCEL button, on the puck edge below the status line — >=
     * FF_THEME_MIN_HIT_PX and visually distinct (outlined pill), matching
     * the takeover screen's DISMISS treatment. */
    flare_make_button(parent, "CANCEL", FF_THEME_COLOR_AMBER, FF_THEME_COLOR_AMBER, false, FLARE_SENDER_CANCEL_W,
                       FLARE_SENDER_CANCEL_H, (int32_t)FLARE_SENDER_CANCEL_DY, flare_cancel_send_cb, rt);
}

void ff_scr_flare_build_lock_chip(lv_obj_t *parent, ff_app_flare_t const *flare)
{
    if (parent == NULL || flare == NULL || !flare->locked) {
        return;
    }

    char text[40];
    char const *name = (flare->locked_from_name[0] != '\0') ? flare->locked_from_name : "?";
    snprintf(text, sizeof(text), "LOCKED - %s", name); /* plain hyphen, not U+00B7 — see build_takeover's note */

    flare_make_chip(parent, text, FF_THEME_COLOR_AMBER, FF_THEME_COLOR_BG, (int32_t)FLARE_LOCK_CHIP_DY);
}

bool ff_scr_flare_selection_locked(ff_flare_t const *rt)
{
    if (rt == NULL) {
        return false;
    }
    return ff_flare_locked_node(rt) != 0;
}
