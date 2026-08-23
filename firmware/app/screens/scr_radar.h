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
 */
void ff_scr_radar_build(lv_obj_t *parent, ff_radar_view_t const *radar);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_RADAR_H */
