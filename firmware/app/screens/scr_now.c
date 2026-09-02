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
 *
 * Dispatch is a plain switch over `now->state` (now_state_t,
 * ff_app_state.h) — PR #21 code review finding #2/ruling replaced the
 * earlier `pack_loaded`+`tbd` bool pair with this enum specifically so
 * "which state is this" is a fact of the data, not something this file
 * has to re-derive from check order.
 *
 * S26 slice e renames (2026-09-01): "Now" -> "Lineup" everywhere the
 * user can read it. This file carries no standalone page-title string
 * of its own to rename (unlike scr_signals.c's header "SIGNALS" ->
 * "INBOX") — its only chrome header is "NOW PLAYING"
 * (now_render_live), which describes CONTENT that is currently live,
 * not this screen's own name, the same "now" the word / "now" the
 * screen-name distinction CLAUDE.md's "now" age-formatting caution
 * already draws elsewhere; left unchanged, along with "NOTHING LIVE
 * RIGHT NOW". The renamed occurrence for this face is the launcher's
 * own caption (scr_launcher.c: "NOW" -> "LINEUP"). Interpretation call,
 * noted per AGENTS.md — see the PR body.
 */
#include "scr_now.h"

#include <stdio.h>

#include "ff_theme.h"
#include "now_layout.h"
#include "radar_layout.h" /* RADAR_LAYOUT_PAGE_DOT_DY reuse — see this file's top comment and now_layout.h's */

/* Bottom keep-out for every scrollable/list section on this face: stay
 * clear of the page-dot row scr_nav.c draws directly on the puck (on top
 * of every tile, this one included) at RADAR_LAYOUT_PAGE_DOT_DY. That
 * row's own reserved rect (radar_layout.c's build_registry) spans
 * PAGE_DOT_DY +/- 10px; 36px of margin here is a deliberate cushion
 * beyond the bare minimum, not a tight fit. Derived from the SAME
 * constant radar_layout.c's registry uses, not a second independently-
 * chosen number that could drift out of sync with where the dots
 * actually are. Shared by NOW_TBD's full lineup AND NOW_MIXED's
 * still-unknown list — both end at the same physical boundary. */
#define NOW_SCR_LINEUP_BOTTOM_DY (RADAR_LAYOUT_PAGE_DOT_DY - 36.0f)

/* ---------------------------------------------------------------------
 * Small shared helpers.
 * ------------------------------------------------------------------- */

/* now_stage_or_unknown — the one explicit fallback string for "we don't
 * know this set's stage", used everywhere a stage name renders on this
 * face (now-playing rows, the next-starred card, the still-unknown
 * list). PR #21 UX review finding #2: this used to be applied
 * inconsistently (rows said "STAGE UNKNOWN", the TBD list silently
 * omitted the stage) — same fact, same words, everywhere now, by
 * construction rather than by remembering to copy the fallback at every
 * call site. */
static char const *now_stage_or_unknown(char const *stage_name)
{
    return (stage_name != NULL && stage_name[0] != '\0') ? stage_name : "STAGE UNKNOWN";
}

/* now_build_tbd_banner — the amber pill shared by NOW_TBD and NOW_MIXED
 * (PR #21 code review finding #1/ruling: "show the TBD banner whenever
 * ANY set on the day lacks a time" — the reviewer's literal wording).
 * `text` differs between the two callers (UX review round 2 finding #3:
 * NOW_TBD's literal "SET TIMES TBD" sitting directly above a "KNOWN SO
 * FAR" section that proves some times AREN'T TBD read as a wording
 * tension in NOW_MIXED — see now_render_mixed's "SOME SET TIMES TBD"). */
