/**
 * scr_radar.h — app/screens: the Radar face (S06 slices c/d).
 *
 * Pure rendering: reads an `ff_radar_view_t` and draws it. No domain
 * logic lives here (CLAUDE.md: "UI code only renders core state and
 * forwards input" — every `if` here is about *how to draw* a value core
 * already computed, never about *what the value should be*).
 */
#ifndef FF_SCR_RADAR_H
#define FF_SCR_RADAR_H

#include <stdbool.h>

#include "ff_radar.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_scr_radar_build — renders `*radar` into `parent` (expected to be
 * `FF_THEME_PUCK_PX` square — the shell, scr_nav.c, hands it a tileview
 * tile sized to the puck). Builds fresh children every call; does not
 * clear `parent` itself (caller's responsibility if re-rendering onto a
 * reused object — the sim harness only ever builds once per process, so
 * this has never needed to matter yet).
 *
 * Renders every `radar_mode_t` per docs/specs/S06-radar-face.md's
 * rendering section, plus the never-fixed special case
 * (`ff_radar.h`'s "RENDERER CONTRACT": `mode == RADAR_LOST &&
 * age_str[0] == '\0'` gets "NO FIX YET", never "LAST SEEN"). Also draws
 * the cross-mode chrome: status bar (clock/mesh/battery) and crew ring
 * dots (dashed when a dot's own freshness isn't LIVE).
 *
 * The CLOSE-mode FLARE button emits `FF_INTENT_FLARE_START` through the
 * intent seam (`ff_intent_emit`, app/include/ff_intent.h) — S16 slice c2's
 * `[api]` change dropped the `ff_flare_t *flare_rt` parameter this
 * function used to take (S10 slice b) precisely so this file could stop
 * touching core's flare engine directly. The shell decides what the press
 * means (`ff_shell_intent`'s `FF_INTENT_FLARE_START` case); this file
 * only reports the gesture. Unbound (golden/headless rendering, which
 * never fires a click), the emit is a safe no-op — same contract every
 * intent emit site in this codebase follows (see ff_intent.h's top
 * comment).
 *
 * `colorblind` (S17 slice a, [api]): selects the crew palette every dot/
 * wedge on this face is drawn with — `ff_settings_t.colorblind` projected
 * through as `ff_app_settings_t.colorblind` by the caller (scr_nav.c has
 * the full `ff_app_state_t` in scope; see ff_theme.h's own doc comment
 * for why this is an explicit parameter rather than a hidden global).
 */
void ff_scr_radar_build(lv_obj_t *parent, ff_radar_view_t const *radar, bool colorblind);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_RADAR_H */
