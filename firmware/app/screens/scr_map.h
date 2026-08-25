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
 * ff_scr_map_build — builds the Map face on the CURRENT default display's
 * active screen (same calling convention as `ff_scr_settings_build`/
 * `ff_scr_compose_build`: a full-screen modal face, not a swipe tile —
 * see `ff_app_state.h`'s `FF_APP_FACE_MAP` comment for the routing
 * rationale).
 *
 * Tapping ANYWHERE on the puck emits `FF_INTENT_BACK` through the intent
 * seam (S09 spec: "Tap anywhere -> back to Radar", the same modal-dismiss
 * idiom `ff_route_pop_modal` already gives Compose/Settings's own back
 * buttons — here the whole glass is the button). Unbound (golden/headless
 * rendering, which never fires a click), the emit is a safe no-op — same
 * contract every intent emit site in this codebase follows.
 *
 * `map == NULL` draws nothing (same "no invented fallback" contract as
 * every other screen builder here).
 *
 * `colorblind` (S17 slice a, [api]): selects the crew palette every crew
 * dot on this face is drawn with — see scr_radar.h's identical parameter
 * for the full rationale (ff_theme.h's doc comment has the layering
 * argument for why this is explicit, not a hidden global).
 */
void ff_scr_map_build(ff_app_map_t const *map, bool colorblind);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_MAP_H */
