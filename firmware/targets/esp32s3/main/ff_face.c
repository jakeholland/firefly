/**
 * ff_face.c — see ff_face.h. Mirrors targets/sim/face_dispatch.c's mapping
 * (the two must agree, since both project the same ff_app_state_t through
 * the same screen builders) minus the sim-only fixture_view fallback: the
 * shell only ever projects a face with a real screen, so an unknown face
 * is a bug, logged rather than papered over with a placeholder.
 */
#include "ff_face.h"

#include "esp_log.h"
#include "scr_compose.h"
#include "scr_flare.h"
#include "scr_nav.h"      /* the 5-face carousel — Radar/Now/Signals/Map/Settings all render through here */
#include "scr_settings.h" /* the Settings scroll-reset hook (the tile itself is built by scr_nav) */

static const char *TAG = "ff_face";

void ff_face_build(ff_app_state_t const *state)
{
    if (state == NULL) {
        return;
    }

    /* #bug4 — reset the Settings scroll only on a fresh entry (a
     * not-Settings -> Settings transition), so navigating in lands at the top
     * while an in-place rebuild after a toggle preserves the offset. Mirrors
     * targets/sim/face_dispatch.c; a receive-takeover renders FLARE, so it
     * counts as a not-Settings face here too. */
    static ff_app_face_t s_prev_face = FF_APP_FACE_NONE;
    ff_app_face_t const eff = state->flare.takeover_active ? FF_APP_FACE_FLARE : state->active_face;
    if (eff == FF_APP_FACE_SETTINGS && s_prev_face != FF_APP_FACE_SETTINGS) {
        ff_scr_settings_reset_scroll();
    }
    s_prev_face = eff;

    /* Full-screen receive takeover interrupts any face (S10). */
    if (state->flare.takeover_active) {
        ff_scr_flare_build_takeover(&state->flare);
        return;
    }

    /* Horizontal-carousel rework: all five swipe faces — Radar, Now,
     * Signals, Map, Settings — render through the nav tileview now (Map
     * and Settings were their own full-screen builds here; they are
     * carousel tiles today, built by scr_nav.c). Compose stays the one
     * full-screen modal. */
    if (state->active_face == FF_APP_FACE_RADAR || state->active_face == FF_APP_FACE_NOW ||
        state->active_face == FF_APP_FACE_SIGNALS || state->active_face == FF_APP_FACE_MAP ||
        state->active_face == FF_APP_FACE_SETTINGS) {
        ff_scr_nav_build(state);
    } else if (state->active_face == FF_APP_FACE_COMPOSE) {
        ff_scr_compose_build(&state->compose);
    } else {
        ESP_LOGW(TAG, "no device screen for face %d — nothing built", (int)state->active_face);
    }
}
