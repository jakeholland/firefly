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
 * As of S16 slice c3 this file no longer holds a live `ff_t9_t` at all —
 * it moved to shell-owned draft state (`app/ff_shell.c`'s `compose_draft`,
 * per the spec's Intents section: "the draft is shell-owned T9 state").
 * Every keypad control below is a pure emitter, same shape as SEND/BACK
 * already were: it reports WHICH key was pressed, in the context of the
 * mode this screen itself is currently showing (`s_mode`, still a local
 * snapshot from `*compose` at build time — deciding "is this a multi-tap
 * letter key or a literal insert" is what makes the intent semantic
 * rather than a bare hardware scancode), and the shell decides what, if
 * anything, mutates. This screen never touches `ff_t9.h` directly anymore
 * — no engine, no local echo, nothing to reset. A live session's bubble
 * only reflects a keypress once the shell's next projection reaches the
 * screen through the render lifecycle (S16 slice d); until then, exactly
 * like every other button in this codebase, a click here is a report, not
 * a repaint.
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
 *
 * ## Round-glass layout (PR #25 UX review, blocking finding)
 * The first pass of this file positioned header/footer chrome against
 * the puck's SQUARE bounding box (`LV_ALIGN_TOP_LEFT`/`TOP_RIGHT` with
 * flat pixel offsets) — the back button ended up ~42px entirely off the
 * round glass, the mode chip ~35px off, DEL/SEND lost roughly half their
 * tappable area. Every position below is now computed from
 * `ff_layout_chord_half_width` (app/screens/ff_layout.h — hoisted out of
 * this bug specifically so the next face doesn't repeat it) against the
 * puck's REAL circle, not eyeballed against its square corners: each
 * row's horizontal margin is derived from how wide the circle actually
 * is at that row's OWN worst-case (farthest-from-center) y, with a fixed
 * `FF_COMPOSE_SAFETY_PX` buffer subtracted for slack. This is why the
 * grid's rows get progressively MORE inset the closer they sit to the
 * puck's bottom pole (the reviewer's own suggested fix: "a grid inscribed
 * in a circle wants a narrower row" near the edge, not a uniform one) —
 * and it's asserted, not just designed-and-hoped: see
 * targets/sim/tests/test_face_hit_targets.c, which builds this exact
 * screen from every committed fixture and fails if any hit-rect ever
 * drifts back outside the circle.
 */
#include "scr_compose.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ff_intent.h" /* S16c1 — the emit seam; see compose_back_cb */
#include "ff_layout.h"
#include "ff_theme.h"

/* ---------------------------------------------------------------------
 * Layout constants.
 *
 * Row Y positions/heights are the fixed design choices; horizontal
 * margins are NOT independently chosen numbers — they're derived from
 * these Y values via compose_safe_margin_x() below, so the two can never
 * drift out of sync the way the original off-glass layout did (a Y
 * value could change here and the margins would automatically follow).
 * ------------------------------------------------------------------- */

#define FF_COMPOSE_SAFETY_PX 10.0f /* inset from the true glass edge — comfortable slack,
                                     * same "generous inset" spirit as radar_layout.h's
                                     * RING_RADIUS_PX (185) vs the puck's own 220px radius */

#define FF_COMPOSE_HEADER_Y 56
#define FF_COMPOSE_HEADER_H FF_THEME_MIN_HIT_PX /* back button / mode chip */

#define FF_COMPOSE_BUBBLE_Y (FF_COMPOSE_HEADER_Y + FF_COMPOSE_HEADER_H + 8)
#define FF_COMPOSE_BUBBLE_H 84

#define FF_COMPOSE_GRID_Y (FF_COMPOSE_BUBBLE_Y + FF_COMPOSE_BUBBLE_H + 8)
#define FF_COMPOSE_KEY_GAP 6
#define FF_COMPOSE_KEY_H   46 /* >= FF_THEME_MIN_HIT_PX; see the _Static_assert below */
#define FF_COMPOSE_BOTTOM_ROW_GAP_EXTRA 4
#define FF_COMPOSE_BOTTOM_ROW_H 46 /* >= FF_THEME_MIN_HIT_PX; see the _Static_assert below */

_Static_assert(FF_COMPOSE_KEY_H >= FF_THEME_MIN_HIT_PX, "compose grid keys must clear the 44px hit-target floor");
_Static_assert(FF_COMPOSE_BOTTOM_ROW_H >= FF_THEME_MIN_HIT_PX,
               "compose bottom row must clear the 44px hit-target floor");

/* Row 0 (keys 1-3), row 1 (keys 4-6), row 2 (keys 7-9), then the DEL/0/SEND row. */
#define FF_COMPOSE_ROW0_Y (FF_COMPOSE_GRID_Y)
#define FF_COMPOSE_ROW1_Y (FF_COMPOSE_ROW0_Y + FF_COMPOSE_KEY_H + FF_COMPOSE_KEY_GAP)
#define FF_COMPOSE_ROW2_Y (FF_COMPOSE_ROW1_Y + FF_COMPOSE_KEY_H + FF_COMPOSE_KEY_GAP)
#define FF_COMPOSE_BOTTOM_ROW_Y (FF_COMPOSE_ROW2_Y + FF_COMPOSE_KEY_H + FF_COMPOSE_KEY_GAP + FF_COMPOSE_BOTTOM_ROW_GAP_EXTRA)

/* ---------------------------------------------------------------------
 * Chord-aware horizontal margin — see this file's header comment and
 * ff_layout.h's own doc comment for ff_layout_chord_half_width.
 * ------------------------------------------------------------------- */

/**
 * compose_safe_margin_x — thin int32_t/ceil wrapper around
 * ff_layout.h's shared `ff_layout_safe_margin_x`, bound to this puck's
 * own center/radius (ff_theme.h) and this file's safety buffer. Kept
 * local (not inlined at every call site) purely so the six call sites
 * below don't each repeat the `FF_THEME_PUCK_RADIUS_PX,
 * FF_THEME_PUCK_RADIUS_PX, FF_COMPOSE_SAFETY_PX` argument triple.
 */
static int32_t compose_safe_margin_x(int32_t top_y, int32_t h)
{
    float margin = ff_layout_safe_margin_x((float)top_y, (float)h, (float)FF_THEME_PUCK_RADIUS_PX,
                                            (float)FF_THEME_PUCK_RADIUS_PX, FF_COMPOSE_SAFETY_PX);
    return (int32_t)ceilf(margin);
}

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
 * call's compose session. `s_mode` is the one piece of state this file
 * still keeps: a build-time snapshot of `*compose`, needed to decide
 * which intent a keypress means (T9_KEY vs. T9_INSERT — see
 * compose_key_pressed) — not live state, since nothing here mutates it
 * anymore (S16 slice c3 moved the mode chip's cycling to the shell too;
 * see compose_mode_chip_click_cb).
 * ------------------------------------------------------------------- */
static ff_app_compose_mode_t s_mode;
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

/* ---------------------------------------------------------------------
 * Keypad input handlers — S16 slice c3: every path below is a pure
 * emitter now (see this file's header comment). `s_mode` (the build-time
 * snapshot) decides which INTENT a keypress means, matching exactly what
 * the removed `ff_t9_*` calls used to do directly:
 *   ABC key 0    -> T9_SPACE          ABC key 1-9  -> T9_KEY(key)
 *   123 key 0-9  -> T9_INSERT("0".."9")  (still a literal digit insert,
 *                   same as the engine-side call this replaces)
 *   SYM key 0    -> T9_SPACE          SYM key 1-9  -> T9_INSERT(legend)
 * The shell interprets T9_KEY as multi-tap and T9_INSERT as an atomic
 * append (ff_t9_key / ff_t9_insert_text respectively) — this file never
 * calls either again.
 * ------------------------------------------------------------------- */

static void compose_key_pressed(uint8_t key)
{
    ff_intent_t in = {.kind = FF_INTENT_T9_SPACE, .u = {0}};

    switch (s_mode) {
    case FF_APP_COMPOSE_ABC:
        if (key == 0) {
            in.kind = FF_INTENT_T9_SPACE;
        } else {
            in.kind = FF_INTENT_T9_KEY;
            in.u.t9_key = key;
        }
        break;
    case FF_APP_COMPOSE_123: {
        char digit[2] = {(char)('0' + key), '\0'};
        in.kind = FF_INTENT_T9_INSERT;
        in.u.text = digit;
        ff_intent_emit(&in); /* emit here: `digit` doesn't outlive this block */
        return;
    }
    case FF_APP_COMPOSE_SYM:
        if (key == 0) {
            in.kind = FF_INTENT_T9_SPACE;
        } else {
            in.kind = FF_INTENT_T9_INSERT;
            in.u.text = kSymLegends[key];
        }
        break;
    }

    ff_intent_emit(&in);
}

static void compose_key_click_cb(lv_event_t *e)
{
    uintptr_t key = (uintptr_t)lv_event_get_user_data(e);
    compose_key_pressed((uint8_t)key);
}

static void compose_backspace_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_T9_BACKSPACE, .u = {0}};
    ff_intent_emit(&in);
}

