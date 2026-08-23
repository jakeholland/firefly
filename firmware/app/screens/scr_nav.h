/**
 * scr_nav.h — app/screens: the three-face swipe shell (S06 slice b).
 *
 * Radar / Now / Signals, one `lv_tileview` tile each, plus a page-dot row.
 * Settings (S11) is not a swipe tile — it's reached by a long-press
 * anywhere on the puck (see scr_nav.c's stub hook), matching the S06 PR B
 * brief ("long-press-anywhere hook reserved for Settings").
 */
#ifndef FF_SCR_NAV_H
#define FF_SCR_NAV_H

#include "ff_app_state.h"
#include "ff_flare.h" /* S10 slice b — flare_rt param, see ff_scr_nav_build's doc comment */
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_scr_nav_build — builds the shell on the current default display's
 * active screen (same calling convention as fixture_view.h's
 * ff_fixture_view_build: a display must already be the LVGL default).
 *
 * Only the Radar tile renders real content in this slice
 * (`ff_scr_radar_build`, driven by `state->radar`) — Now/Signals are
 * placeholder panes clearly marked with their owning spec number
 * (S07/S08), per the PR B brief ("Only Radar renders real content this
 * PR; Now/Signals get placeholder panes clearly marked TODO"). The
 * tileview opens on whichever tile matches `state->active_face`
 * (`FF_APP_FACE_SETTINGS` falls back to the Radar tile — Settings has no
 * tile of its own here).
 *
 * S10 slice b additions, both driven by `state->flare` (ff_app_state.h)
 * and both cross-face per spec (not gated on `state->active_face`):
 *  - the Radar tile gets a "LOCKED" chip when `state->flare.locked`
 *    (scr_flare.h's ff_scr_flare_build_lock_chip);
 *  - the puck itself (survives a face swipe) gets the pulsing-amber
 *    sender overlay when `state->flare.sending`
 *    (scr_flare.h's ff_scr_flare_build_sender_overlay), and the base
 *    face's tileview is dimmed to LV_OPA_30 first (PR #20 UX review
 *    finding #4: the overlay must OWN the headline slot while sending,
 *    structurally, for whichever face happens to be showing — not just
 *    a NOSEL-specific fix).
 * The full-screen RECEIVE takeover (`state->flare.takeover_active`) is
 * NOT built here — per spec it "interrupts any face", so the caller
 * (targets/sim/main.c) builds `ff_scr_flare_build_takeover` INSTEAD of
 * calling this function at all when a takeover is pending.
 *
 * `flare_rt` ([api] new parameter, S10 slice b): the live `ff_flare_t`
 * forwarded to `ff_scr_radar_build`'s FLARE button and the sender
 * overlay's CANCEL button — NULL is always safe (golden/headless
 * rendering never fires a click at all; see scr_flare.h's top comment).
 */
void ff_scr_nav_build(ff_app_state_t const *state, ff_flare_t *flare_rt);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_NAV_H */
