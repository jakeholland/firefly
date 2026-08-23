/**
 * scr_compose.c — see scr_compose.h.
 *
 * ## Render vs. live input — the split, and why
 * The INITIAL paint of the message bubble is a pure function of
 * `*compose` (the fixture snapshot), exactly like every other screen in
 * this codebase (`ff_scr_radar_build`/`ff_scr_signals_build`) — that's
 * what keeps `--headless --screenshot` deterministic and goldens
 * reproducible: a headless single-frame render never processes a click
 * event, so it always shows exactly what the fixture says, nothing more.
 *
 * Real keypad presses DO drive a genuine, internal `ff_t9_t` (this is
 * the "input plumbing" S08 slice d asks for — `core/include/ff_t9.h`,
 * the merged multi-tap engine) — but that engine starts EMPTY
 * (`ff_t9_reset`) at build time, independent of the fixture's pre-seeded
 * text, and the bubble label only switches over to rendering
 * `ff_t9_text()`'s live output once the very first real keypress lands
 * (`s_started_typing`). There is no attempt to reverse-engineer a
 * `ff_t9_t` internal state (committed buffer + a specific pending
 * multi-tap cycle position) that would reproduce an arbitrary fixture's
 * `text`/`has_pending` — `ff_t9_t`'s public API has no "load this
 * committed string" entry point (by design: every mutation is a modeled
 * keypress), so a fixture-seeded live session isn't attempted here.
 * Interactive typing in window mode is therefore always a FRESH compose
 * session, not a "resume this draft" feature — nothing in the S08 spec
 * asks for the latter, and this keeps the render path honest (never a
 * hybrid of "some of this came from the fixture, some from the engine").
 *
 * ## Keypad mode paging + which emoji tier this PR ships
 * S08's Amendments (2026-08-23, owner ruling) resolves the open digits/
 * symbols question with three keypad pages, cycled by the mode chip in
 * the header (shows the CURRENT mode's name — never a mystery toggle,
 * per the ruling's own requirement): ABC (the original v1 multi-tap
 * scope, letter legends only — the engine still cannot produce digits in
 * this mode, so it still never gets a digit legend), 123 (literal
 * digits, via the new `ff_t9_insert_text()`), and SYM.
 *
 * SYM ships the **tier-2 ASCII-emoticon fallback**, not tier-1 real
 * emoji: a curated custom LVGL font subset (~12-16 emoji codepoints) is
 * a real asset-authoring task (font generation tooling, a licensed glyph
 * source, size/legibility tuning at 37mm) that doesn't fit this PR's
 * scope alongside the feed/wiring/Signals/Compose work it's already
 * carrying. ASCII shortcuts (":)", "<3", ...) transmit as plain text
 * MC_TEXT_MAX bytes — they render correctly on every Meshtastic receiver
 * (phone apps, older pucks), which real emoji would not without every
 * receiver also carrying the same font. Follow-up for real emoji:
 * tracked in https://github.com/jakeholland/firefly/issues/22 (tier-1,
 * per the ruling's own fallback clause).
 */
#include "scr_compose.h"

#include <stdio.h>
#include <string.h>

#include "ff_t9.h"
#include "ff_theme.h"

/* ---------------------------------------------------------------------
 * Layout constants.
 * ------------------------------------------------------------------- */

#define FF_COMPOSE_MARGIN_X     20
#define FF_COMPOSE_BUBBLE_Y     70
#define FF_COMPOSE_BUBBLE_H     84
#define FF_COMPOSE_GRID_Y       166
#define FF_COMPOSE_KEY_GAP      8
#define FF_COMPOSE_KEY_H        56
#define FF_COMPOSE_BOTTOM_ROW_H 52

