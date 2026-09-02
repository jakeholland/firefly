/**
 * scr_nav.h — app/screens: the base-face shell (S06 slice b; carousel
 * retired S26 slice e).
 *
 * Renders whichever ONE of the five base faces (Radar / Now / Signals /
 * Map / Settings) `state->active_face` names into a plain container on
 * the puck. Through S26 slice d these were reached by a horizontal
 * swipe carousel (a page-dot row, an `lv_tileview`, a gesture handler);
 * S26 slice e retired all of that — the BOOT-button launcher
 * (`scr_launcher.h`) is the one way in now, and this file is a pure
 * "build the named face" renderer with no navigation machinery of its
 * own (see scr_nav.c's header comment for the tileview -> plain
 * container swap this took).
 */
#ifndef FF_SCR_NAV_H
#define FF_SCR_NAV_H

#include "ff_app_state.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_scr_nav_build — builds the shell on the current default display's
 * active screen (same calling convention as fixture_view.h's
 * ff_fixture_view_build: a display must already be the LVGL default).
 *
 * Every one of the five base faces renders real content
 * (`ff_scr_radar_build`/`ff_scr_now_build`/`ff_scr_signals_build`/
 * `ff_scr_map_build`/`ff_scr_settings_build`, each driven by its own
 * `ff_app_state_t` section). A face outside that set (COMPOSE/FLARE/
 * POWER_MENU/LAUNCHER/NONE — none of which the dispatcher routes here)
 * falls back to Radar rather than an undefined build.
 *
 * S10 slice b additions, both driven by `state->flare` (ff_app_state.h)
 * and both cross-face per spec (not gated on `state->active_face`):
 *  - the Radar content gets a "LOCKED" chip when `state->flare.locked`
 *    (scr_flare.h's ff_scr_flare_build_lock_chip);
 *  - the puck itself gets the pulsing-amber sender overlay when
 *    `state->flare.sending` (scr_flare.h's
 *    ff_scr_flare_build_sender_overlay), and the base face's content is
 *    dimmed to LV_OPA_30 first (PR #20 UX review finding #4: the
 *    overlay must OWN the headline slot while sending, structurally,
 *    for whichever face happens to be showing — not just a
 *    NOSEL-specific fix).
 * The full-screen RECEIVE takeover (`state->flare.takeover_active`) is
 * NOT built here — per spec it "interrupts any face", so the caller
 * (targets/sim/main.c) builds `ff_scr_flare_build_takeover` INSTEAD of
 * calling this function at all when a takeover is pending.
 *
 * S16 slice c2's `[api]` change dropped the `ff_flare_t *flare_rt`
 * parameter this function used to take and forward to
 * `ff_scr_radar_build`'s FLARE button and the sender overlay's CANCEL
 * button: both now emit semantic intents through the seam
 * (`ff_intent_emit`, app/include/ff_intent.h — `FF_INTENT_FLARE_START`
 * and `FF_INTENT_FLARE_END` respectively) instead of mutating a live
 * core struct handed down through three layers of build functions.
 * Unbound (golden/headless rendering, which never fires a click), every
 * emit is a safe no-op — same contract every intent emit site in this
 * codebase follows.
 */
void ff_scr_nav_build(ff_app_state_t const *state);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_NAV_H */
