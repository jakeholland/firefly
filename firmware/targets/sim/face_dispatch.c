/**
 * face_dispatch.c — see face_dispatch.h.
 */
#include "face_dispatch.h"

#include "fixture_view.h"
#include "scr_compose.h"
#include "scr_flare.h" /* S10 slice b — full-screen receive takeover */
#include "scr_launcher.h" /* S26 slice e — the BOOT-button launcher */
#include "scr_nav.h"   /* the five base faces — Radar/Now/Signals/Map/Settings render through here */
#include "scr_power_menu.h" /* S26 slice b — the PWR-button power menu modal */
#include "scr_settings.h" /* the Settings scroll reset/hint hooks (the face itself is built by scr_nav) */

/* The face actually built for `state` — a receive-takeover overrides the
 * base face. Kept in sync with the dispatch below so the fresh-entry scroll
 * reset (#bug4) keys off what really rendered, not the base active_face a
 * takeover masked. */
static ff_app_face_t face_dispatch_effective(ff_app_state_t const *state)
{
    if (state->flare.takeover_active) {
        return FF_APP_FACE_FLARE;
    }
    return state->active_face;
}

void ff_build_face_screen(ff_app_state_t const *state)
{
    if (state == NULL) {
        return;
    }

    /* #bug4 — reset the Settings scroll ONLY on a fresh entry (a
     * not-Settings -> Settings face transition), so navigating in from
     * another face lands at the top while an in-place rebuild after a toggle
     * preserves the offset. `s_prev_face` remembers the last face this
     * dispatcher built. */
    static ff_app_face_t s_prev_face = FF_APP_FACE_NONE;
    ff_app_face_t const eff = face_dispatch_effective(state);
    if (eff == FF_APP_FACE_SETTINGS && s_prev_face != FF_APP_FACE_SETTINGS) {
        ff_scr_settings_reset_scroll();
    }
    s_prev_face = eff;

    if (state->flare.takeover_active) {
        ff_scr_flare_build_takeover(&state->flare);
        return;
    }

    /* The five base faces — Radar, Now, Signals, Map, Settings — render
     * through scr_nav.c, reached only via the launcher/BOOT since S26
     * slice e retired the swipe carousel. Compose/Power menu/Launcher
     * are the full-screen modals. */
    if (state->active_face == FF_APP_FACE_RADAR || state->active_face == FF_APP_FACE_NOW ||
        state->active_face == FF_APP_FACE_SIGNALS || state->active_face == FF_APP_FACE_MAP ||
        state->active_face == FF_APP_FACE_SETTINGS) {
        ff_scr_nav_build(state);
        /* #bug5a — apply the fixture's sim-only scroll hint AFTER the
         * build (and after any fresh-entry reset above), so a golden can
         * capture a scrolled Settings state. A no-op for the default 0
         * hint the live shell always carries, and for any non-Settings
         * face (no list is built, so ff_scr_settings_apply_scroll_hint
         * self-guards on its NULL list pointer). */
        if (state->active_face == FF_APP_FACE_SETTINGS) {
            ff_scr_settings_apply_scroll_hint(state->ui_settings_scroll_y);
        }
    } else if (state->active_face == FF_APP_FACE_COMPOSE) {
        ff_scr_compose_build(&state->compose);
    } else if (state->active_face == FF_APP_FACE_POWER_MENU) {
        /* S26 slice b — no state parameter: ff_scr_power_menu_build
         * renders fixed content (see scr_power_menu.h's top comment for
         * why). */
        ff_scr_power_menu_build();
    } else if (state->active_face == FF_APP_FACE_LAUNCHER) {
        /* S26 slice e — the BOOT-button launcher. Takes the full state
         * (unlike the power menu): the Signals circle's unread badge is
         * its one dynamic fact (scr_launcher.h's top comment). */
        ff_scr_launcher_build(state);
    } else {
        ff_fixture_view_build(state);
    }
}
