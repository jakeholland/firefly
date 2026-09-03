/**
 * scr_power_menu.c — see scr_power_menu.h.
 *
 * Layout constants stay a self-contained local copy (these three buttons'
 * stacked-vertical layout and glass-fit math are specific to this screen).
 * The button helper itself is NOT a local copy any more: this file's
 * `power_menu_make_button` used to say the duplication with `scr_flare.c`'s
 * (near-identical) `flare_make_button` was deliberate ("both are file-
 * static, nothing to import"). S17 debt cleanup reverses that call — once
 * `scr_settings.c`'s `settings_make_pill` turned up wanting the same
 * "rounded pill, centered label" shape too, three independent copies
 * crossed the line from "a couple dozen lines not worth cross-coupling
 * two files over" into a real, three-way drift risk. `power_menu_make_button`
 * below is now a thin adapter over the shared `ff_scr_pill_create`
 * (scr_widgets.h), byte-identical to its pre-refactor pixels.
 */
#include "scr_power_menu.h"

#include "ff_intent.h"
#include "ff_theme.h"
#include "lvgl.h"
#include "scr_widgets.h" /* ff_scr_pill_create — the shared pill factory (S17 debt cleanup) */

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
 * Button helper — a thin adapter over the shared `ff_scr_pill_create`
 * (scr_widgets.h, S17 debt cleanup), byte-identical to this file's
 * pre-refactor pixels. Same filled/outlined round-glass pill shape (and
 * LV_STATE_PRESSED touch-down feedback) as scr_flare.c's
 * flare_make_button, with ONE real difference from it: an outlined pill
 * here tints toward `bg_hex` (its own border colour) on press, not a
 * fixed amber — Reboot tints amber (its border IS amber) but Cancel
 * tints MUTED, matching its "safe default, not alarming" role.
 * `filled` true: solid `bg_hex` fill (Power off — the one destructive
 * action, the primary/unmistakably-pressable shape). `filled` false: an
 * outlined pill over the surface color (Reboot, Cancel).
 * ------------------------------------------------------------------- */
static lv_obj_t *power_menu_make_button(lv_obj_t *parent, char const *text, uint32_t bg_hex, uint32_t fg_hex,
                                         bool filled, int32_t dy, lv_event_cb_t cb)
{
    ff_scr_pill_cfg_t cfg = {
        .w = POWER_MENU_BTN_W,
        .h = POWER_MENU_BTN_H,
        .use_pos = false,
        .dy = dy,
        .radius = LV_RADIUS_CIRCLE,
        .filled = filled,
        .border_width = 3,
        .bg_hex = bg_hex,
        .fg_hex = fg_hex,
        .press = filled ? FF_SCR_PILL_PRESS_DIM : FF_SCR_PILL_PRESS_TINT,
        .press_tint_hex = bg_hex, /* outlined pill tints toward its OWN border colour, not a fixed amber */
        .font = FF_THEME_FONT_NAME,
        .letter_space = 0,
        .cb = cb,
        .user_data = NULL,
    };
    lv_obj_t *btn = ff_scr_pill_create(parent, text, &cfg);

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
