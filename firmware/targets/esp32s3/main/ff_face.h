/**
 * ff_face.h — device face dispatch (S15 slice b).
 *
 * The device analogue of targets/sim/face_dispatch.h's ff_build_face_screen:
 * given an ff_app_state_t projection from ff_shell_view(), build the
 * matching real screen on the active LVGL screen. Deliberately a SEPARATE
 * (tiny) file from the sim's — that one lives under targets/sim and pulls
 * in fixture_view.h (an S13 sim-only placeholder). This one covers exactly
 * the faces the shell actually projects and has no placeholder fallback.
 *
 * MUST be called with the LVGL lock held (ff_display_lock) — esp_lvgl_port
 * runs LVGL in its own task.
 */
#ifndef FF_FACE_H
#define FF_FACE_H

#include "ff_app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void ff_face_build(ff_app_state_t const *state);

#ifdef __cplusplus
}
#endif

#endif /* FF_FACE_H */
