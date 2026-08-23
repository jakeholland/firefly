/**
 * fixture_view.c — S13 slice b placeholder fixture debug view.
 *
 * See fixture_view.h for scope. Deliberately dumb: reads ff_app_state_t
 * fields and lays out text, nothing else.
 */
#include "fixture_view.h"

#include <stdio.h>

#include "lvgl.h"

/* Matches main.c's boot-placeholder palette/window size. Duplicated
 * (not shared via a header) rather than introducing a cross-file
 * coupling for two constants that both go away once S06's real theme
 * (app/theme/ff_theme.h) lands and this whole file is deleted. */
#define FFV_WINDOW_W 456
#define FFV_WINDOW_H 456
#define FFV_COLOR_BG_DARK 0x0b0b10
#define FFV_COLOR_AMBER   0xffc66b
#define FFV_COLOR_DIM     0xaaaaaa

static char const *ffv_radar_mode_str(radar_mode_t m)
{
    switch (m) {
    case RADAR_LIVE: return "LIVE";
    case RADAR_STALE: return "STALE";
    case RADAR_LOST: return "LOST";
    case RADAR_CLOSE: return "CLOSE";
    case RADAR_NOFIX: return "NOFIX";
    case RADAR_NOSEL: return "NOSEL";
    }
    return "?";
}

static char const *ffv_flare_state_str(ff_app_flare_state_t s)
{
    switch (s) {
    case FF_APP_FLARE_IDLE: return "IDLE";
    case FF_APP_FLARE_SENDING: return "SENDING";
    case FF_APP_FLARE_RECEIVED: return "RECEIVED";
    case FF_APP_FLARE_LOCKED: return "LOCKED";
    }
    return "?";
}

static void ffv_build_radar_body(char *buf, size_t n, ff_radar_view_t const *r)
{
    snprintf(buf, n,
             "FACE: RADAR\n"
             "MODE: %s\n"
             "NAME: %s\n"
             "DIST: %s\n"
             "AGE: %s\n"
             "TREND: %d\n"
             "BATT: %d%%\n"
             "MESH: %s\n"
             "CLOCK: %s\n"
             "DOTS: %u",
             ffv_radar_mode_str(r->mode), r->name, r->dist_str, r->age_str, (int)r->trend, (int)r->batt_pct,
             r->mesh_ok ? "OK" : "--", r->clock_str, (unsigned)r->n_dots);
}

/* ffv_build_now_body — REMOVED (PR #21 code review finding #5c): this
 * placeholder's FF_APP_FACE_NOW path is dead code as of S07 slice b —
 * main.c's ff_build_face_screen() routes every FF_APP_FACE_NOW fixture to
 * the real shell (scr_nav.c -> scr_now.c) before this file is ever
 * reached, the same way it already did for FF_APP_FACE_RADAR once S06
 * landed. The switch below falls through to the generic "FACE: ?" default
 * for FF_APP_FACE_NOW now, which is the honest answer for this file: it
 * no longer has (or needs) a Now-specific rendering, by design. */

static void ffv_build_signals_body(char *buf, size_t n, ff_app_signals_t const *sig)
{
    if (sig->n_items > 0) {
        ff_app_feed_item_t const *it = &sig->items[0];
        snprintf(buf, n,
                 "FACE: SIGNALS\n"
                 "ITEMS: %u UNREAD: %u\n"
                 "LATEST FROM: %s\n"
                 "TEXT: %s",
                 (unsigned)sig->n_items, (unsigned)sig->unread_count, it->from_name, it->text);
    } else {
        snprintf(buf, n, "FACE: SIGNALS\nITEMS: 0 UNREAD: %u", (unsigned)sig->unread_count);
    }
}

static void ffv_build_settings_body(char *buf, size_t n, ff_app_settings_t const *s)
{
    snprintf(buf, n,
             "FACE: SETTINGS\n"
             "UNITS: %s\n"
             "SHARE MODE: %u\n"
             "HAPTICS: %s NIGHT GLOW: %s\n"
             "WATER: %u MIN\n"
             "QUIET: %u-%u\n"
             "NAME: %s",
             s->imperial ? "FT/MI" : "M/KM", (unsigned)s->share_mode, s->haptics ? "ON" : "OFF",
             s->night_glow ? "ON" : "OFF", (unsigned)s->water_min, (unsigned)s->quiet_from_min,
             (unsigned)s->quiet_to_min, s->my_name);
}

void ff_fixture_view_build(ff_app_state_t const *state)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *puck = lv_obj_create(scr);
    lv_obj_remove_style_all(puck);
    lv_obj_set_size(puck, FFV_WINDOW_W - 16, FFV_WINDOW_H - 16);
    lv_obj_align(puck, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(puck, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puck, lv_color_hex(FFV_COLOR_BG_DARK), 0);
    lv_obj_set_style_bg_opa(puck, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(puck, 0, 0);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(puck);
    lv_label_set_text(title, state->fixture_name[0] != '\0' ? state->fixture_name : "(unnamed fixture)");
    lv_obj_set_style_text_color(title, lv_color_hex(FFV_COLOR_AMBER), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 56);

    char face_text[448];
    switch (state->active_face) {
    case FF_APP_FACE_RADAR: ffv_build_radar_body(face_text, sizeof(face_text), &state->radar); break;
    /* FF_APP_FACE_NOW: no case here — see the removed ffv_build_now_body's
     * comment just above. Falls to `default` below. */
    case FF_APP_FACE_SIGNALS: ffv_build_signals_body(face_text, sizeof(face_text), &state->signals); break;
    case FF_APP_FACE_SETTINGS: ffv_build_settings_body(face_text, sizeof(face_text), &state->settings); break;
    default: snprintf(face_text, sizeof(face_text), "FACE: ?"); break;
    }

    /* Flare is a cross-face overlay in the real app (S10: "full-screen
     * takeover regardless of current face"), so it's always appended
     * rather than gated behind active_face. Composed via one snprintf
     * (not a manual append) — simpler and no truncation-math to get
     * wrong; `face_text`'s 448-byte budget leaves comfortable headroom
     * before this 512-byte buffer for the flare line's worst case. */
    char buf[512];
    snprintf(buf, sizeof(buf), "%s\nFLARE: %s", face_text, ffv_flare_state_str(state->flare.state));

    lv_obj_t *body = lv_label_create(puck);
    lv_label_set_text(body, buf);
    lv_obj_set_style_text_color(body, lv_color_hex(FFV_COLOR_DIM), 0);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 10);
}
