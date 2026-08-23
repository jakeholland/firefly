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
#include "ff_flare.h" /* S10 slice b — ff_flare_t, the flare_rt param below */

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
 * Otherwise: RADAR, NOW, and SIGNALS all go through the shared three-tile
 * shell (`ff_scr_nav_build` — NOW renders real content as of S07b, SIGNALS
 * as of S08c); COMPOSE is its own full-screen face (`ff_scr_compose_build`,
 * reached from Signals' "+", not a swipe tile). Every other face
 * (Settings) still has no real screen and falls through to
 * fixture_view.h's S13 placeholder.
 *
 * `flare_rt` is forwarded to whichever screen builder needs it for its
 * button callbacks (NULL is a fully-defined, safe value — see
 * scr_flare.h's top comment — used by headless one-shot rendering, which
 * has no interactivity for a GO/DISMISS/CANCEL press to act on; a real
 * per-process `ff_flare_t` is threaded through in window mode).
 */
void ff_build_face_screen(ff_app_state_t const *state, ff_flare_t *flare_rt);

#ifdef __cplusplus
}
#endif

#endif /* FF_FACE_DISPATCH_H */