/* SEND -> emits FF_INTENT_SEND_TEXT through the intent seam (S16 slice
 * c2 wired the emit; c3 wires the shell's handling of it). No payload:
 * the draft is shell-owned T9 state (`app/ff_shell.c`'s `compose_draft`)
 * as of this slice, so `ff_shell_intent`'s FF_INTENT_SEND_TEXT case
 * actually sends now, via `sh->wiring.sender` — see ff_shell.c. Routing
 * rule 4 ("a touch landing where SEND was does not send" while a takeover
 * is up) is enforced there, not here: this button still only ever reports
 * the tap. Unbound (goldens/headless), the emit is a no-op. */
static void compose_send_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_SEND_TEXT, .u = {0}};
    ff_intent_emit(&in);
}

/* Back "<" -> emits FF_INTENT_BACK through the intent seam (S16 slice
 * c1 — this replaces the issue-#23 stub). Which face is revealed is
 * still not this file's decision: the shell pops its modal route and
 * the next projection says what to render. Unbound (goldens/headless),
 * the emit is a no-op. */
static void compose_back_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_BACK, .u = {0}};
    ff_intent_emit(&in);
}

static void compose_update_mode_chip_label(void)
{
    if (s_mode_chip_label != NULL) {
        lv_label_set_text(s_mode_chip_label, compose_mode_name(s_mode));
    }
}

