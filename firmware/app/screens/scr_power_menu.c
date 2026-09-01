/**
 * scr_power_menu.c — see scr_power_menu.h.
 *
 * Layout constants + the button helper are deliberately a SELF-CONTAINED
 * local copy, not shared with `scr_flare.c`'s (near-identical)
 * `flare_make_button` — both are file-static, so there is nothing to
 * import, and this is the same "duplicated rather than cross-coupling
 * two render files over a couple dozen lines" tradeoff this codebase
 * already documents elsewhere (`fixture_view.c`'s header comment;
 * `scr_flare.c`'s own top comment makes the identical call against
 * `scr_radar.c`).
 */
#include "scr_power_menu.h"

#include "ff_intent.h"
#include "ff_theme.h"
#include "lvgl.h"

/* ---------------------------------------------------------------------
 * Layout constants.
 *
 * Three buttons, stacked and centered — verified to fit the round glass
 * (FF_THEME_PUCK_RADIUS_PX == 206) at their worst-case (farthest-from-
 * center) corner: at BTN3_DY=120, half-height 28, half-width 95, the
 * corner sits sqrt(95^2 + 148^2) ~= 176px from center, comfortably
 * inside the 206px radius with ~30px of margin — no dynamic chord
 * clamping needed the way `scr_flare.c`'s variable-length name chips
 * need (every label here is a short, fixed caption, never user text).
 * ------------------------------------------------------------------- */

#define POWER_MENU_HEADLINE_DY (-120.0f)

#define POWER_MENU_BTN_W 190
#define POWER_MENU_BTN_H 56 /* spec: ">= 56 px targets" */
#define POWER_MENU_OFF_DY      (-40.0f)
#define POWER_MENU_REBOOT_DY   (40.0f)
#define POWER_MENU_CANCEL_DY   (120.0f)

_Static_assert(POWER_MENU_BTN_H >= FF_THEME_MIN_HIT_PX, "power menu buttons must clear the 44px hit-target floor");
/* Edge-to-edge gap between adjacent buttons (24px here) must clear the
 * shared adjacency floor test_face_hit_targets.c sweeps for. */
_Static_assert((int32_t)(POWER_MENU_REBOOT_DY - POWER_MENU_OFF_DY) - POWER_MENU_BTN_H >= FF_HIT_MIN_GAP_PX,
               "power menu Power-off/Reboot gap must clear the adjacency floor");
_Static_assert((int32_t)(POWER_MENU_CANCEL_DY - POWER_MENU_REBOOT_DY) - POWER_MENU_BTN_H >= FF_HIT_MIN_GAP_PX,
               "power menu Reboot/Cancel gap must clear the adjacency floor");

/* ---------------------------------------------------------------------
 * Button helper — same filled/outlined round-glass pill shape (and
 * LV_STATE_PRESSED touch-down feedback) as scr_flare.c's flare_make_button.
 * `filled` true: solid `bg_hex` fill (Power off — the one destructive
 * action, the primary/unmistakably-pressable shape). `filled` false: an
 * outlined pill over the surface color (Reboot, Cancel).
 * ------------------------------------------------------------------- */
static lv_obj_t *power_menu_make_button(lv_obj_t *parent, char const *text, uint32_t bg_hex, uint32_t fg_hex,
                                         bool filled, int32_t dy, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, POWER_MENU_BTN_W, POWER_MENU_BTN_H);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    if (filled) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        /* Press feedback: dims on touch-down, same convention as
         * scr_flare.c's GO button (a solid-amber fill flashing amber on
         * amber would be invisible). */
        lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_INK), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 3, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(bg_hex), 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
        /* Press feedback: tints toward `bg_hex` (the border color) on
         * touch-down, same convention as scr_flare.c's DISMISS button. */
        lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_PRESSED);
    }
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, (int32_t)dy);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, FF_THEME_FONT_NAME, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(fg_hex), 0);
    lv_obj_center(label);

    return btn;
}

/* ---------------------------------------------------------------------
 * Button callbacks — each emits exactly one semantic intent, no
 * branching (see this file's header comment; scr_flare.c's flare_go_cb
 * et al. are the precedent this mirrors).
 * ------------------------------------------------------------------- */

static void power_off_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_POWER_OFF, .u = {0}};
    ff_intent_emit(&in);
}

static void power_reboot_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_POWER_REBOOT, .u = {0}};
    ff_intent_emit(&in);
}

static void power_cancel_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_POWER_CANCEL, .u = {0}};
    ff_intent_emit(&in);
}

/* ---------------------------------------------------------------------
 * Entry point.
 * ------------------------------------------------------------------- */

void ff_scr_power_menu_build(void)
{
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

    lv_obj_t *headline = lv_label_create(puck);
    lv_label_set_text(headline, "POWER");
    lv_obj_set_style_text_font(headline, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(headline, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_align(headline, LV_ALIGN_CENTER, 0, (int32_t)POWER_MENU_HEADLINE_DY);

    /* Power off: solid amber fill — the destructive action, still the
     * unmistakably-pressable shape (this codebase never ships a
     * text-only button — scr_flare.h's doc comment on why). */
    power_menu_make_button(puck, "POWER OFF", FF_THEME_COLOR_AMBER, FF_THEME_COLOR_BG, true,
                            (int32_t)POWER_MENU_OFF_DY, power_off_cb);

    /* Reboot: outlined amber pill. */
    power_menu_make_button(puck, "REBOOT", FF_THEME_COLOR_AMBER, FF_THEME_COLOR_INK, false,
                            (int32_t)POWER_MENU_REBOOT_DY, power_reboot_cb);

    /* Cancel: outlined MUTED pill — visually the least alarming of the
     * three, matching its role as the safe default. */
    power_menu_make_button(puck, "CANCEL", FF_THEME_COLOR_MUTED, FF_THEME_COLOR_INK, false,
                            (int32_t)POWER_MENU_CANCEL_DY, power_cancel_cb);
}
