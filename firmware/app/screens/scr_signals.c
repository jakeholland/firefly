/**
 * scr_signals.c — see scr_signals.h.
 *
 * ## Round-glass layout (PR #25 UX review follow-up)
 * The first pass of this file placed the "+" button and the OMW/5 MIN/
 * PULSE reply row with flat pixel offsets against the puck's SQUARE
 * corners/edges — `targets/sim/tests/test_face_hit_targets.c`'s sweep
 * (added for this same review) caught the "+" button, the feed list, the
 * RALLY row, and the outer two reply buttons all sitting partly off the
 * round glass. Every position below is now derived from
 * `ff_layout_safe_margin_x` (app/screens/ff_layout.h — the same shared
 * helper `scr_compose.c` uses for its own round-glass fix), computed
 * from each block's own worst-case (farthest-from-center) y, not
 * eyeballed against the square bounding box. See that test file for the
 * assertion this is checked against on every build.
 *
 * A second, unrelated class of bug the same sweep surfaced: several
 * purely decorative containers (the "+" button aside, which IS meant to
 * be a control) were left CLICKABLE by omission — `lv_obj_create`
 * defaults to clickable in LVGL, and this file cleared that flag on every
 * icon-badge primitive but missed the feed-item rows themselves, the
 * scrollable list container, and the reply-row container. Fixed by
 * explicitly clearing LV_OBJ_FLAG_CLICKABLE on every container that
 * isn't meant to be a tap target, rather than relying on "nothing calls
 * add_flag on it so it must not be clickable" (false — the default is
 * the opposite).
 */
#include "scr_signals.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ff_intent.h" /* S16c1 — the emit seam; see signals_open_compose_cb */
#include "ff_layout.h"
#include "ff_theme.h"

/* ---------------------------------------------------------------------
 * Layout constants — local to this file (no cross-file geometry-testing
 * need like radar_layout.h's collision-tested numbers, so these don't
 * warrant their own header/module — the geometry MATH they're checked
 * against, ff_layout.h, is shared; these specific Y/height choices are
 * this face's own design, same split as scr_compose.c).
 * ------------------------------------------------------------------- */

#define FF_SIGNALS_SAFETY_PX 10.0f /* see scr_compose.c's FF_COMPOSE_SAFETY_PX — same rationale */

#define FF_SIGNALS_HEADER_Y 56
#define FF_SIGNALS_HEADER_H FF_THEME_MIN_HIT_PX /* the "+" button's row */

#define FF_SIGNALS_LIST_TOP_Y 108 /* header + gap */
#define FF_SIGNALS_LIST_H     192
#define FF_SIGNALS_LIST_BOT_Y (FF_SIGNALS_LIST_TOP_Y + FF_SIGNALS_LIST_H)

#define FF_SIGNALS_TARGET_LABEL_Y 306
#define FF_SIGNALS_TARGET_LABEL_H 18

#define FF_SIGNALS_REPLY_ROW_Y 332 /* well clear of scr_nav.c's shared page-dot chrome,
                                     * which sits at puck-center-relative dy=186 ->
                                     * absolute y~406-411 at this puck size */
#define FF_SIGNALS_REPLY_ROW_H FF_THEME_MIN_HIT_PX

#define FF_SIGNALS_ROW_H       58
#define FF_SIGNALS_ROW_GAP     6
#define FF_SIGNALS_ICON_PX     30

/**
 * signals_safe_margin_x — thin int32_t/ceil wrapper around
 * ff_layout_safe_margin_x, bound to this puck's own center/radius
 * (ff_theme.h) and this file's safety buffer — see scr_compose.c's
 * `compose_safe_margin_x` (identical shape, deliberately: both wrap the
 * same shared primitive around the same puck geometry).
 */
static int32_t signals_safe_margin_x(int32_t top_y, int32_t h)
{
    float margin = ff_layout_safe_margin_x((float)top_y, (float)h, (float)FF_THEME_PUCK_RADIUS_PX,
                                            (float)FF_THEME_PUCK_RADIUS_PX, FF_SIGNALS_SAFETY_PX);
    return (int32_t)ceilf(margin);
}

/* ---------------------------------------------------------------------
 * Header: "SIGNALS" title + "+" (open Compose) button.
 * ------------------------------------------------------------------- */

