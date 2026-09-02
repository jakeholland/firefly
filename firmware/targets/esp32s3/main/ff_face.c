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
#include "scr_launcher.h" /* S26 slice e — the BOOT-button launcher */
#include "scr_nav.h"      /* the five base faces — Radar/Now/Signals/Map/Settings render through here */
#include "scr_power_menu.h" /* S26 slice b — the PWR-button power menu modal */
#include "scr_settings.h" /* the Settings scroll-reset hook (the face itself is built by scr_nav) */

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

    /* The five base faces — Radar, Now, Signals, Map, Settings — render
     * through scr_nav.c, reached only via the launcher/BOOT since S26
     * slice e retired the swipe carousel. Compose and Power menu are
     * the two full-screen modals; the Launcher (amended 2026-09-01) is
     * a BASE face — home itself, not a modal — but is dispatched here
     * exactly like one, since active_face names it either way. */
    if (state->active_face == FF_APP_FACE_RADAR || state->active_face == FF_APP_FACE_LINEUP ||
        state->active_face == FF_APP_FACE_INBOX || state->active_face == FF_APP_FACE_MAP ||
        state->active_face == FF_APP_FACE_SETTINGS) {
        ff_scr_nav_build(state);
    } else if (state->active_face == FF_APP_FACE_COMPOSE) {
        ff_scr_compose_build(&state->compose);
    } else if (state->active_face == FF_APP_FACE_POWER_MENU) {
        ff_scr_power_menu_build();
    } else if (state->active_face == FF_APP_FACE_LAUNCHER) {
        ff_scr_launcher_build(state);
    } else {
        ESP_LOGW(TAG, "no device screen for face %d — nothing built", (int)state->active_face);
    }
}
