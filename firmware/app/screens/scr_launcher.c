/**
 * scr_launcher.c — see scr_launcher.h.
 *
 * Layout constants + the circle-button helper are a SELF-CONTAINED local
 * copy, not shared with `scr_power_menu.c`'s (near-identical) button
 * helper — same file-static, nothing-to-import tradeoff that file's own
 * top comment documents against `scr_flare.c`.
 *
 * PR #142 review, Design 1 + Design 2: the circles now show a real LVGL
 * built-in symbol glyph (`LV_SYMBOL_*` — compiled into the Montserrat
 * bitmap fonts this codebase already ships, exactly like
 * `scr_banner.c`'s `LV_SYMBOL_ENVELOPE`/`LV_SYMBOL_GPS` and
 * `scr_signals.c`'s several `LV_SYMBOL_*` rows) rather than a text
 * abbreviation, and are 96px in diameter rather than 64px.
 */
#include "scr_launcher.h"

#include "ff_intent.h"
#include "ff_theme.h"
#include "scr_signals.h" /* ff_scr_signals_unread_count — the Signals circle's badge */
#include "lvgl.h"

/* ---------------------------------------------------------------------
 * Layout constants.
 *
 * Four circles in a diagonal cross (NW/NE/SW/SE) around the puck
 * center, at radius CIRCLE_OFFSET (85px) — verified to fit the round
 * glass (FF_THEME_PUCK_RADIUS_PX == 206): a circle's farthest corner
 * from center is sqrt(85^2+85^2) + CIRCLE_DIAM/2 ~= 168px, and a
 * caption's farthest label pixel at the bottom row's y (~145) sits
 * within that row's ~146px chord half-width — both comfortably inside
 * the 206px radius, with the corner-clip margin `radar_layout.h`'s own
 * collision-free placement discipline expects. Adjacent circles (e.g.
 * NW-NE) are 170px apart center-to-center, an edge-to-edge gap of
 * 170-96=74px, far past the FF_HIT_MIN_GAP_PX floor — verified by
 * `test_face_hit_targets.c`'s adjacency sweep, not just by hand.
 * ------------------------------------------------------------------- */

#define LAUNCHER_CIRCLE_OFFSET 85.0f /* dx=dy from center for each circle, NE/NW/SW/SE */
#define LAUNCHER_CIRCLE_DIAM   96    /* PR #142 review Design 2 — up from 64px: "easier-to-tap targets" */
#define LAUNCHER_CAPTION_DY    60.0f /* caption baseline offset from its circle's own center */
#define LAUNCHER_BADGE_PX      16    /* the unread badge scales with the circle (was 12px at 64px) */
#define LAUNCHER_GLYPH_FONT    (&lv_font_montserrat_24) /* readable at ~30px inside a 96px circle */

_Static_assert(LAUNCHER_CIRCLE_DIAM >= 56, "launcher circles must clear the spec's 56px floor");
_Static_assert(LAUNCHER_CIRCLE_DIAM >= FF_THEME_MIN_HIT_PX, "launcher circles must clear the shared 44px hit floor");

/* ---------------------------------------------------------------------
 * One app circle + its caption. `glyph`: an `LV_SYMBOL_*` string drawn
 * INSIDE the circle — the spec's "kind glyph" (PR #142 review Design 1:
 * this repo DOES vendor icon glyphs, compiled into its Montserrat
 * bitmap fonts by default; `ff_theme.h`'s font-substitution note is
 * about DEVICE FONTS/type sizes, not about symbols, and does not apply
 * here). `caption`: the full word, drawn below the circle.
 * `launcher_idx`: this file's own fixed circle order (0=Now, 1=Signals,
 * 2=Map, 3=Settings — ff_intent.h's FF_INTENT_LAUNCHER_SELECT payload),
 * passed through LVGL event user_data (the scr_compose.c/scr_signals.c
 * precedent for a per-callback small int with no new global state).
 * ------------------------------------------------------------------- */

static void launcher_circle_click_cb(lv_event_t *e)
{
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    ff_intent_t in = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {0}};
    in.u.launcher_idx = (uint8_t)idx;
    ff_intent_emit(&in);
}