/* "+" -> emits FF_INTENT_OPEN_COMPOSE through the intent seam (S16
 * slice c1 — this replaces the issue-#23 stub). node_id = 0: NO explicit
 * destination. This screen is a pure renderer and cannot know the crew
 * selection, and S08's Behavior section says the composer's TO is the
 * SELECTED CREW MEMBER — so resolving it is the shell's job
 * (`shell_compose_dest` in ff_shell.c: selection if any, else
 * broadcast). Deliberately NOT the newest feed item: that context rule
 * belongs to the canned-reply chips (`ff_wiring_send_canned_reply`),
 * and S08 gives the composer its own, different rule. Unbound
 * (goldens/headless), the emit is a no-op. */
static void signals_open_compose_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_OPEN_COMPOSE, .u = {0}};
    in.u.node_id = 0u;
    ff_intent_emit(&in);
}

static void signals_build_header(lv_obj_t *parent)
{
    int32_t margin = signals_safe_margin_x(FF_SIGNALS_HEADER_Y, FF_SIGNALS_HEADER_H);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "SIGNALS");
    lv_obj_set_style_text_font(title, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_set_pos(title, margin, FF_SIGNALS_HEADER_Y + 12);

    /* "+" open-Compose button — FF_THEME_MIN_HIT_PX square, clears the
     * ux-raver fat-thumb floor with room to spare, and (per the margin
     * above) sits entirely on the round glass rather than the square
     * corner the first pass placed it against. */
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, FF_THEME_MIN_HIT_PX, FF_THEME_MIN_HIT_PX);
    lv_obj_set_pos(btn, FF_THEME_PUCK_PX - margin - FF_THEME_MIN_HIT_PX, FF_SIGNALS_HEADER_Y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(btn, signals_open_compose_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *plus = lv_label_create(btn);
    lv_label_set_text(plus, "+");
    lv_obj_set_style_text_font(plus, FF_THEME_FONT_NAME, 0);
    lv_obj_set_style_text_color(plus, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_center(plus);
}

/* ---------------------------------------------------------------------
 * Per-kind icon badge: a colored circle with a small pictograph inside,
 * built from plain LVGL primitives only (no custom draw callback, unlike
 * scr_radar.c's arrowhead triangles) — every shape below is either a
 * circle (lv_obj + LV_RADIUS_CIRCLE) or a plain rectangle, so there's no
 * static point-storage pool / re-render hazard to inherit from that file
 * (see scr_radar.c's top comment for what that hazard is and why it
 * doesn't apply here).
 * ------------------------------------------------------------------- */

static lv_obj_t *signals_icon_badge(lv_obj_t *parent, uint32_t bg_hex)
{
    lv_obj_t *badge = lv_obj_create(parent);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, FF_SIGNALS_ICON_PX, FF_SIGNALS_ICON_PX);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    return badge;
}

/* PULSE — concentric rings ("thinking of you" ping), literally the
 * mockup's "pulse rings" description: an outer ring outline plus a small
 * filled inner dot, both centered on the badge. */
static void signals_icon_pulse(lv_obj_t *parent)
{
    lv_obj_t *badge = signals_icon_badge(parent, FF_THEME_COLOR_SURFACE);

    lv_obj_t *ring = lv_obj_create(badge);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, FF_SIGNALS_ICON_PX - 6, FF_SIGNALS_ICON_PX - 6);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(ring);

    lv_obj_t *dot = lv_obj_create(badge);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(dot);
}