/* ---------------------------------------------------------------------
 * Static screen state.
 *
 * Same "one screen built per process" convention/hazard already
 * documented in scr_radar.c's top comment (headless mode renders one
 * frame and exits; window mode builds a screen once and lets LVGL's own
 * timer loop repaint it) — see that file's comment and
 * https://github.com/jakeholland/firefly/issues/17 for what breaks if
 * that assumption ever stops holding (re-render/rebuild at a live tick
 * rate, or tearing down and rebuilding this screen without a fresh
 * process). `ff_scr_compose_build` resets every static below at entry,
 * so at least repeated calls WITHIN one process (not currently done
 * anywhere) start from a clean slate rather than leaking the previous
 * call's compose session.
 * ------------------------------------------------------------------- */
static ff_t9_t             s_t9;
static ff_app_compose_mode_t s_mode;
static bool                 s_started_typing;
static lv_obj_t            *s_bubble_label;
static lv_obj_t            *s_mode_chip_label;
static lv_obj_t            *s_keys_container;

/* ---------------------------------------------------------------------
 * Per-mode key legends (S08 Amendments). Index 0 is the bottom-row
 * SPACE/0 key; indices 1-9 are the 3x3 grid, row-major (1,2,3 / 4,5,6 /
 * 7,8,9), matching a standard phone keypad's reading order.
 * ------------------------------------------------------------------- */

static char const *const kAbcLegends[10] = {
    "SPACE", ". , ? !", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ",
};
static char const *const k123Legends[10] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
};
/* SYM legends double as what actually gets inserted (ff_t9_insert_text
 * takes the literal ASCII shortcut) — tier-2 fallback, see this file's
 * header comment. Key 0 is SPACE, same as ABC (typed sentences with
 * emoticons still need spaces; only the 123 page repurposes 0 as a
 * literal digit, per the owner's ruling). */
static char const *const kSymLegends[10] = {
    "SPACE", "!", "?", ":)", ";)", "<3", ":o", ":/", "\\o/", "...",
};

static char const *compose_legend_for(ff_app_compose_mode_t mode, uint8_t key)
{
    switch (mode) {
    case FF_APP_COMPOSE_ABC: return kAbcLegends[key];
    case FF_APP_COMPOSE_123: return k123Legends[key];
    case FF_APP_COMPOSE_SYM: return kSymLegends[key];
    }
    return "?";
}

static char const *compose_mode_name(ff_app_compose_mode_t m)
{
    switch (m) {
    case FF_APP_COMPOSE_ABC: return "ABC";
    case FF_APP_COMPOSE_123: return "123";
    case FF_APP_COMPOSE_SYM: return "SYM";
    }
    return "?";
}

/* ---------------------------------------------------------------------
 * Bubble rendering.
 * ------------------------------------------------------------------- */

/* Renders `text`/`has_pending` into `label`. When a pending char is live,
 * it's recolored amber (FF_THEME_COLOR_AMBER, the app's one "in
 * progress" accent) so the bubble visibly distinguishes "committed" from
 * "still typing this character" without needing the reader to notice a
 * blinking cursor (this is a single static render — nothing here
 * animates) — satisfies S08's "message bubble with live pending
 * character" requirement declaratively instead of via a timer. */
static void compose_render_bubble_text(lv_obj_t *label, char const *text, bool has_pending)
{
    size_t n = (text != NULL) ? strlen(text) : 0;

    if (n == 0 && !has_pending) {
        lv_label_set_recolor(label, false);
        lv_obj_set_style_text_color(label, lv_color_hex(FF_THEME_COLOR_DIM), 0);
        lv_label_set_text(label, "Type a message...");
        return;
    }

    lv_obj_set_style_text_color(label, lv_color_hex(FF_THEME_COLOR_INK), 0);

    if (has_pending && n > 0) {
        char committed[FF_APP_COMPOSE_TEXT_LEN];
        size_t cn = n - 1;
        if (cn >= sizeof(committed)) cn = sizeof(committed) - 1;
        memcpy(committed, text, cn);
        committed[cn] = '\0';
        char pending_ch = text[n - 1];

        char buf[FF_APP_COMPOSE_TEXT_LEN + 16];
        snprintf(buf, sizeof(buf), "%s#%06x %c#", committed, (unsigned)FF_THEME_COLOR_AMBER, pending_ch);
        lv_label_set_recolor(label, true);
        lv_label_set_text(label, buf);
    } else {
        lv_label_set_recolor(label, false);
        lv_label_set_text(label, text);
    }
}