static void launcher_make_circle(lv_obj_t *puck, char const *glyph, char const *caption_text, float dx, float dy,
                                  uintptr_t launcher_idx, bool badge)
{
    lv_obj_t *btn = lv_button_create(puck);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LAUNCHER_CIRCLE_DIAM, LAUNCHER_CIRCLE_DIAM);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 3, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    /* Press feedback: tints toward amber on touch-down — the same
     * outlined-pill convention scr_power_menu.c's Reboot/Cancel buttons
     * use (LV_STATE_PRESSED). */
    lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_AMBER), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_align(btn, LV_ALIGN_CENTER, (int32_t)dx, (int32_t)dy);
    lv_obj_add_event_cb(btn, launcher_circle_click_cb, LV_EVENT_CLICKED, (void *)launcher_idx);

    lv_obj_t *glyph_label = lv_label_create(btn);
    lv_label_set_text(glyph_label, glyph);
    lv_obj_set_style_text_font(glyph_label, LAUNCHER_GLYPH_FONT, 0);
    lv_obj_set_style_text_color(glyph_label, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_center(glyph_label);

    /* Caption: a sibling on `puck`, NOT a child of `btn` — LVGL base
     * labels are non-clickable by default (scr_power_menu.c's "POWER"
     * headline is the precedent: a bare lv_label_create with no explicit
     * CLICKABLE clear), so this stays pure chrome, never a second hit
     * target the adjacency sweep would have to reason about. */
    lv_obj_t *caption_label = lv_label_create(puck);
    lv_label_set_text(caption_label, caption_text);
    lv_obj_set_style_text_font(caption_label, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(caption_label, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_align(caption_label, LV_ALIGN_CENTER, (int32_t)dx, (int32_t)(dy + LAUNCHER_CAPTION_DY));

    if (badge) {
        /* Small amber dot at the circle's upper-right — same badge shape
         * the old page-dot row used (scr_nav.c's retired
         * nav_build_page_dots), moved here per this slice's spec, scaled
         * up with the circle (PR #142 review Design 2). */
        lv_obj_t *dot = lv_obj_create(puck);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, LAUNCHER_BADGE_PX, LAUNCHER_BADGE_PX);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE); /* indicator, not a control */
        int32_t const corner = (int32_t)(LAUNCHER_CIRCLE_DIAM / 2 * 0.7071f); /* ~45 degrees out on the rim */
        lv_obj_align(dot, LV_ALIGN_CENTER, (int32_t)dx + corner, (int32_t)dy - corner);
    }
}

void ff_scr_launcher_build(ff_app_state_t const *state)
{
    if (state == NULL) {
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

    bool const signals_unread = ff_scr_signals_unread_count(&state->signals) > 0;

    /* Fixed circle order — MUST match ff_intent.h's FF_INTENT_LAUNCHER_SELECT
     * payload convention (0=Now, 1=Signals, 2=Map, 3=Settings) and
     * ff_shell.c's k_launcher_faces mapping table. Reading order:
     * top-left, top-right, bottom-left, bottom-right.
     *
     * Glyphs (PR #142 review Design 1): LV_SYMBOL_LIST for Now (the
     * lineup/schedule list), LV_SYMBOL_ENVELOPE for Signals (the same
     * glyph scr_banner.c already uses for an incoming MESSAGE),
     * LV_SYMBOL_GPS for Map (the same glyph scr_banner.c uses for RALLY
     * and scr_signals.c's own rally row), LV_SYMBOL_SETTINGS for
     * Settings — the standard LVGL gear glyph. */
    launcher_make_circle(puck, LV_SYMBOL_LIST, "NOW", -LAUNCHER_CIRCLE_OFFSET, -LAUNCHER_CIRCLE_OFFSET, 0, false);
    launcher_make_circle(puck, LV_SYMBOL_ENVELOPE, "SIGNALS", LAUNCHER_CIRCLE_OFFSET, -LAUNCHER_CIRCLE_OFFSET, 1,
                          signals_unread);
    launcher_make_circle(puck, LV_SYMBOL_GPS, "MAP", -LAUNCHER_CIRCLE_OFFSET, LAUNCHER_CIRCLE_OFFSET, 2, false);
    launcher_make_circle(puck, LV_SYMBOL_SETTINGS, "SETTINGS", LAUNCHER_CIRCLE_OFFSET, LAUNCHER_CIRCLE_OFFSET, 3,
                          false);
}
