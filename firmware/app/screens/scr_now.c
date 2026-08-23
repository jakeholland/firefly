/**
 * scr_now.c — see scr_now.h. Pure render: ff_app_now_t -> LVGL objects.
 *
 * Layout constants (row/card/banner vertical positions, progress-bar
 * geometry) live in now_layout.h/.c, NOT here — same "compute vs. render"
 * split scr_radar.c documents at its own top (this file only draws the
 * coordinates now_layout.c's pure functions hand back). now_layout.h's
 * top comment explains what it reuses from radar_layout.h (the
 * cross-mode `RADAR_LAYOUT_PAGE_DOT_DY` constant, reused directly below)
 * and what it doesn't (radar_layout's search-based arrow/dot resolvers —
 * not applicable, this face has no radially-placed elements).
 */
#include "scr_now.h"

#include <stdio.h>

#include "ff_theme.h"
#include "now_layout.h"
#include "radar_layout.h" /* RADAR_LAYOUT_PAGE_DOT_DY reuse — see this file's top comment and now_layout.h's */

/* Bottom keep-out for the TBD lineup list: stay clear of the page-dot row
 * scr_nav.c draws directly on the puck (on top of every tile, this one
 * included) at RADAR_LAYOUT_PAGE_DOT_DY. That row's own reserved rect
 * (radar_layout.c's build_registry) spans PAGE_DOT_DY +/- 10px; 36px of
 * margin here is a deliberate cushion beyond the bare minimum, not a
 * tight fit. Derived from the SAME constant radar_layout.c's registry
 * uses, not a second independently-chosen number that could drift out of
 * sync with where the dots actually are. */
#define NOW_SCR_LINEUP_BOTTOM_DY (RADAR_LAYOUT_PAGE_DOT_DY - 36.0f)

/* ---------------------------------------------------------------------
 * LIVE state.
 * ------------------------------------------------------------------- */