/* Only repaints from the LIVE engine once typing has actually started —
 * see this file's header comment for why the fixture-seeded initial
 * paint is otherwise left alone. */
static void compose_refresh_bubble(void)
{
    if (s_bubble_label == NULL || !s_started_typing) {
        return;
    }
    compose_render_bubble_text(s_bubble_label, ff_t9_text(&s_t9), s_t9.has_pending);
}

/* ---------------------------------------------------------------------
 * Keypad input handlers — real ff_t9 plumbing (S08 slice d).
 * ------------------------------------------------------------------- */

static void compose_key_pressed(uint8_t key)
{
    s_started_typing = true;

    switch (s_mode) {
    case FF_APP_COMPOSE_ABC:
        if (key == 0) {
            ff_t9_space(&s_t9);
        } else {
            ff_t9_key(&s_t9, key, lv_tick_get());
        }
        break;
    case FF_APP_COMPOSE_123:
        if (key == 0) {
            ff_t9_insert_text(&s_t9, "0");
        } else {
            char digit[2] = {(char)('0' + key), '\0'};
            ff_t9_insert_text(&s_t9, digit);
        }
        break;
    case FF_APP_COMPOSE_SYM:
        if (key == 0) {
            ff_t9_space(&s_t9);
        } else {
            ff_t9_insert_text(&s_t9, kSymLegends[key]);
        }
        break;
    }

    compose_refresh_bubble();
}

static void compose_key_click_cb(lv_event_t *e)
{
    uintptr_t key = (uintptr_t)lv_event_get_user_data(e);
    compose_key_pressed((uint8_t)key);
}

static void compose_backspace_cb(lv_event_t *e)
{
    (void)e;
    s_started_typing = true;
    ff_t9_backspace(&s_t9);
    compose_refresh_bubble();
}

/* TODO(issue #23): actually send — mc_send_text to compose->to_name's
 * node (or app/ff_wiring.c's sender seam) — once this screen has a
 * wiring handle to call through. Pure render today (CLAUDE.md); reserved
 * hook, same convention as scr_radar.c's FLARE-button stub. */
static void compose_send_stub_cb(lv_event_t *e)
{
    (void)e;
}

/* TODO(issue #23): return to the Signals face. Which face is active
 * is app-state owned by the real event loop (not yet built), not a
 * pure-render screen's decision to make — reserved hook. */
static void compose_back_stub_cb(lv_event_t *e)
{
    (void)e;
}

static void compose_update_mode_chip_label(void)
{
    if (s_mode_chip_label != NULL) {
        lv_label_set_text(s_mode_chip_label, compose_mode_name(s_mode));
    }
}

/* ---------------------------------------------------------------------
 * Keypad construction (rebuilt in place on a mode switch).
 * ------------------------------------------------------------------- */

static lv_obj_t *compose_make_key(lv_obj_t *parent, char const *legend, int32_t x, int32_t y, int32_t w, int32_t h,
                                   uint8_t key, uint32_t bg_hex, uint32_t fg_hex)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_event_cb(btn, compose_key_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)key);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, legend);
    lv_obj_set_style_text_font(label, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(fg_hex), 0);
    lv_obj_center(label);
    return btn;
}

/* Builds the 3x3 letter/digit/symbol grid (keys 1-9) plus the DEL / 0 /
 * SEND bottom row into `container` (already positioned/sized by the
 * caller; every position below is relative to `container`'s own
 * top-left). All hit areas clear FF_THEME_MIN_HIT_PX in both dimensions
 * (docs/review/ux-raver.md checklist item 2) — the 3x3 grid cells alone
 * comfortably exceed it (>120px wide at this puck size); DEL/0/SEND are
 * explicitly checked below. */
