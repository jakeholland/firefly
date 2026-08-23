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
 */
void ff_scr_nav_build(ff_app_state_t const *state);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_NAV_H */
