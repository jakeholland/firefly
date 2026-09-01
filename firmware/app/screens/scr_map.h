/**
 * scr_map.h — app/screens: the S09 Map face (Radar's alternate view).
 *
 * Spec: docs/specs/S09-map-face.md
 *
 * Pure render (CLAUDE.md: "UI code only renders core state and forwards
 * input") of the flattened `ff_app_map_t` (`ff_app_state.h`) — the same
 * "state in, pixels out" projection every other face uses. Every screen
 * coordinate this file draws comes from `core/include/ff_map.h`'s
 * `ff_map_xform_fit`/`ff_map_project` — no geometry is recomputed or
 * duplicated here.
 */
#ifndef FF_SCR_MAP_H
#define FF_SCR_MAP_H

#include "ff_app_state.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_scr_map_build — builds the Map face into `parent` [api].
 *
 * HORIZONTAL-CAROUSEL REWORK: Map is a SWIPE TILE now, not a full-screen
 * modal, so it builds into the caller's container (`scr_nav.c` passes the
 * Map tile) rather than onto the active screen — the same tile-parented
 * convention `ff_scr_radar_build`/`ff_scr_now_build`/`ff_scr_signals_build`
 * already use. The old "tap anywhere -> back to Radar" is gone with the
 * modal: you swipe left to leave Map like any other carousel face, so this
 * face no longer emits `FF_INTENT_BACK` and installs no tap handler.
 *
 * `parent == NULL` or `map == NULL` draws nothing (same "no invented
 * fallback" contract as every other screen builder here).
 *
 * `colorblind` (S17 slice a, [api]): selects the crew palette every crew
 * dot on this face is drawn with — see scr_radar.h's identical parameter
 * for the full rationale (ff_theme.h's doc comment has the layering
 * argument for why this is explicit, not a hidden global).
 */
void ff_scr_map_build(lv_obj_t *parent, ff_app_map_t const *map, bool colorblind);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_MAP_H */
