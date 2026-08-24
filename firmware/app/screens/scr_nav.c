/**
 * scr_nav.c — see scr_nav.h. Shell chrome only: no domain logic (which
 * face is "correct" to show is decided entirely by the caller-supplied
 * ff_app_state_t; this file only lays out LVGL objects for whichever
 * state it's handed).
 */
#include "scr_nav.h"

#include "ff_intent.h" /* S16c1 — the emit seam; see nav_long_press_cb */
#include "ff_theme.h"
#include "scr_flare.h" /* S10 slice b — lock chip + sender overlay */
#include "scr_now.h" /* S07b — ff_scr_now_build, the Now face */
#include "scr_radar.h"
#include "scr_signals.h" /* S08c */

/* Long-press-anywhere -> Settings: emits FF_INTENT_OPEN_SETTINGS through
 * the intent seam (S16 slice c1 — this replaces the stub S06 reserved).
 * Whether anything HAPPENS is the shell's decision, not this file's:
 * today `ff_shell_intent` rejects it, deliberately, because the S11b
 * Settings renderer does not exist yet (see ff_shell.c's judgment-call
 * comment) — this screen just reports the gesture and stays a pure
 * renderer. Unbound (goldens/headless), the emit is a no-op. */
static void nav_long_press_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_OPEN_SETTINGS, .u = {0}};
    ff_intent_emit(&in);
}

/* Page-dot row: chrome that sits ON the puck (not inside any one tile),
 * so it survives a swipe instead of scrolling away with the content —
 * created after (so drawn on top of) the tileview.
 *
 * `signals_unread_count`: S08 spec ("unread count drives a badge on the
 * page dot") — the Signals tile is always dot index 2 (fixed tile order
 * Radar/Now/Signals, see ff_scr_nav_build below), so this only ever
 * decorates that one dot, regardless of which tile is currently active. */
static void nav_build_page_dots(lv_obj_t *puck, uint32_t active_idx, uint16_t signals_unread_count)
{
    enum { N_DOTS = 3, SIGNALS_DOT_IDX = 2 };
    const int32_t dot_px = 10;
    const int32_t gap_px = 20;
    const int32_t total_w = N_DOTS * dot_px + (N_DOTS - 1) * gap_px;
    const int32_t start_x = -(total_w / 2) + dot_px / 2;

    for (uint32_t i = 0; i < N_DOTS; i++) {
        int32_t dx = start_x + (int32_t)i * (dot_px + gap_px);

        lv_obj_t *dot = lv_obj_create(puck);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, dot_px, dot_px);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(i == active_idx ? FF_THEME_COLOR_AMBER : FF_THEME_COLOR_DIM), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE); /* indicator, not a control */
        /* y=186: inset from the puck's 220px radius, comfortably inside
         * the circular silhouette at this width (matches the puck's own
         * geometry — see ff_theme.h's FF_THEME_PUCK_RADIUS_PX). */
        lv_obj_align(dot, LV_ALIGN_CENTER, dx, 186);

        if (i == SIGNALS_DOT_IDX && signals_unread_count > 0) {
            /* Small badge dot above-right of the Signals page dot —
             * visible even when that tile isn't the active one (that's
             * the whole point: it tells you there's something to look at
             * on a tile you're NOT currently looking at). Always amber
             * regardless of active/inactive dot color underneath, so it
             * reads consistently. */
            lv_obj_t *badge = lv_obj_create(puck);
            lv_obj_remove_style_all(badge);
            lv_obj_set_size(badge, 8, 8);
            lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(badge, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
            lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
            lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_align(badge, LV_ALIGN_CENTER, dx + 8, 186 - 8);
        }
    }
}

