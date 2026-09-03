/**
 * face_dispatch.c — see face_dispatch.h.
 *
 * debt/shared-face-dispatch: the mapping logic that used to live here
 * moved to app/ff_face_dispatch.c (`ff_face_dispatch_build`). This is
 * now a thin adapter: it owns the process-lifetime
 * `ff_face_dispatch_ctx_t` (the #bug4 fresh-entry memory — same lifetime
 * the old file `static s_prev_face` had) and supplies the sim's two
 * divergent tails as hooks — the fixture-only Settings scroll hint
 * (`ff_scr_settings_apply_scroll_hint`) and the S13 placeholder fallback
 * (`ff_fixture_view_build`) for any face with no real screen.
 */
#include "face_dispatch.h"

#include "ff_face_dispatch.h"
#include "fixture_view.h"
#include "scr_settings.h" /* ff_scr_settings_apply_scroll_hint — the sim-only scroll hint hook */

static void sim_settings_scroll_hint(void *user_data, int32_t y)
{
    (void)user_data;
    ff_scr_settings_apply_scroll_hint(y);
}

static void sim_unknown_face(void *user_data, ff_app_state_t const *state)
{
    (void)user_data;
    ff_fixture_view_build(state);
}

void ff_build_face_screen(ff_app_state_t const *state)
{
    static ff_face_dispatch_ctx_t s_ctx = FF_FACE_DISPATCH_CTX_INIT;
    static ff_face_dispatch_hooks_t const s_hooks = {
        .settings_scroll_hint = sim_settings_scroll_hint,
        .unknown_face = sim_unknown_face,
        .user_data = NULL,
    };

    ff_face_dispatch_build(state, &s_ctx, &s_hooks);
}
