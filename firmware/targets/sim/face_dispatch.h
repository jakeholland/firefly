/**
 * face_dispatch.h — targets/sim: which screen builder a fixture's
 * active_face maps to.
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
 * ff_build_face_screen — builds whichever screen matches
 * `state->active_face` on the current default display's active screen:
 * RADAR and SIGNALS both go through the shared three-tile shell
 * (`ff_scr_nav_build` — SIGNALS renders real content there as of S08c);
 * COMPOSE is its own full-screen face (`ff_scr_compose_build`, reached
 * from Signals' "+", not a swipe tile). Every other face (Now, Settings)
 * still has no real screen and falls through to fixture_view.h's S13
 * placeholder.
 */
void ff_build_face_screen(ff_app_state_t const *state);

#ifdef __cplusplus
}
#endif

#endif /* FF_FACE_DISPATCH_H */
