/**
 * face_dispatch.c — see face_dispatch.h.
 */
#include "face_dispatch.h"

#include "fixture_view.h"
#include "scr_compose.h"
#include "scr_nav.h"

void ff_build_face_screen(ff_app_state_t const *state)
{
    if (state == NULL) {
        return;
    }

    if (state->active_face == FF_APP_FACE_RADAR || state->active_face == FF_APP_FACE_SIGNALS) {
        ff_scr_nav_build(state);
    } else if (state->active_face == FF_APP_FACE_COMPOSE) {
        ff_scr_compose_build(&state->compose);
    } else {
        ff_fixture_view_build(state);
    }
}