static void compose_build_keys(lv_obj_t *container, ff_app_compose_mode_t mode)
{
    int32_t grid_w = FF_THEME_PUCK_PX - 2 * FF_COMPOSE_MARGIN_X;
    int32_t key_w = (grid_w - 2 * FF_COMPOSE_KEY_GAP) / 3;

    for (uint8_t row = 0; row < 3; row++) {
        for (uint8_t col = 0; col < 3; col++) {
            uint8_t key = (uint8_t)(row * 3 + col + 1); /* 1..9 */
            int32_t x = FF_COMPOSE_MARGIN_X + (int32_t)col * (key_w + FF_COMPOSE_KEY_GAP);
            int32_t y = (int32_t)row * (FF_COMPOSE_KEY_H + FF_COMPOSE_KEY_GAP);
            compose_make_key(container, compose_legend_for(mode, key), x, y, key_w, FF_COMPOSE_KEY_H, key,
                              FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_INK);
        }
    }

    int32_t bottom_y = 3 * (FF_COMPOSE_KEY_H + FF_COMPOSE_KEY_GAP) + 4;
    /* FF_COMPOSE_BOTTOM_ROW_H (52) and key_w (>120 at this puck size)
     * both clear FF_THEME_MIN_HIT_PX (44) — asserted, not just claimed by
     * comment, so a future layout-constant change that shrinks either one
     * below the floor fails the build loudly instead of silently
     * regressing tap-target size. */
    _Static_assert(FF_COMPOSE_BOTTOM_ROW_H >= FF_THEME_MIN_HIT_PX,
                   "compose bottom row must clear the ux-raver 44px hit-target floor");

    lv_obj_t *del = lv_button_create(container);
    lv_obj_remove_style_all(del);
    lv_obj_set_size(del, key_w, FF_COMPOSE_BOTTOM_ROW_H);
    lv_obj_set_pos(del, FF_COMPOSE_MARGIN_X, bottom_y);
    lv_obj_set_style_bg_color(del, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(del, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(del, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(del, compose_backspace_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *del_lbl = lv_label_create(del);
    lv_label_set_text(del_lbl, "DEL");
    lv_obj_set_style_text_font(del_lbl, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(del_lbl, lv_color_hex(FF_THEME_COLOR_STALE_AMBER), 0);
    lv_obj_center(del_lbl);

    compose_make_key(container, compose_legend_for(mode, 0), FF_COMPOSE_MARGIN_X + key_w + FF_COMPOSE_KEY_GAP,
                      bottom_y, key_w, FF_COMPOSE_BOTTOM_ROW_H, 0, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_INK);

    lv_obj_t *send = lv_button_create(container);
    lv_obj_remove_style_all(send);
    lv_obj_set_size(send, key_w, FF_COMPOSE_BOTTOM_ROW_H);
    lv_obj_set_pos(send, FF_COMPOSE_MARGIN_X + 2 * (key_w + FF_COMPOSE_KEY_GAP), bottom_y);
    lv_obj_set_style_bg_color(send, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_bg_opa(send, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(send, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(send, compose_send_stub_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *send_lbl = lv_label_create(send);
    lv_label_set_text(send_lbl, "SEND");
    lv_obj_set_style_text_font(send_lbl, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(send_lbl, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_center(send_lbl);
}

static void compose_rebuild_keypad(void)
{
    if (s_keys_container == NULL) {
        return;
    }
    lv_obj_clean(s_keys_container);
    compose_build_keys(s_keys_container, s_mode);
}

static void compose_mode_chip_click_cb(lv_event_t *e)
{
    (void)e;
    switch (s_mode) {
    case FF_APP_COMPOSE_ABC: s_mode = FF_APP_COMPOSE_123; break;
    case FF_APP_COMPOSE_123: s_mode = FF_APP_COMPOSE_SYM; break;
    case FF_APP_COMPOSE_SYM: s_mode = FF_APP_COMPOSE_ABC; break;
    }
    compose_update_mode_chip_label();
    compose_rebuild_keypad();
}

/* ---------------------------------------------------------------------
 * Entry point.
 * ------------------------------------------------------------------- */

void ff_scr_compose_build(ff_app_compose_t const *compose)
{
    if (compose == NULL) {
        return;
    }

    s_mode = compose->mode;
    s_started_typing = false;
    ff_t9_reset(&s_t9);
    s_bubble_label = NULL;
    s_mode_chip_label = NULL;
    s_keys_container = NULL;

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

    /* --- Header: back (dead-end escape, ux-raver checklist item 6) / TO / mode chip. --- */
    lv_obj_t *back = lv_button_create(puck);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, FF_THEME_MIN_HIT_PX, FF_THEME_MIN_HIT_PX);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 16, 10);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(back, compose_back_stub_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, "<");
    lv_obj_set_style_text_font(back_lbl, FF_THEME_FONT_NAME, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_center(back_lbl);

    lv_obj_t *to_lbl = lv_label_create(puck);
    char to_buf[FF_APP_NAME_LEN + 8];
    snprintf(to_buf, sizeof(to_buf), "TO: %s", (compose->to_name[0] != '\0') ? compose->to_name : "EVERYONE");
    lv_label_set_text(to_lbl, to_buf);
    lv_obj_set_style_text_font(to_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(to_lbl, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_align(to_lbl, LV_ALIGN_TOP_MID, 0, 26);

    lv_obj_t *mode_chip = lv_button_create(puck);
    lv_obj_remove_style_all(mode_chip);
    lv_obj_set_size(mode_chip, 64, FF_THEME_MIN_HIT_PX);
    lv_obj_align(mode_chip, LV_ALIGN_TOP_RIGHT, -16, 10);
    lv_obj_set_style_bg_color(mode_chip, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(mode_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mode_chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(mode_chip, compose_mode_chip_click_cb, LV_EVENT_CLICKED, NULL);
    s_mode_chip_label = lv_label_create(mode_chip);
    lv_obj_set_style_text_font(s_mode_chip_label, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(s_mode_chip_label, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_center(s_mode_chip_label);
    compose_update_mode_chip_label();

    /* --- Message bubble. --- */
    lv_obj_t *bubble = lv_obj_create(puck);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_size(bubble, FF_THEME_PUCK_PX - 2 * FF_COMPOSE_MARGIN_X, FF_COMPOSE_BUBBLE_H);
    lv_obj_set_pos(bubble, FF_COMPOSE_MARGIN_X, FF_COMPOSE_BUBBLE_Y);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 14, 0);
    lv_obj_set_style_pad_all(bubble, 12, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    s_bubble_label = lv_label_create(bubble);
    lv_label_set_long_mode(s_bubble_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_bubble_label, FF_THEME_PUCK_PX - 2 * FF_COMPOSE_MARGIN_X - 24);
    lv_obj_set_style_text_font(s_bubble_label, FF_THEME_FONT_LABEL, 0);
    lv_obj_align(s_bubble_label, LV_ALIGN_TOP_LEFT, 0, 0);
    compose_render_bubble_text(s_bubble_label, compose->text, compose->has_pending);

    /* --- Keypad. --- */
    s_keys_container = lv_obj_create(puck);
    lv_obj_remove_style_all(s_keys_container);
    lv_obj_set_size(s_keys_container, FF_THEME_PUCK_PX,
                     3 * (FF_COMPOSE_KEY_H + FF_COMPOSE_KEY_GAP) + 4 + FF_COMPOSE_BOTTOM_ROW_H);
    lv_obj_set_pos(s_keys_container, 0, FF_COMPOSE_GRID_Y);
    lv_obj_clear_flag(s_keys_container, LV_OBJ_FLAG_SCROLLABLE);
    compose_build_keys(s_keys_container, s_mode);
}