static void now_build_tbd_banner(lv_obj_t *parent, char const *text, int32_t dy)
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
    lv_obj_align(banner, LV_ALIGN_CENTER, 0, dy);

    lv_obj_t *banner_lbl = lv_label_create(banner);
    lv_label_set_text(banner_lbl, text);
    lv_obj_set_style_text_font(banner_lbl, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(banner_lbl, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_center(banner_lbl);
}

/* now_build_unknown_list — the scrollable "sets whose time isn't known"
 * list, shared by NOW_TBD (every entry on the day) and NOW_MIXED (just
 * the still-unknown subset — see ff_app_now_t.lineup's doc comment).
 * `top_dy`/`bottom_dy` bound it vertically; width is derived from the
 * puck's chord at `bottom_dy` (always the narrower of the two edges in
 * both callers' layouts — see now_layout.h's top comment).
 *
 * PR #21 code review finding #5b: an earlier version of this function
 * clamped the derived `list_w` to [120, 320] at runtime. Both call sites
 * always pass `bottom_dy = NOW_SCR_LINEUP_BOTTOM_DY`, a compile-time
 * constant, so `now_layout_chord_half_width_px(bottom_dy)` — and
 * therefore `list_w` — is the SAME fixed, already-known-safe value (281)
 * on every call; the clamp branches could never actually trigger. Removed
 * rather than kept as inert insurance: a real C11 `_Static_assert` can't
 * validate a non-constant-expression sqrtf() result, so a "static assert"
 * here would just be a comment wearing a macro's name. If a future caller
 * ever passes a different `bottom_dy`, that's a new layout decision to
 * verify at its call site, not a runtime clamp silently reinterpreting an
 * unexpected geometry as "fine, we bounded it". */
static void now_build_unknown_list(lv_obj_t *parent, ff_app_lineup_item_t const *lineup, uint8_t n_lineup,
                                    int32_t top_dy, int32_t bottom_dy)
{
    float half_w = now_layout_chord_half_width_px((float)bottom_dy);
    int32_t list_w = (int32_t)(half_w * 2.0f) - 40;
    int32_t list_h = bottom_dy - top_dy;
    int32_t list_cy = (top_dy + bottom_dy) / 2;

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

    if (n_lineup == 0) {
        lv_obj_t *empty_lbl = lv_label_create(list);
        lv_label_set_text(empty_lbl, "No sets listed for today yet");
        lv_obj_set_style_text_font(empty_lbl, FF_THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_color(empty_lbl, lv_color_hex(FF_THEME_COLOR_DIM), 0);
        return;
    }

    for (uint8_t i = 0; i < n_lineup && i < FF_APP_NOW_MAX_LINEUP; i++) {
        ff_app_lineup_item_t const *item = &lineup[i];
        char line[FF_APP_ARTIST_LEN + FF_APP_STAGE_LEN + 4];
        char const *artist = (item->artist[0] != '\0') ? item->artist : "(unknown)";
        /* Plain hyphen, not an em dash: LVGL's built-in Montserrat bitmap
         * fonts only cover the ASCII printable range by default (same
         * substitution scr_radar.c's radar_render_nofix already makes
         * for U+00B7, for the identical reason). */
        snprintf(line, sizeof(line), "%s - %s", artist, now_stage_or_unknown(item->stage_name));

        lv_obj_t *item_lbl = lv_label_create(list);
        lv_label_set_text(item_lbl, line);
        lv_obj_set_width(item_lbl, list_w);
        lv_obj_set_style_text_font(item_lbl, FF_THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_color(item_lbl, lv_color_hex(FF_THEME_COLOR_INK), 0);
    }
}

/* ---------------------------------------------------------------------
 * NOW_LIVE state.
 * ------------------------------------------------------------------- */

static void now_build_row(lv_obj_t *parent, ff_app_now_row_t const *row, int32_t row_dy)
{
    /* PR #21 code review finding #3: stage_color_valid, not "is
     * stage_color_rgb nonzero", decides the fallback — see
     * ff_app_now_row_t's doc comment (ff_app_state.h) for why treating 0
     * as the sentinel used to misrender both a genuinely black stage AND
     * a malformed fixture color the same silently-wrong way. */
    uint32_t stage_color = row->stage_color_valid ? row->stage_color_rgb : FF_THEME_COLOR_MUTED;

    lv_obj_t *stage_lbl = lv_label_create(parent);
    lv_label_set_text(stage_lbl, now_stage_or_unknown(row->stage_name));
    lv_obj_set_style_text_font(stage_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(stage_lbl, lv_color_hex(stage_color), 0);
    lv_obj_align(stage_lbl, LV_ALIGN_CENTER, 0, row_dy + (int32_t)NOW_LAYOUT_ROW_STAGE_OFFSET_DY);

    lv_obj_t *artist_lbl = lv_label_create(parent);
    lv_label_set_text(artist_lbl, (row->artist[0] != '\0') ? row->artist : "(unknown)");
    lv_obj_set_style_text_font(artist_lbl, FF_THEME_FONT_NAME, 0);
    lv_obj_set_style_text_color(artist_lbl, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_align(artist_lbl, LV_ALIGN_CENTER, 0, row_dy + (int32_t)NOW_LAYOUT_ROW_ARTIST_OFFSET_DY);

    /* 2026-08-24 amendment (S07 ## Amendments, "starts-only set grids"):
     * pct_valid false means this set's duration/progress is genuinely
     * unknowable (last known-start set on its stage this day, no end
     * published, no later same-stage set to derive one from) — NOT
     * "0% done". Rendering an empty or full-looking track either way
     * would claim knowledge this face doesn't have, so the whole
     * progress-bar element (track + fill) is omitted rather than shown
     * with a fabricated fill. See ff_app_now_row_t.pct_valid's doc
     * comment (ff_app_state.h) for the full contract. */
    if (row->pct_valid) {
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
    lv_label_set_text(stage_lbl, now_stage_or_unknown(next->stage_name));
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
 * NOW_NOTHING_PLAYING state — pack loaded, every set's time known,
 * nothing currently playing, nothing starred upcoming. A separate
 * top-level state (PR #21 code review finding #2/ruling), not a fallback
 * branch inside now_render_live: distinct from both NOW_NO_PACK and
 * NOW_TBD/NOW_MIXED (CLAUDE.md: unknown must stay explicit, never
 * collapse into a lookalike message).
 * ------------------------------------------------------------------- */

static void now_render_nothing_playing(lv_obj_t *parent)
{
    lv_obj_t *headline = lv_label_create(parent);
    lv_label_set_text(headline, "NOTHING LIVE RIGHT NOW");
    lv_obj_set_style_text_font(headline, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(headline, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_align(headline, LV_ALIGN_CENTER, 0, (int32_t)NOW_LAYOUT_NOTHING_PLAYING_HEADLINE_DY);

    lv_obj_t *sub = lv_label_create(parent);
    lv_label_set_text(sub, "Check back closer to your next set");
    lv_obj_set_style_text_font(sub, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, (int32_t)NOW_LAYOUT_NOTHING_PLAYING_SUB_DY);
}

/* ---------------------------------------------------------------------
 * NOW_TBD state.
 * ------------------------------------------------------------------- */

static void now_render_tbd(lv_obj_t *parent, ff_app_now_t const *now)
{
    now_build_tbd_banner(parent, "SET TIMES TBD", (int32_t)NOW_LAYOUT_TBD_BANNER_DY);
    now_build_unknown_list(parent, now->lineup, now->n_lineup, (int32_t)NOW_LAYOUT_LINEUP_TOP_DY,
                            (int32_t)NOW_SCR_LINEUP_BOTTOM_DY);
}

/* ---------------------------------------------------------------------
 * NOW_MIXED state (PR #21 code review finding #1/ruling): some of the
 * day's sets have a known time, some don't — both must be visible. The
 * fix for "a set with an unknown time used to disappear the moment ANY
 * set on the day got a real time" (the bug the ruling targets).
 *
 * UX review round 2 RULING: three classes inside "known so far", each
 * unmistakable at a glance, never distinguished by an ABSENCE of an
 * element (that's the same in-band-unknown mistake the color-validity
 * flag and the state enum already fixed elsewhere on this face):
 *   (a) playing now       -> now_build_row(), same treatment as NOW_LIVE.
 *   (b) scheduled, later   -> now_build_mixed_next(), countdown-LED, no bar.
 *   (c) time unknown       -> now_build_unknown_list(), unchanged.
 * ------------------------------------------------------------------- */

/* now_build_mixed_next — class (b): the starred-next set, INSIDE
 * NOW_MIXED's "known so far" section. Countdown-led and un-mistakably
 * prominent (UX review round 2 finding #2: round 1 gave NOW_LIVE's
 * next-card countdown FF_THEME_FONT_DISTANCE/36px specifically so it
 * reads as the anxiety-killer element; an inline 14px suffix on a plain
 * text line lost that entirely the moment the SAME field appeared in
 * NOW_MIXED instead). Not literally 36px here — this block shares the
 * screen with a variable-height "playing now" stack above it and the
 * still-unknown list below, so the full NOW_LIVE hero treatment doesn't
 * fit — but FF_THEME_FONT_NAME (22px, amber) is still clearly the
 * biggest, most colorful element in its own block and in the whole
 * "known so far" section (next to 14px header/artist/stage text
 * everywhere else here), which is the actual bar this needs to clear:
 * "reads as the prominent element of its row/card", not a specific
 * point size. No progress bar, ever — that absence is now what
 * DISTINGUISHES this from a now_build_row() entry, on purpose, but the
 * countdown lead is what a glance actually keys off, not the absence. */
static void now_build_mixed_next(lv_obj_t *parent, ff_app_next_t const *next, int32_t block_dy)
{
    char countdown[24];
    now_layout_format_countdown(next->mins_until, countdown, sizeof(countdown));
    lv_obj_t *cd_lbl = lv_label_create(parent);
    lv_label_set_text(cd_lbl, countdown);
    lv_obj_set_style_text_font(cd_lbl, FF_THEME_FONT_NAME, 0);
    lv_obj_set_style_text_color(cd_lbl, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_align(cd_lbl, LV_ALIGN_CENTER, 0, block_dy);

    char line[FF_APP_ARTIST_LEN + FF_APP_STAGE_LEN + 4];
    char const *artist = (next->artist[0] != '\0') ? next->artist : "(unknown)";
    snprintf(line, sizeof(line), "%s - %s", artist, now_stage_or_unknown(next->stage_name));
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, line);
    lv_obj_set_style_text_font(lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, block_dy + 20);
}

static void now_render_mixed(lv_obj_t *parent, ff_app_now_t const *now)
{
    /* UX review round 2 finding #3: NOT the literal "SET TIMES TBD" —
     * that wording sitting directly above a "KNOWN SO FAR" section that
     * proves some times AREN'T TBD read as a self-contradiction at a
     * glance. "SOME SET TIMES TBD" keeps the same banner (finding #1's
     * "same banner whenever ANY set lacks a time") honest about there
     * being a known/unknown split, not just an unqualified "TBD". */
    now_build_tbd_banner(parent, "SOME SET TIMES TBD", (int32_t)NOW_LAYOUT_MIXED_BANNER_DY);

    bool any_known = (now->n_rows > 0) || now->next.valid;
    int32_t cursor_dy = (int32_t)NOW_LAYOUT_MIXED_ROW0_DY;

    if (any_known) {
        lv_obj_t *header = lv_label_create(parent);
        lv_label_set_text(header, "KNOWN SO FAR");
        lv_obj_set_style_text_font(header, FF_THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_color(header, lv_color_hex(FF_THEME_COLOR_DIM), 0);
        lv_obj_align(header, LV_ALIGN_CENTER, 0, (int32_t)NOW_LAYOUT_MIXED_KNOWN_HEADER_DY);

        /* Class (a) first — reuses now_build_row() verbatim (stage-
         * colored label + progress bar), NOT a compact re-implementation,
         * so "playing now" is provably the identical visual fact in
         * every state it appears in. */
        for (uint8_t i = 0; i < now->n_rows && i < FF_APP_NOW_MAX_ROWS; i++) {
            now_build_row(parent, &now->rows[i], cursor_dy);
            cursor_dy += (int32_t)NOW_LAYOUT_MIXED_ROW_SPACING_DY;
        }
        /* Class (b) after: the one starred-next entry, countdown-led. */
        if (now->next.valid) {
            now_build_mixed_next(parent, &now->next, cursor_dy);
            cursor_dy += (int32_t)NOW_LAYOUT_MIXED_NEXT_BLOCK_H_DY;
        }
    }

    /* "STILL TBD" (class (c)) always shown: NOW_MIXED is defined as "at
     * least one known-time set AND at least one unknown-time set" (see
     * now_state_t's doc comment), so `lineup` is never empty here. Its
     * header sits just below wherever the known section actually ended
     * (or at a fixed fallback position if `any_known` was false — a
     * known-time set that's neither currently playing nor starred-
     * upcoming, e.g. one that already finished, is a real if unusual
     * case this still has to render honestly). */
    int32_t unknown_header_dy =
        any_known ? cursor_dy + (int32_t)NOW_LAYOUT_MIXED_SECTION_GAP_DY : (int32_t)NOW_LAYOUT_MIXED_UNKNOWN_HEADER_MIN_DY;

    lv_obj_t *unknown_header = lv_label_create(parent);
    lv_label_set_text(unknown_header, "STILL TBD");
    lv_obj_set_style_text_font(unknown_header, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(unknown_header, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_align(unknown_header, LV_ALIGN_CENTER, 0, unknown_header_dy);

    now_build_unknown_list(parent, now->lineup, now->n_lineup,
                            unknown_header_dy + (int32_t)NOW_LAYOUT_MIXED_UNKNOWN_HEADER_TO_LIST_GAP_DY,
                            (int32_t)NOW_SCR_LINEUP_BOTTOM_DY);
}

/* ---------------------------------------------------------------------
 * NOW_NO_PACK state.
 * ------------------------------------------------------------------- */

static void now_render_no_pack(lv_obj_t *parent)
{
    lv_obj_t *headline = lv_label_create(parent);
    lv_label_set_text(headline, "NO FESTIVAL LOADED");
    lv_obj_set_style_text_font(headline, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(headline, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_align(headline, LV_ALIGN_CENTER, 0, (int32_t)NOW_LAYOUT_NO_PACK_HEADLINE_DY);

    lv_obj_t *sub = lv_label_create(parent);
    lv_label_set_text(sub, "Load a festpack to see what's playing");
    lv_obj_set_style_text_font(sub, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_set_width(sub, 280);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, (int32_t)NOW_LAYOUT_NO_PACK_SUB_DY);
}

/* ---------------------------------------------------------------------
 * NOW_TIME_UNKNOWN state (issue #48).
 * ------------------------------------------------------------------- */

/* A festpack IS loaded — the missing fact is the CLOCK, not the pack, so
 * the copy names the clock ("WAITING FOR TIME FIX", echoing the radar
 * face's own NOFIX vocabulary for the same "we're honestly waiting on a
 * signal" shape — see scr_radar.c's radar_render_nofix) and never
 * mentions loading a festpack (that would repeat the exact mis-claim
 * this state exists to fix) or a schedule/TBD (that would be a claim
 * about DATA the projection never checked, since it bailed before
 * touching the pack's schedule at all — see ff_shell.c's
 * shell_project_now). No countdown, no lineup, no invented time: this
 * state's whole job is to say "I don't know what time it is" and stop
 * there. */
static void now_render_time_unknown(lv_obj_t *parent)
{
    lv_obj_t *headline = lv_label_create(parent);
    lv_label_set_text(headline, "WAITING FOR TIME FIX");
    lv_obj_set_style_text_font(headline, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(headline, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_width(headline, 320);
    lv_obj_set_style_text_align(headline, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(headline, LV_ALIGN_CENTER, 0, (int32_t)NOW_LAYOUT_TIME_UNKNOWN_HEADLINE_DY);

    lv_obj_t *sub = lv_label_create(parent);
    lv_label_set_text(sub, "Clock hasn't synced from the mesh yet");
    lv_obj_set_style_text_font(sub, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_set_width(sub, 280);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, (int32_t)NOW_LAYOUT_TIME_UNKNOWN_SUB_DY);
}

/* ---------------------------------------------------------------------
 * Entry point.
 * ------------------------------------------------------------------- */

void ff_scr_now_build(lv_obj_t *parent, ff_app_now_t const *now)
{
    if (parent == NULL || now == NULL) {
        return;
    }

    switch (now->state) {
    case NOW_TBD:
        now_render_tbd(parent, now);
        break;
    case NOW_MIXED:
        now_render_mixed(parent, now);
        break;
    case NOW_LIVE:
        now_render_live(parent, now);
        break;
    case NOW_NOTHING_PLAYING:
        now_render_nothing_playing(parent);
        break;
    case NOW_TIME_UNKNOWN:
        now_render_time_unknown(parent);
        break;
    case NOW_NO_PACK:
    default:
        now_render_no_pack(parent);
        break;
    }
}