/* Mode chip "ABC"/"123"/"SYM" -> emits FF_INTENT_T9_MODE (S16 slice c3).
 * This used to cycle `s_mode` locally and rebuild the keypad in place for
 * instant feedback; now, like every other control in this file, it only
 * reports the tap — the shell owns the mode (so SEND/backspace/keys stay
 * consistent with whatever mode is actually current even across a
 * takeover interruption) and the next projection reaches this screen
 * through the render lifecycle (S16 slice d), not through a local
 * rebuild here. */
static void compose_mode_chip_click_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_T9_MODE, .u = {0}};
    ff_intent_emit(&in);
}

/* ---------------------------------------------------------------------
 * Keypad construction. Built once, at ff_scr_compose_build entry — S16
 * slice c3 retired the local "clear and rebuild in place" path a mode
 * chip tap used to trigger; a live mode change now reaches this screen
 * only through the render lifecycle (S16 slice d), same as everything
 * else the shell owns.
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

/* Builds ONE row of the grid (either the 3-key letter/digit/symbol row,
 * or the DEL/0/SEND row conceptually — callers pass their own widths)
 * with `margin_x` computed per-row by the caller via
 * compose_safe_margin_x — see this file's header comment for why each
 * row gets its OWN margin instead of one uniform value for the whole
 * grid: the circle narrows as y moves away from center, so a margin
 * generous enough for the bottom row would waste usable width on rows
 * closer to center, while a margin sized for the top row would leave the
 * bottom row off-glass — exactly the original bug. */
