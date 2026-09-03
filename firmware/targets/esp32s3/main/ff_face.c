/**
 * ff_face.c — see ff_face.h.
 *
 * debt/shared-face-dispatch: the mapping logic that used to live here
 * moved to app/ff_face_dispatch.c (`ff_face_dispatch_build`). This is
 * now a thin adapter: it owns the process-lifetime
 * `ff_face_dispatch_ctx_t` (the #bug4 fresh-entry memory — same lifetime
 * the old file `static s_prev_face` had) and supplies the device's one
 * divergent tail — ESP_LOGW on a face with no real screen, in place of
 * the sim's fixture_view.h placeholder (the shell only ever projects a
 * face with a real screen, so an unknown face here is a bug, not a
 * state to paper over).
 */
#include "ff_face.h"

#include "esp_log.h"
#include "ff_face_dispatch.h"

static const char *TAG = "ff_face";

static void device_unknown_face(void *user_data, ff_app_state_t const *state)
{
    (void)user_data;
    ESP_LOGW(TAG, "no device screen for face %d — nothing built", (int)state->active_face);
}

void ff_face_build(ff_app_state_t const *state)
{
    static ff_face_dispatch_ctx_t s_ctx = FF_FACE_DISPATCH_CTX_INIT;
    static ff_face_dispatch_hooks_t const s_hooks = {
        .settings_scroll_hint = NULL, /* device has no fixture scroll hint */
        .unknown_face = device_unknown_face,
        .user_data = NULL,
    };

    ff_face_dispatch_build(state, &s_ctx, &s_hooks);
}