/* RALLY — a map-pin silhouette: a filled "head" dot above a thin stem. */
static void signals_icon_rally(lv_obj_t *parent)
{
    lv_obj_t *badge = signals_icon_badge(parent, FF_THEME_COLOR_SURFACE);

    lv_obj_t *head = lv_obj_create(badge);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, 12, 12);
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(head, lv_color_hex(FF_THEME_CREW_VIOLET), 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 3);

    lv_obj_t *stem = lv_obj_create(badge);
    lv_obj_remove_style_all(stem);
    lv_obj_set_size(stem, 3, 10);
    lv_obj_set_style_bg_color(stem, lv_color_hex(FF_THEME_CREW_VIOLET), 0);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    lv_obj_clear_flag(stem, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(stem, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(stem, LV_ALIGN_BOTTOM_MID, 0, -3);
}

/* TEXT — a rounded-rect message bubble. */
static void signals_icon_text(lv_obj_t *parent)
{
    lv_obj_t *badge = signals_icon_badge(parent, FF_THEME_COLOR_SURFACE);

    lv_obj_t *bubble = lv_obj_create(badge);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_size(bubble, 18, 13);
    lv_obj_set_style_radius(bubble, 4, 0);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(FF_THEME_CREW_TEAL), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(bubble);
}

/* STATUS — a plain solid dot (a status update, nothing fancier implied). */
static void signals_icon_status(lv_obj_t *parent)
{
    lv_obj_t *badge = signals_icon_badge(parent, FF_THEME_COLOR_SURFACE);

    lv_obj_t *dot = lv_obj_create(badge);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(FF_THEME_COLOR_LIVE_GREEN), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(dot);
}

/* FLARE — a small "+" spark/burst, alert-colored (same alert amber as
 * low-battery/mesh-loss chrome, ff_theme.h's FF_THEME_COLOR_STALE_AMBER). */
static void signals_icon_flare(lv_obj_t *parent)
{
    lv_obj_t *badge = signals_icon_badge(parent, FF_THEME_COLOR_SURFACE);

    lv_obj_t *bar_h = lv_obj_create(badge);
    lv_obj_remove_style_all(bar_h);
    lv_obj_set_size(bar_h, 16, 3);
    lv_obj_set_style_bg_color(bar_h, lv_color_hex(FF_THEME_COLOR_STALE_AMBER), 0);
    lv_obj_set_style_bg_opa(bar_h, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar_h, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bar_h, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(bar_h);

    lv_obj_t *bar_v = lv_obj_create(badge);
    lv_obj_remove_style_all(bar_v);
    lv_obj_set_size(bar_v, 3, 16);
    lv_obj_set_style_bg_color(bar_v, lv_color_hex(FF_THEME_COLOR_STALE_AMBER), 0);
    lv_obj_set_style_bg_opa(bar_v, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar_v, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bar_v, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(bar_v);
}

static void signals_build_icon(lv_obj_t *parent, ff_app_feed_kind_t kind)
{
    switch (kind) {
    case FF_APP_FEED_PULSE: signals_icon_pulse(parent); break;
    case FF_APP_FEED_RALLY: signals_icon_rally(parent); break;
    case FF_APP_FEED_TEXT: signals_icon_text(parent); break;
    case FF_APP_FEED_STATUS: signals_icon_status(parent); break;
    case FF_APP_FEED_FLARE: signals_icon_flare(parent); break;
    default: signals_icon_status(parent); break;
    }
}

/* ---------------------------------------------------------------------
 * Event rows.
 * ------------------------------------------------------------------- */

/* Rally row tap -> emits FF_INTENT_SELECT_RALLY through the intent seam
 * (S16 slice c2 — this replaces the issue-#23 stub). `rally_idx` is this
 * row's index into the `ff_app_signals_t` slice this screen was handed
 * (the row's own position in the visible list, passed through
 * `user_data` the same way `compose_key_click_cb` threads a key index) —
 * not a crew/core handle, which a pure-render screen must not hold
 * (CLAUDE.md). The shell still has nothing to DO with this yet:
 * `ff_crew_select_rally()` is not implemented in core (deferred — see
 * core/include/ff_crew.h's own documented deviation note: "S08's own
 * wording puts the combined crew+landmark selection cursor at S06...
 * Deferred to S06/S08"), so `ff_shell_intent`'s FF_INTENT_SELECT_RALLY
 * case stays a no-op until that lands — wiring the emit site now means
 * the eventual handler is the only piece still missing. Unbound
 * (goldens/headless), the emit is a no-op. */
static void signals_rally_tap_cb(lv_event_t *e)
{
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    ff_intent_t in = {.kind = FF_INTENT_SELECT_RALLY, .u = {0}};
    in.u.rally_idx = (uint8_t)idx;
    ff_intent_emit(&in);
}

static void signals_build_row(lv_obj_t *parent, ff_app_feed_item_t const *it, int32_t y, int32_t margin_x,
                               uint8_t idx)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, FF_THEME_PUCK_PX - 2 * margin_x, FF_SIGNALS_ROW_H);
    lv_obj_set_pos(row, margin_x, y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    /* lv_obj_create defaults CLICKABLE — explicitly clear it first so
     * every kind starts non-interactive, then opt RALLY back in below.
     * A prior pass relied on "nothing add_flag's it" to mean
     * non-clickable, which is backwards (see this file's header
     * comment) and is exactly why test_face_hit_targets.c flagged every
     * non-RALLY row as an unchecked (and off-glass) hit-rect. */
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    if (it->kind == FF_APP_FEED_RALLY) {
        /* Only RALLY rows are documented as tappable (S08 spec: "Rally row
         * tap -> sets rally as radar/map target") — everything else is
         * informational, so leaving them non-clickable avoids a "what does
         * tapping this do?" dead end (ux-raver checklist item 6). Height
         * already clears FF_THEME_MIN_HIT_PX (58 > 44); `margin_x` (the
         * list's own, computed against its worst-case row slot — see
         * ff_scr_signals_build) keeps the whole row's width on-glass
         * regardless of which slot a RALLY item happens to scroll into. */
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, signals_rally_tap_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);
    }

    lv_obj_t *icon_parent = lv_obj_create(row);
    lv_obj_remove_style_all(icon_parent);
    lv_obj_set_size(icon_parent, FF_SIGNALS_ICON_PX, FF_SIGNALS_ICON_PX);
    lv_obj_clear_flag(icon_parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(icon_parent, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(icon_parent, LV_ALIGN_LEFT_MID, 0, 0);
    signals_build_icon(icon_parent, it->kind);

    int32_t text_x = FF_SIGNALS_ICON_PX + 12;

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, (it->from_name[0] != '\0') ? it->from_name : "(unknown)");
    lv_obj_set_style_text_font(name, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_set_pos(name, text_x, 4);

    lv_obj_t *text = lv_label_create(row);
    lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(text, FF_THEME_PUCK_PX - 2 * margin_x - text_x - 56);
    lv_label_set_text(text, (it->text[0] != '\0') ? it->text : "-");
    lv_obj_set_style_text_font(text, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(text, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_set_pos(text, text_x, 26);

    lv_obj_t *age = lv_label_create(row);
    lv_label_set_text(age, (it->age_str[0] != '\0') ? it->age_str : "--");
    lv_obj_set_style_text_font(age, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(age, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_align(age, LV_ALIGN_RIGHT_MID, 0, -12);

    if (it->unread) {
        /* Unread dot: small amber dot above the age string — honesty read
         * (ux-raver checklist item 4): unread state must be visible at a
         * glance without reading any text. */
        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(dot, LV_ALIGN_RIGHT_MID, 0, 10);
    }
}

static void signals_build_empty_state(lv_obj_t *parent)
{
    /* Honest empty state (CLAUDE.md: "honest data over pretty data") —
     * nothing has happened yet, said plainly, not a fake sample row. */
    lv_obj_t *headline = lv_label_create(parent);
    lv_label_set_text(headline, "NO SIGNALS YET");
    lv_obj_set_style_text_font(headline, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(headline, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_align(headline, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *sub = lv_label_create(parent);
    lv_label_set_text(sub, "Pulses, rallies, and texts show up here");
    lv_obj_set_style_text_font(sub, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_set_width(sub, 260);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 4);
}

/* ---------------------------------------------------------------------
 * Canned-reply chip row: OMW / 5 MIN / PULSE.
 * ------------------------------------------------------------------- */

/* OMW / 5 MIN / PULSE -> emits FF_INTENT_CANNED_REPLY through the intent
 * seam (S16 slice c2 — this replaces the issue-#23 stub). `which` (the
 * chip's own ff_wiring_canned_reply_t) travels through `user_data`, same
 * pattern as compose_key_click_cb's key index and signals_rally_tap_cb's
 * row index. Reply-context (which sender OMW/5 MIN/PULSE address) is the
 * shell's call, not this screen's: `ff_wiring_send_canned_reply`'s
 * documented contract is the newest feed item, and this screen cannot
 * resolve that itself (pure render, per CLAUDE.md) — it only reports
 * which chip was pressed. See signals_build_target_label below for the
 * ON-SCREEN half of the same fact — PR #25 UX review (non-blocking
 * finding): a code comment saying "it's items[0]" is invisible to Bailey;
 * the label makes it visible. Unbound (goldens/headless), the emit is a
 * no-op. */
static void signals_canned_reply_cb(lv_event_t *e)
{
    uintptr_t which = (uintptr_t)lv_event_get_user_data(e);
    ff_intent_t in = {.kind = FF_INTENT_CANNED_REPLY, .u = {0}};
    in.u.reply = (ff_wiring_canned_reply_t)which;
    ff_intent_emit(&in);
}

static lv_obj_t *signals_make_reply_button(lv_obj_t *parent, char const *text, int32_t x, int32_t w,
                                            ff_wiring_canned_reply_t which)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, FF_SIGNALS_REPLY_ROW_H);
    lv_obj_set_pos(btn, x, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(btn, signals_canned_reply_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)which);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_center(label);

    return btn;
}

/* PR #25 UX review (non-blocking finding): "nothing on screen says which
 * row a canned reply targets... at 2am I'd genuinely wonder 'did OMW
 * just go to Dana, or to everyone?'". Renders "-> <name>" (the newest
 * feed item's sender, matching the reply-context this row will actually
 * use once wired — see signals_canned_reply_stub_cb's comment) or
 * "-> EVERYONE" when there's no feed item to reply to (broadcast, per
 * S08's "...else broadcast"). */
static void signals_build_target_label(lv_obj_t *parent, ff_app_signals_t const *signals, int32_t margin_x)
{
    char buf[FF_APP_NAME_LEN + 8];
    if (signals->n_items > 0 && signals->items[0].from_name[0] != '\0') {
        snprintf(buf, sizeof(buf), "-> %s", signals->items[0].from_name);
    } else {
        snprintf(buf, sizeof(buf), "-> EVERYONE");
    }

    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, buf);
    lv_obj_set_style_text_font(label, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_set_pos(label, margin_x, FF_SIGNALS_TARGET_LABEL_Y);
}

static void signals_build_reply_row(lv_obj_t *parent)
{
    int32_t margin_x = signals_safe_margin_x(FF_SIGNALS_REPLY_ROW_Y, FF_SIGNALS_REPLY_ROW_H);
    int32_t row_w = FF_THEME_PUCK_PX - 2 * margin_x;

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, row_w, FF_SIGNALS_REPLY_ROW_H);
    lv_obj_set_pos(row, margin_x, FF_SIGNALS_REPLY_ROW_Y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE); /* a layout container, not a control itself */

    int32_t gap = 8;
    int32_t btn_w = (row_w - 2 * gap) / 3;

    signals_make_reply_button(row, "OMW", 0, btn_w, FF_WIRING_REPLY_OMW);
    signals_make_reply_button(row, "5 MIN", btn_w + gap, btn_w, FF_WIRING_REPLY_5MIN);
    signals_make_reply_button(row, "PULSE", 2 * (btn_w + gap), btn_w, FF_WIRING_REPLY_PULSE);
}

/* ---------------------------------------------------------------------
 * Entry point.
 * ------------------------------------------------------------------- */

void ff_scr_signals_build(lv_obj_t *parent, ff_app_signals_t const *signals)
{
    if (parent == NULL || signals == NULL) {
        return;
    }

    signals_build_header(parent);

    if (signals->n_items == 0) {
        signals_build_empty_state(parent);
    } else {
        /* Scrollable list container so more items than fit on screen are
         * reachable (S08's fixture cap is 8; the row math above fits
         * about 3 before needing to scroll) without ever clipping/hiding
         * an item outright — CLAUDE.md's honesty rule applies to
         * visibility too, not just values.
         *
         * The list container itself is NOT clickable (a scroll viewport,
         * not a control — lv_obj_create defaults clickable, cleared
         * explicitly below per this file's header comment); it stays
         * full puck width so scrolling isn't accidentally narrowed, and
         * each ROW is individually positioned at `list_margin` — the
         * list's own chord-derived margin, computed once against its
         * worst-case (farthest-from-center) edge so it's correct
         * regardless of which item scrolls into which visible slot. */
        int32_t list_margin = signals_safe_margin_x(FF_SIGNALS_LIST_TOP_Y, FF_SIGNALS_LIST_H);

        lv_obj_t *list = lv_obj_create(parent);
        lv_obj_remove_style_all(list);
        lv_obj_set_pos(list, 0, FF_SIGNALS_LIST_TOP_Y);
        lv_obj_set_size(list, FF_THEME_PUCK_PX, FF_SIGNALS_LIST_H);
        lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(list, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

        int32_t y = 0;
        for (uint8_t i = 0; i < signals->n_items && i < FF_APP_SIGNALS_MAX_ITEMS; i++) {
            signals_build_row(list, &signals->items[i], y, list_margin, i);
            y += FF_SIGNALS_ROW_H + FF_SIGNALS_ROW_GAP;
        }
    }

    int32_t target_margin = signals_safe_margin_x(FF_SIGNALS_TARGET_LABEL_Y, FF_SIGNALS_TARGET_LABEL_H);
    signals_build_target_label(parent, signals, target_margin);
    signals_build_reply_row(parent);
}