void ff_scr_nav_build(ff_app_state_t const *state, ff_flare_t *flare_rt)
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

    /* Long-press-anywhere -> Settings: emits an intent (S16c1); the shell
     * decides (rejected until the S11b renderer exists). */
    lv_obj_add_flag(puck, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(puck, nav_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t *tileview = lv_tileview_create(puck);
    lv_obj_remove_style_all(tileview);
    lv_obj_set_size(tileview, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_set_pos(tileview, 0, 0);
    lv_obj_set_style_bg_opa(tileview, LV_OPA_TRANSP, 0);

    lv_obj_t *tile_radar = lv_tileview_add_tile(tileview, 0, 0, LV_DIR_HOR);
    lv_obj_set_style_pad_all(tile_radar, 0, 0);
    lv_obj_set_style_bg_opa(tile_radar, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(tile_radar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tile_now = lv_tileview_add_tile(tileview, 1, 0, LV_DIR_HOR);
    lv_obj_set_style_pad_all(tile_now, 0, 0);
    lv_obj_set_style_bg_opa(tile_now, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(tile_now, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tile_signals = lv_tileview_add_tile(tileview, 2, 0, LV_DIR_HOR);
    lv_obj_set_style_pad_all(tile_signals, 0, 0);
    lv_obj_set_style_bg_opa(tile_signals, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(tile_signals, LV_OBJ_FLAG_SCROLLABLE);

    ff_scr_radar_build(tile_radar, &state->radar, flare_rt);
    ff_scr_now_build(tile_now, &state->now); /* S07b — real content, replaces the placeholder pane */
    ff_scr_signals_build(tile_signals, &state->signals); /* S08c */

    /* S10 slice b: the Radar face's lock chip. Added as a child of
     * tile_radar specifically (not the puck) — spec: "the Radar face
     * shows a lock indicator" — so it only ever appears alongside the
     * Radar tile's own content, not on Now/Signals. */
    ff_scr_flare_build_lock_chip(tile_radar, &state->flare);

    uint32_t tile_idx;
    switch (state->active_face) {
    case FF_APP_FACE_RADAR:
        tile_idx = 0;
        break;
    case FF_APP_FACE_NOW:
        tile_idx = 1;
        break;
    case FF_APP_FACE_SIGNALS:
        tile_idx = 2;
        break;
    case FF_APP_FACE_SETTINGS:
    default:
        /* Settings has no tile of its own (reached by long-press, not
         * swipe) — default to Radar rather than an undefined tile. */
        tile_idx = 0;
        break;
    }
    lv_tileview_set_tile_by_index(tileview, tile_idx, 0, LV_ANIM_OFF);

    /* PR #20 UX review (finding #4, BLOCKING — "flaring_self reads as an
     * error"): whatever face is showing underneath, its own headline-
     * shaped content (NOSEL's "NO CREW SELECTED", NOFIX's "NO FIX -
     * RADIO ONLY", a LIVE/STALE name label, ...) was drawn at full
     * opacity and visually outshouted the sender overlay's actual news
     * ("you are flaring"). Fixed STRUCTURALLY, at the tileview container
     * (one place), not per-renderer: dim the WHOLE base face when
     * sending, then draw the overlay at full opacity on top. This is
     * deliberately face-agnostic — it needs no "am I flaring" branch
     * threaded through scr_radar.c's per-mode renderers (or any future
     * Now/Signals screen's own headline), and unlike a per-label opacity
     * flag it can't miss a mode this reviewer didn't happen to check.
     * `lv_obj_set_style_opa` on a container blends its whole subtree as
     * one layer, so every child (status bar, arrow, dots, chips, ...)
     * dims together, not just top-level labels. */
    if (state->flare.sending) {
        lv_obj_set_style_opa(tileview, LV_OPA_30, 0);
    }

    nav_build_page_dots(puck, tile_idx, state->signals.unread_count);

    /* S10 slice b: the sender overlay, built LAST (on the puck itself,
     * not any one tile) so it paints on top of whichever tile/page-dots
     * are showing — spec: "own screen pulses amber" applies regardless
     * of which face is active. No-op internally when !state->flare.sending. */
    ff_scr_flare_build_sender_overlay(puck, &state->flare, flare_rt);
}
