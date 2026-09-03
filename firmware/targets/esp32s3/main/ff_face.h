/**
 * ff_face.h — device face dispatch (S15 slice b).
 *
 * The device analogue of targets/sim/face_dispatch.h's ff_build_face_screen:
 * given an ff_app_state_t projection from ff_shell_view(), build the
 * matching real screen on the active LVGL screen.
 *
 * debt/shared-face-dispatch: the mapping itself is now
 * app/ff_face_dispatch.h's `ff_face_dispatch_build`, the SAME chain
 * targets/sim/face_dispatch.c calls — closing the "two independently
 * hand-synced copies" gap this header used to paper over with a comment
 * ("the two must agree"). This file stays deliberately separate from
 * the sim's own face_dispatch.h/.c (that one lives under targets/sim
 * and pulls in fixture_view.h, an S13 sim-only placeholder — this one
 * has no placeholder fallback, only an ESP_LOGW): both are now thin
 * adapters over the same shared function, supplying their own hooks.
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
