/**
 * face_dispatch.h — targets/sim: which screen builder a fixture's
 * active_face (or a pending flare takeover) maps to.
 *
 * Extracted out of main.c (PR #25 UX review follow-up) so this one
 * dispatch table has a single source of truth shared by ffsim itself
 * (window/headless render paths) AND by
 * targets/sim/tests/test_face_hit_targets.c (the cross-face round-glass
 * hit-target sweep, which needs to build the exact same real screen a
 * fixture would render in order to walk its actual LVGL object tree —
 * duplicating this mapping in the test would risk it silently drifting
 * from what ffsim actually does).
 */
#ifndef FF_FACE_DISPATCH_H
#define FF_FACE_DISPATCH_H

#include "ff_app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_build_face_screen — builds whichever screen matches `*state` on the
 * current default display's active screen.
 *
 * S10 slice b: a pending receive takeover (`state->flare.takeover_active`)
 * is checked FIRST and, if true, is the ONLY thing built — per spec
 * ("full-screen takeover regardless of current face"), it replaces
 * whatever face would otherwise show, exactly like a real full-screen
 * interrupt would; `active_face` is not consulted at all on that path.
 *
 * Otherwise: the five base faces — RADAR, NOW, SIGNALS, MAP, and
 * SETTINGS — go through the shared nav shell (`ff_scr_nav_build`, which
 * builds the active face's content). SETTINGS keeps its fresh-entry
 * scroll reset and the sim scroll hint, applied around the nav call.
 * COMPOSE, POWER_MENU, and (S26 slice e) LAUNCHER are the three
 * full-screen modals (`ff_scr_compose_build`/`ff_scr_power_menu_build`/
 * `ff_scr_launcher_build`) — none is a base face reached through nav.
 * Every other face still has no real screen and falls through to
 * fixture_view.h's S13 placeholder.
 *
 * S16 slice c2's `[api]` change dropped the `ff_flare_t *flare_rt`
 * parameter this function used to forward to `ff_scr_nav_build`/
 * `ff_scr_flare_build_takeover`: every S10 button (GO/DISMISS/CANCEL/
 * FLARE) now emits a semantic intent through the seam
 * (`ff_intent_emit`, app/include/ff_intent.h) instead of mutating a live
 * `ff_flare_t` handed down through this dispatch. A target that wants
 * those presses to DO something binds the seam to a real `ff_shell_t`
 * (`ff_intent_emit_bind(ff_shell_intent_sink, &shell)`) — this dispatch
 * function no longer needs to know that engine exists at all.
 */
void ff_build_face_screen(ff_app_state_t const *state);

#ifdef __cplusplus
}
#endif

#endif /* FF_FACE_DISPATCH_H */
