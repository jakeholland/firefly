/**
 * scr_nav.c — see scr_nav.h. Shell chrome only: no domain logic (which
 * face is "correct" to show is decided entirely by the caller-supplied
 * ff_app_state_t; this file only lays out LVGL objects for whichever
 * state it's handed).
 *
 * S26 slice e (docs/specs/S26-device-lifecycle.md "(e) Home button +
 * launcher") RETIRES the horizontal swipe carousel as a navigation
 * mechanism: BOOT (via `scr_launcher.c`) is the one global nav gesture
 * now, so this file no longer emits `FF_INTENT_SWIPE` (no gesture
 * handler at all — see the header comment on why) and no longer emits
 * `FF_INTENT_OPEN_SETTINGS` (the long-press-anywhere-to-Settings hook is
 * gone; Settings is a launcher circle). The page-dot row is gone too —
 * the Signals unread badge it used to carry moved to the launcher's
 * Signals circle (`scr_launcher.c`).
 *
 * ## The tileview -> plain container swap, and why
 * The old `lv_tileview` gave this file two things: a native
 * drag-to-scroll it immediately disabled (`LV_DIR_NONE` on every tile,
 * S16 slice c3) and `lv_tileview_set_tile_by_index`'s scroll-to-tile as
 * the "which face is showing" swap mechanism. With the swipe GESTURE
 * handler gone too, nothing here still uses anything a tileview offers
 * over a bare container — issue #29 already established that only the
 * ACTIVE face's content is ever built (never all five), so "which tile"
 * was always just "which face", one value, decided once per build. A
 * plain non-scrollable `lv_obj_create` container is the SIMPLER of the
 * two ways to keep serving as that one swap surface (the other being
 * "keep the tileview, stop touching its gesture/tile-index machinery")
 * — chosen because it deletes real code (the five-tile array, the
 * per-tile flag setup, `lv_tileview_add_tile`/`_set_tile_by_index`)
 * rather than leaving inert tileview machinery behind for a future
 * reader to wonder whether it still matters.
 */
#include "scr_nav.h"

#include "ff_theme.h"
#include "scr_banner.h" /* S26 slice d — the ff_notify message banner overlay */
#include "scr_flare.h" /* S10 slice b — lock chip + sender overlay */
#include "scr_map.h" /* Map — an ordinary base face */
#include "scr_now.h" /* S07b — ff_scr_now_build, the Now face */
#include "scr_radar.h"
#include "scr_settings.h" /* Settings — the launcher's fourth circle */
#include "scr_signals.h" /* S08c */

void ff_scr_nav_build(ff_app_state_t const *state)
{
    if (state == NULL) {
        return;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *puck = lv_obj_create(scr);
    lv_obj_remove_style_all(puck);
    lv_obj_set_size(puck, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_align(puck, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(puck, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puck, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(puck, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(puck, 0, 0);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_SCROLLABLE);

    /* The one swap surface: a plain, non-scrollable, non-clickable
     * container — see this file's header comment for why this replaced
     * the tileview. Pure chrome, never a control a thumb aims at. */
    lv_obj_t *content = lv_obj_create(puck);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_set_pos(content, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_CLICKABLE);

    /* Which base face to render — the same five-way switch issue #29
     * established (build the ACTIVE face's content only), now writing
     * straight into `content` instead of picking a tile index first.
     * `default` (COMPOSE/FLARE/POWER_MENU/LAUNCHER/NONE — none of which
     * the dispatcher routes here) falls back to Radar rather than an
     * undefined build. */
    switch (state->active_face) {
    case FF_APP_FACE_RADAR:
        ff_scr_radar_build(content, &state->radar, state->settings.colorblind);
        /* S10 slice b: the Radar face's lock chip — a child of the
         * Radar content specifically, so it only ever appears alongside
         * Radar's own content. */
        ff_scr_flare_build_lock_chip(content, &state->flare);
        break;
    case FF_APP_FACE_NOW:
        ff_scr_now_build(content, &state->now); /* S07b */
        break;
    case FF_APP_FACE_SIGNALS:
        ff_scr_signals_build(content, &state->signals, state->settings.colorblind); /* S22b */
        break;
    case FF_APP_FACE_MAP:
        ff_scr_map_build(content, &state->map, state->settings.colorblind);
        break;
    case FF_APP_FACE_SETTINGS:
        /* The fresh-entry scroll reset and the sim golden scroll hint
         * stay the dispatcher's job, wrapped around this build
         * (targets/sim/face_dispatch.c) — unchanged by this file's
         * tileview -> container swap. */
        ff_scr_settings_build(content, &state->settings);
        break;
    default:
        ff_scr_radar_build(content, &state->radar, state->settings.colorblind);
        ff_scr_flare_build_lock_chip(content, &state->flare);
        break;
    }

    /* PR #20 UX review (finding #4, BLOCKING — "flaring_self reads as an
     * error"): whatever face is showing underneath, its own headline-
     * shaped content (NOSEL's "NO CREW SELECTED", NOFIX's "NO FIX -
     * RADIO ONLY", a LIVE/STALE name label, ...) was drawn at full
     * opacity and visually outshouted the sender overlay's actual news
     * ("you are flaring"). Fixed STRUCTURALLY, at this container (one
     * place), not per-renderer: dim the WHOLE base face when sending,
     * then draw the overlay at full opacity on top. `lv_obj_set_style_opa`
     * on a container blends its whole subtree as one layer, so every
     * child (status bar, arrow, chips, ...) dims together, not just
     * top-level labels. */
    if (state->flare.sending) {
        lv_obj_set_style_opa(content, LV_OPA_30, 0);
    }

    /* S10 slice b: the sender overlay, built on the puck itself (not
     * `content`) so it paints on top of whatever face is showing —
     * spec: "own screen pulses amber" applies regardless of which face
     * is active. No-op internally when !state->flare.sending. */
    ff_scr_flare_build_sender_overlay(puck, &state->flare);

    /* S26 slice d — the message banner, built LAST (on the puck, after
     * the sender overlay) so it paints on top of everything else this
     * function draws. No-op internally when !state->banner.active. Note:
     * this is unreachable while the flare TAKEOVER is up — the dispatcher
     * (face_dispatch.c / ff_face.c) returns before ever calling
     * ff_scr_nav_build in that case, so a banner never competes with the
     * takeover for the same glass. */
    ff_scr_banner_build(puck, &state->banner, state->settings.colorblind);
}