static void now_build_row(lv_obj_t *parent, ff_app_now_row_t const *row, int32_t row_dy)
{
    uint32_t stage_color = (row->stage_color_rgb != 0) ? row->stage_color_rgb : FF_THEME_COLOR_MUTED;

    lv_obj_t *stage_lbl = lv_label_create(parent);
    lv_label_set_text(stage_lbl, (row->stage_name[0] != '\0') ? row->stage_name : "STAGE UNKNOWN");
    lv_obj_set_style_text_font(stage_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(stage_lbl, lv_color_hex(stage_color), 0);
    lv_obj_align(stage_lbl, LV_ALIGN_CENTER, 0, row_dy + (int32_t)NOW_LAYOUT_ROW_STAGE_OFFSET_DY);

    lv_obj_t *artist_lbl = lv_label_create(parent);
    lv_label_set_text(artist_lbl, (row->artist[0] != '\0') ? row->artist : "(unknown)");
    lv_obj_set_style_text_font(artist_lbl, FF_THEME_FONT_NAME, 0);
    lv_obj_set_style_text_color(artist_lbl, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_align(artist_lbl, LV_ALIGN_CENTER, 0, row_dy + (int32_t)NOW_LAYOUT_ROW_ARTIST_OFFSET_DY);

    lv_obj_t *track = lv_obj_create(parent);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, NOW_LAYOUT_ROW_BAR_TRACK_W_PX, NOW_LAYOUT_ROW_BAR_H_PX);
    lv_obj_set_style_bg_color(track, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(track, LV_ALIGN_CENTER, 0, row_dy + (int32_t)NOW_LAYOUT_ROW_BAR_OFFSET_DY);

    int32_t fill_w = now_layout_bar_fill_px(row->pct_done, NOW_LAYOUT_ROW_BAR_TRACK_W_PX);
    if (fill_w > 0) {
        lv_obj_t *fill = lv_obj_create(track);
        lv_obj_remove_style_all(fill);
        lv_obj_set_size(fill, fill_w, NOW_LAYOUT_ROW_BAR_H_PX);
        lv_obj_set_style_bg_color(fill, lv_color_hex(stage_color), 0);
        lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(fill, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(fill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);
    }
}

static void now_build_next_card(lv_obj_t *parent, ff_app_next_t const *next)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, "UP NEXT (STARRED)");
    lv_obj_set_style_text_font(label, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, (int32_t)NOW_LAYOUT_NEXT_LABEL_DY);

    lv_obj_t *artist_lbl = lv_label_create(parent);
    lv_label_set_text(artist_lbl, (next->artist[0] != '\0') ? next->artist : "(unknown)");
    lv_obj_set_style_text_font(artist_lbl, FF_THEME_FONT_NAME, 0);
    lv_obj_set_style_text_color(artist_lbl, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_align(artist_lbl, LV_ALIGN_CENTER, 0, (int32_t)NOW_LAYOUT_NEXT_ARTIST_DY);

    lv_obj_t *stage_lbl = lv_label_create(parent);
    lv_label_set_text(stage_lbl, (next->stage_name[0] != '\0') ? next->stage_name : "STAGE UNKNOWN");
    lv_obj_set_style_text_font(stage_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(stage_lbl, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_align(stage_lbl, LV_ALIGN_CENTER, 0, (int32_t)NOW_LAYOUT_NEXT_STAGE_DY);

    /* Anxiety-critical text (S07 spec / UX review brief: "the countdown
     * ... keep >= 13px equivalent"). FF_THEME_FONT_LABEL/CHIP (14px)
     * already clears that floor with margin (see ff_theme.h's own
     * rounding rationale). UX review round 1 (PR #21) had this at
     * FF_THEME_FONT_NAME (22px) — legible, but tied for biggest text on
     * the card with the artist name above it. Reviewer's note: on the
     * Radar face, the single most time-critical number (distance) is
     * STRICTLY the largest thing on that whole face (36px, bigger than
     * the 22px name) — this is the Now face's equivalent anxiety-killer
     * number and should read the same way: unambiguously the most
     * prominent element, not merely tied for it. Bumped to
     * FF_THEME_FONT_DISTANCE (36px, the same constant Radar's own
     * distance readout uses) for that reason, not just floor compliance.
     * Judgment call, no mockup access — see now_layout.h's top comment
     * for the same category of call. */
    char countdown[24];
    now_layout_format_countdown(next->mins_until, countdown, sizeof(countdown));
    lv_obj_t *cd_lbl = lv_label_create(parent);
    lv_label_set_text(cd_lbl, countdown);
    lv_obj_set_style_text_font(cd_lbl, FF_THEME_FONT_DISTANCE, 0);
    lv_obj_set_style_text_color(cd_lbl, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_align(cd_lbl, LV_ALIGN_CENTER, 0, (int32_t)NOW_LAYOUT_NEXT_COUNTDOWN_DY);
}

static void now_render_live(lv_obj_t *parent, ff_app_now_t const *now)
{
    if (now->n_rows == 0 && !now->next.valid) {
        /* Pack loaded, schedule known, but genuinely nothing live and
         * nothing starred upcoming right now — distinct from both the
         * no-pack-loaded state and the TBD state (CLAUDE.md: unknown must
         * stay explicit, not collapse into a lookalike message). */
        lv_obj_t *headline = lv_label_create(parent);
        lv_label_set_text(headline, "NOTHING LIVE RIGHT NOW");
        lv_obj_set_style_text_font(headline, FF_THEME_FONT_HEADLINE, 0);
        lv_obj_set_style_text_color(headline, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
        lv_obj_align(headline, LV_ALIGN_CENTER, 0, (int32_t)NOW_LAYOUT_QUIET_HEADLINE_DY);

        lv_obj_t *sub = lv_label_create(parent);
        lv_label_set_text(sub, "Check back closer to your next set");
        lv_obj_set_style_text_font(sub, FF_THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_color(sub, lv_color_hex(FF_THEME_COLOR_DIM), 0);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, (int32_t)NOW_LAYOUT_QUIET_SUB_DY);
        return;
    }

    if (now->n_rows > 0) {
        /* "2-second test" header — see now_layout.h's doc comment on
         * NOW_LAYOUT_LIVE_HEADER_DY. Only shown when something is
         * actually live: an empty header over zero rows would be its own
         * small honesty gap (implying live content that isn't there). */
        lv_obj_t *header = lv_label_create(parent);
        lv_label_set_text(header, "NOW PLAYING");
        lv_obj_set_style_text_font(header, FF_THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_color(header, lv_color_hex(FF_THEME_COLOR_DIM), 0);
        lv_obj_align(header, LV_ALIGN_CENTER, 0, (int32_t)NOW_LAYOUT_LIVE_HEADER_DY);
    }

    for (uint8_t i = 0; i < now->n_rows && i < FF_APP_NOW_MAX_ROWS; i++) {
        int32_t row_dy = (int32_t)(NOW_LAYOUT_ROW0_DY + (float)i * NOW_LAYOUT_ROW_SPACING_DY);
        now_build_row(parent, &now->rows[i], row_dy);
    }

    if (now->next.valid) {
        now_build_next_card(parent, &now->next);
    }
}

/* ---------------------------------------------------------------------
 * TBD state.
 * ------------------------------------------------------------------- */

static void now_build_tbd_banner(lv_obj_t *parent)
{
    lv_obj_t *banner = lv_obj_create(parent);
    lv_obj_remove_style_all(banner);
    lv_obj_set_style_bg_color(banner, lv_color_hex(FF_THEME_COLOR_STALE_AMBER), 0);
    lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(banner, 10, 0);
    lv_obj_set_style_pad_left(banner, 16, 0);
    lv_obj_set_style_pad_right(banner, 16, 0);
    lv_obj_set_style_pad_top(banner, 8, 0);
    lv_obj_set_style_pad_bottom(banner, 8, 0);
    lv_obj_set_width(banner, LV_SIZE_CONTENT);
    lv_obj_set_height(banner, LV_SIZE_CONTENT);
    lv_obj_clear_flag(banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(banner, LV_ALIGN_CENTER, 0, (int32_t)NOW_LAYOUT_TBD_BANNER_DY);

    lv_obj_t *banner_lbl = lv_label_create(banner);
    lv_label_set_text(banner_lbl, "SET TIMES TBD");
    lv_obj_set_style_text_font(banner_lbl, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(banner_lbl, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_center(banner_lbl);
}

static void now_render_tbd(lv_obj_t *parent, ff_app_now_t const *now)
{
    now_build_tbd_banner(parent);

    /* Width: the narrower of the puck's chord at the list's top and
     * bottom edges (the bottom is farther from center, hence narrower —
     * see now_layout_chord_half_width_px), minus a fixed margin so text
     * never hugs the round bezel. */
    float half_w = now_layout_chord_half_width_px(NOW_SCR_LINEUP_BOTTOM_DY);
    int32_t list_w = (int32_t)(half_w * 2.0f) - 40;
    if (list_w > 320) {
        list_w = 320;
    }
    if (list_w < 120) {
        list_w = 120;
    }
    int32_t list_h = (int32_t)(NOW_SCR_LINEUP_BOTTOM_DY - NOW_LAYOUT_LINEUP_TOP_DY);
    int32_t list_cy = (int32_t)((NOW_LAYOUT_LINEUP_TOP_DY + NOW_SCR_LINEUP_BOTTOM_DY) / 2.0f);

    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, list_w, list_h);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, list_cy);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    /* Spec: "per-day lineup list (scroll)" — real device content can
     * exceed the visible band, so this stays a real scrollable container
     * (deterministic in the headless golden regardless: a single-frame
     * capture always shows the initial, unscrolled top of the list). */
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_CLICKABLE);

    if (now->n_lineup == 0) {
        lv_obj_t *empty_lbl = lv_label_create(list);
        lv_label_set_text(empty_lbl, "No sets listed for today yet");
        lv_obj_set_style_text_font(empty_lbl, FF_THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_color(empty_lbl, lv_color_hex(FF_THEME_COLOR_DIM), 0);
        return;
    }

    for (uint8_t i = 0; i < now->n_lineup && i < FF_APP_NOW_MAX_LINEUP; i++) {
        ff_app_lineup_item_t const *item = &now->lineup[i];
        char line[FF_APP_ARTIST_LEN + FF_APP_STAGE_LEN + 4];
        char const *artist = (item->artist[0] != '\0') ? item->artist : "(unknown)";
        /* UX review finding #2 (PR #21): this used to omit the stage
         * silently when unknown, while now_build_row() (the LIVE rows,
         * just above in this same file) falls back to the literal
         * "STAGE UNKNOWN" string for the identical missing-data case.
         * Same face, same "we don't know the stage" fact, must read the
         * same way — a bare artist name next to five others that DO show
         * a stage reads as a broken template, not a stated unknown
         * (CLAUDE.md: unknown must stay explicit). Always show a stage
         * field now, honest placeholder or not.
         *
         * Plain hyphen, not an em dash: LVGL's built-in Montserrat bitmap
         * fonts only cover the ASCII printable range by default (same
         * substitution scr_radar.c's radar_render_nofix already makes
         * for U+00B7, for the identical reason). */
        char const *stage = (item->stage_name[0] != '\0') ? item->stage_name : "STAGE UNKNOWN";
        snprintf(line, sizeof(line), "%s - %s", artist, stage);

        lv_obj_t *item_lbl = lv_label_create(list);
        lv_label_set_text(item_lbl, line);
        lv_obj_set_width(item_lbl, list_w);
        lv_obj_set_style_text_font(item_lbl, FF_THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_color(item_lbl, lv_color_hex(FF_THEME_COLOR_INK), 0);
    }
}

/* ---------------------------------------------------------------------
 * No-pack-loaded state.
 * ------------------------------------------------------------------- */

static void now_render_empty(lv_obj_t *parent)
{
    lv_obj_t *headline = lv_label_create(parent);
    lv_label_set_text(headline, "NO FESTIVAL LOADED");
    lv_obj_set_style_text_font(headline, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(headline, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_align(headline, LV_ALIGN_CENTER, 0, (int32_t)NOW_LAYOUT_EMPTY_HEADLINE_DY);

    lv_obj_t *sub = lv_label_create(parent);
    lv_label_set_text(sub, "Load a festpack to see what's playing");
    lv_obj_set_style_text_font(sub, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_set_width(sub, 280);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, (int32_t)NOW_LAYOUT_EMPTY_SUB_DY);
}

/* ---------------------------------------------------------------------
 * Entry point.
 * ------------------------------------------------------------------- */

void ff_scr_now_build(lv_obj_t *parent, ff_app_now_t const *now)
{
    if (parent == NULL || now == NULL) {
        return;
    }

    if (!now->pack_loaded) {
        now_render_empty(parent);
        return;
    }
    if (now->tbd) {
        now_render_tbd(parent, now);
        return;
    }
    now_render_live(parent, now);
}