static void compose_build_grid_row(lv_obj_t *container, ff_app_compose_mode_t mode, int32_t y, uint8_t first_key,
                                    int32_t margin_x)
{
    int32_t row_w = FF_THEME_PUCK_PX - 2 * margin_x;
    int32_t key_w = (row_w - 2 * FF_COMPOSE_KEY_GAP) / 3;

    for (int32_t col = 0; col < 3; col++) {
        uint8_t key = (uint8_t)(first_key + col);
        int32_t x = margin_x + col * (key_w + FF_COMPOSE_KEY_GAP);
        compose_make_key(container, compose_legend_for(mode, key), x, y, key_w, FF_COMPOSE_KEY_H, key,
                          FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_INK);
    }
}

static void compose_build_bottom_row(lv_obj_t *container, ff_app_compose_mode_t mode, int32_t y, int32_t margin_x)
{
    int32_t row_w = FF_THEME_PUCK_PX - 2 * margin_x;
    int32_t btn_w = (row_w - 2 * FF_COMPOSE_KEY_GAP) / 3;

    lv_obj_t *del = lv_button_create(container);
    lv_obj_remove_style_all(del);
    lv_obj_set_size(del, btn_w, FF_COMPOSE_BOTTOM_ROW_H);
    lv_obj_set_pos(del, margin_x, y);
    lv_obj_set_style_bg_color(del, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(del, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(del, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(del, compose_backspace_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *del_lbl = lv_label_create(del);
    lv_label_set_text(del_lbl, "DEL");
    lv_obj_set_style_text_font(del_lbl, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(del_lbl, lv_color_hex(FF_THEME_COLOR_STALE_AMBER), 0);
    lv_obj_center(del_lbl);

    compose_make_key(container, compose_legend_for(mode, 0), margin_x + btn_w + FF_COMPOSE_KEY_GAP, y, btn_w,
                      FF_COMPOSE_BOTTOM_ROW_H, 0, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_INK);

    lv_obj_t *send = lv_button_create(container);
    lv_obj_remove_style_all(send);
    lv_obj_set_size(send, btn_w, FF_COMPOSE_BOTTOM_ROW_H);
    lv_obj_set_pos(send, margin_x + 2 * (btn_w + FF_COMPOSE_KEY_GAP), y);
    lv_obj_set_style_bg_color(send, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_bg_opa(send, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(send, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(send, compose_send_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *send_lbl = lv_label_create(send);
    lv_label_set_text(send_lbl, "SEND");
    lv_obj_set_style_text_font(send_lbl, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(send_lbl, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_center(send_lbl);
}

/* Builds the 3x3 letter/digit/symbol grid (keys 1-9) plus the DEL / 0 /
 * SEND bottom row into `container` — one row at a time, each with its
 * OWN chord-derived margin (see compose_safe_margin_x).
 *
 * `container` itself is positioned at puck-local y = FF_COMPOSE_GRID_Y
 * (set in ff_scr_compose_build), so every child position passed to
 * `lv_obj_set_pos` below must be CONTAINER-relative (subtract
 * FF_COMPOSE_GRID_Y) — but compose_safe_margin_x's circle math needs the
 * ABSOLUTE puck-local y (the circle's center is defined in that space,
 * not the container's). The FF_COMPOSE_ROW*_Y / FF_COMPOSE_BOTTOM_ROW_Y
 * constants are absolute; this function is the one place that converts
 * between the two spaces, so that conversion can't drift out of sync
 * between the margin calculation and the actual child placement. */
static void compose_build_keys(lv_obj_t *container, ff_app_compose_mode_t mode)
{
    int32_t margin_row0 = compose_safe_margin_x(FF_COMPOSE_ROW0_Y, FF_COMPOSE_KEY_H);
    int32_t margin_row1 = compose_safe_margin_x(FF_COMPOSE_ROW1_Y, FF_COMPOSE_KEY_H);
    int32_t margin_row2 = compose_safe_margin_x(FF_COMPOSE_ROW2_Y, FF_COMPOSE_KEY_H);
    int32_t margin_bottom = compose_safe_margin_x(FF_COMPOSE_BOTTOM_ROW_Y, FF_COMPOSE_BOTTOM_ROW_H);

    compose_build_grid_row(container, mode, FF_COMPOSE_ROW0_Y - FF_COMPOSE_GRID_Y, 1, margin_row0);
    compose_build_grid_row(container, mode, FF_COMPOSE_ROW1_Y - FF_COMPOSE_GRID_Y, 4, margin_row1);
    compose_build_grid_row(container, mode, FF_COMPOSE_ROW2_Y - FF_COMPOSE_GRID_Y, 7, margin_row2);
    compose_build_bottom_row(container, mode, FF_COMPOSE_BOTTOM_ROW_Y - FF_COMPOSE_GRID_Y, margin_bottom);
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
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_CLICKABLE); /* base lv_obj defaults clickable; this one is a plain backdrop */

    /* --- Header: back (dead-end escape, ux-raver checklist item 6) / TO / mode chip. ---
     * Margin derived from the header row's own y band, per this file's
     * header comment — NOT the flat "16, 10" pixel offsets the original
     * (off-glass) version used. */
    int32_t header_margin = compose_safe_margin_x(FF_COMPOSE_HEADER_Y, FF_COMPOSE_HEADER_H);

    lv_obj_t *back = lv_button_create(puck);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, FF_THEME_MIN_HIT_PX, FF_THEME_MIN_HIT_PX);
    lv_obj_set_pos(back, header_margin, FF_COMPOSE_HEADER_Y);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(back, compose_back_cb, LV_EVENT_CLICKED, NULL);
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
    lv_obj_align(to_lbl, LV_ALIGN_TOP_MID, 0, FF_COMPOSE_HEADER_Y + 12);

    int32_t mode_chip_w = 64;
    lv_obj_t *mode_chip = lv_button_create(puck);
    lv_obj_remove_style_all(mode_chip);
    lv_obj_set_size(mode_chip, mode_chip_w, FF_THEME_MIN_HIT_PX);
    lv_obj_set_pos(mode_chip, FF_THEME_PUCK_PX - header_margin - mode_chip_w, FF_COMPOSE_HEADER_Y);
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
    int32_t bubble_margin = compose_safe_margin_x(FF_COMPOSE_BUBBLE_Y, FF_COMPOSE_BUBBLE_H);
    lv_obj_t *bubble = lv_obj_create(puck);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_size(bubble, FF_THEME_PUCK_PX - 2 * bubble_margin, FF_COMPOSE_BUBBLE_H);
    lv_obj_set_pos(bubble, bubble_margin, FF_COMPOSE_BUBBLE_Y);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 14, 0);
    lv_obj_set_style_pad_all(bubble, 12, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_CLICKABLE); /* a display box, not a control */

    s_bubble_label = lv_label_create(bubble);
    lv_label_set_long_mode(s_bubble_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_bubble_label, FF_THEME_PUCK_PX - 2 * bubble_margin - 24);
    lv_obj_set_style_text_font(s_bubble_label, FF_THEME_FONT_LABEL, 0);
    lv_obj_align(s_bubble_label, LV_ALIGN_TOP_LEFT, 0, 0);
    compose_render_bubble_text(s_bubble_label, compose->text, compose->has_pending);

    /* --- Keypad. --- */
    s_keys_container = lv_obj_create(puck);
    lv_obj_remove_style_all(s_keys_container);
    lv_obj_set_size(s_keys_container, FF_THEME_PUCK_PX,
                     (FF_COMPOSE_BOTTOM_ROW_Y + FF_COMPOSE_BOTTOM_ROW_H) - FF_COMPOSE_GRID_Y);
    lv_obj_set_pos(s_keys_container, 0, FF_COMPOSE_GRID_Y);
    lv_obj_clear_flag(s_keys_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_keys_container, LV_OBJ_FLAG_CLICKABLE); /* a layout container, not a control itself */
    compose_build_keys(s_keys_container, s_mode);
}
