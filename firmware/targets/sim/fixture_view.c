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
/* S15c: the sim window/panel is 412x412 (ff_theme.h's FF_THEME_WINDOW_PX);
 * this debug-face puck must fit inside it, not the old 456. */
#define FFV_WINDOW_W 412
#define FFV_WINDOW_H 412
#define FFV_COLOR_BG_DARK 0x0b0b10
#define FFV_COLOR_AMBER   0xffc66b
#define FFV_COLOR_DIM     0xaaaaaa

static char const *ffv_radar_mode_str(radar_mode_t m)
{
    switch (m) {
    case RADAR_LIVE: return "LIVE";
    case RADAR_STALE: return "STALE";
    case RADAR_LOST: return "LOST";
    case RADAR_PLACE: return "PLACE"; /* issue #33 */
    case RADAR_CLOSE: return "CLOSE";
    case RADAR_NOFIX: return "NOFIX";
    case RADAR_NOSEL: return "NOSEL";
    }
    return "?";
}

/* [api] S10 slice b: ff_app_flare_t is no longer a single `state` enum
 * (see ff_app_state.h's updated doc comment) — it's three independent
 * groups that can each be true or false at once. This debug placeholder
 * just prints all three rather than picking one, so it stays honest about
 * that independence instead of collapsing it back into a fake single
 * "state" the way the old ffv_flare_state_str() did. */
static void ffv_flare_line(char *buf, size_t n, ff_app_flare_t const *fl)
{
    snprintf(buf, n, "FLARE: send=%s takeover=%s(%s) lock=%s(%s)", fl->sending ? "Y" : "N",
             fl->takeover_active ? "Y" : "N", fl->takeover_active ? fl->takeover_from_name : "-",
             fl->locked ? "Y" : "N", fl->locked ? fl->locked_from_name : "-");
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

static char const *ffv_compose_mode_str(ff_app_compose_mode_t m)
{
    switch (m) {
    case FF_APP_COMPOSE_ABC: return "ABC";
    case FF_APP_COMPOSE_123: return "123";
    case FF_APP_COMPOSE_SYM: return "SYM";
    case FF_APP_COMPOSE_PRED: return "PRED"; /* S08 addendum — exhaustiveness for -Wswitch */
    }
    return "?";
}

static void ffv_build_compose_body(char *buf, size_t n, ff_app_compose_t const *cp)
{
    snprintf(buf, n,
             "FACE: COMPOSE\n"
             "TO: %s\n"
             "MODE: %s\n"
             "TEXT: %s%s",
             cp->to_name[0] != '\0' ? cp->to_name : "(broadcast)", ffv_compose_mode_str(cp->mode), cp->text,
             cp->has_pending ? " |" : "");
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
    case FF_APP_FACE_COMPOSE: ffv_build_compose_body(face_text, sizeof(face_text), &state->compose); break;
    default: snprintf(face_text, sizeof(face_text), "FACE: ?"); break;
    }

    /* Flare is a cross-face overlay in the real app (S10: "full-screen
     * takeover regardless of current face"), so it's always appended
     * rather than gated behind active_face. `buf` is sized to the sum of
     * both source buffers' FULL declared capacity (448 + 128) plus the
     * separator/terminator, not a "should be enough in practice" guess —
     * GCC's `-Wformat-truncation` (gcc-only; AppleClang doesn't emit
     * this, which is why this was missed building locally on macOS, same
     * "caught in CI, not locally" class of gap this repo's other
     * cross-compiler notes call out) reasons conservatively from each
     * source buffer's declared size, not its actual (much shorter)
     * runtime contents, and fails the build under -Werror otherwise —
     * verified against a real Linux/gcc CI failure, not just satisfying
     * the warning speculatively. */
    char flare_line[128];
    ffv_flare_line(flare_line, sizeof(flare_line), &state->flare);
    char buf[sizeof(face_text) + sizeof(flare_line) + 2]; /* +1 '\n', +1 NUL */
    snprintf(buf, sizeof(buf), "%s\n%s", face_text, flare_line);

    lv_obj_t *body = lv_label_create(puck);
    lv_label_set_text(body, buf);
    lv_obj_set_style_text_color(body, lv_color_hex(FFV_COLOR_DIM), 0);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 10);
}
