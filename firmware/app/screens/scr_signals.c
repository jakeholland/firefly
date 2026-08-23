/**
 * scr_signals.c — see scr_signals.h.
 */
#include "scr_signals.h"

#include <stdio.h>

#include "ff_theme.h"

/* ---------------------------------------------------------------------
 * Layout constants — local to this file (no cross-file geometry-testing
 * need like radar_layout.h's collision-tested numbers, so these don't
 * warrant their own header/module).
 * ------------------------------------------------------------------- */

#define FF_SIGNALS_MARGIN_X    18
#define FF_SIGNALS_LIST_TOP_Y  44  /* below the header row */
#define FF_SIGNALS_LIST_BOT_Y  340 /* above the canned-reply chip row (and the shared
                                     * page-dot chrome scr_nav.c draws at puck-center-
                                     * relative y=186, i.e. absolute y~406 at this puck
                                     * size — see signals_build_reply_row's own comment) */
#define FF_SIGNALS_ROW_H       58
#define FF_SIGNALS_ROW_GAP     6
#define FF_SIGNALS_ICON_PX     30

/* ---------------------------------------------------------------------
 * Header: "SIGNALS" title + "+" (open Compose) button.
 * ------------------------------------------------------------------- */

/* TODO(issue #23): open the real Compose face once face-switching
 * exists in the app's main loop (this screen only renders — which face is
 * active lives in ff_app_state_t, owned by whatever drives the real
 * event loop, not by a pure-render screen file per CLAUDE.md). Reserved
 * hook, same pattern as scr_radar.c's radar_flare_stub_cb. */
static void signals_open_compose_stub_cb(lv_event_t *e)
{
    (void)e;
}

static void signals_build_header(lv_obj_t *parent)
{
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "SIGNALS");
    lv_obj_set_style_text_font(title, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, FF_SIGNALS_MARGIN_X, 16);

    /* "+" open-Compose button — FF_THEME_MIN_HIT_PX square, clears the
     * ux-raver fat-thumb floor with room to spare even at a corner. */
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, FF_THEME_MIN_HIT_PX, FF_THEME_MIN_HIT_PX);
    lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -FF_SIGNALS_MARGIN_X + (FF_THEME_MIN_HIT_PX / 2), 8);
    lv_obj_add_event_cb(btn, signals_open_compose_stub_cb, LV_EVENT_CLICKED, NULL);

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

/* TODO(issue #23): tapping a RALLY row should call
 * ff_crew_select_rally() (deferred — see core/include/ff_crew.h's own
 * documented deviation note: "S08's own wording puts the combined
 * crew+landmark selection cursor at S06... Deferred to S06/S08"). This
 * screen has no core/crew handle to call it with (pure render, per
 * CLAUDE.md), so the hook is reserved here rather than guessed at. */
static void signals_rally_tap_stub_cb(lv_event_t *e)
{
    (void)e;
}

static void signals_build_row(lv_obj_t *parent, ff_app_feed_item_t const *it, int32_t y)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, FF_THEME_PUCK_PX - 2 * FF_SIGNALS_MARGIN_X, FF_SIGNALS_ROW_H);
    lv_obj_set_pos(row, FF_SIGNALS_MARGIN_X, y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    if (it->kind == FF_APP_FEED_RALLY) {
        /* Only RALLY rows are documented as tappable (S08 spec: "Rally row
         * tap -> sets rally as radar/map target") — everything else is
         * informational, so leaving them non-clickable avoids a "what does
         * tapping this do?" dead end (ux-raver checklist item 6). Height
         * already clears FF_THEME_MIN_HIT_PX (58 > 44). */
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, signals_rally_tap_stub_cb, LV_EVENT_CLICKED, NULL);
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
    lv_obj_set_width(text, FF_THEME_PUCK_PX - 2 * FF_SIGNALS_MARGIN_X - text_x - 56);
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

/* TODO(issue #23): actually send via app/ff_wiring.c's
 * ff_wiring_send_canned_reply() once this screen has a wiring handle to
 * call it through (pure render today, per CLAUDE.md — see this file's
 * header comment). Reply-context (which sender OMW/5 MIN/PULSE address)
 * is the newest feed item, per S08's "to the feed item's sender if a
 * reply-context exists" — that selection already happens correctly by
 * construction once this is wired (the caller passes the same
 * `ff_app_signals_t` this screen renders, whose `items[0]` is newest,
 * matching `ff_feed_at(feed, 0)` on the live core side). */
static void signals_canned_reply_stub_cb(lv_event_t *e)
{
    (void)e;
}

static lv_obj_t *signals_make_reply_button(lv_obj_t *parent, char const *text, int32_t x, int32_t w)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, FF_THEME_MIN_HIT_PX);
    lv_obj_set_pos(btn, x, -1); /* y set by caller via lv_obj_align on the row container instead */
    lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(btn, signals_canned_reply_stub_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_center(label);

    return btn;
}

static void signals_build_reply_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    int32_t row_w = FF_THEME_PUCK_PX - 2 * FF_SIGNALS_MARGIN_X;
    lv_obj_set_size(row, row_w, FF_THEME_MIN_HIT_PX);
    /* dy=-48, not the "eyeballed" -20 a first pass used: scr_nav.c draws
     * the shared page-dot row at puck-center-relative y=186 (absolute
     * y~406 at FF_THEME_PUCK_PX=440) on TOP of every tile's content, and
     * -20 put this row's bottom edge at y=420 — squarely on top of the
     * dots (caught visually, not by any golden diff, since a single bad
     * golden generation would've just "become" the new golden; re-check
     * any future page-dot geometry change against this row's bottom edge
     * too). -48 keeps the bottom edge at y=392, a clear ~14px above the
     * dots' own 401px top edge. */
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    int32_t gap = 8;
    int32_t btn_w = (row_w - 2 * gap) / 3;

    signals_make_reply_button(row, "OMW", 0, btn_w);
    signals_make_reply_button(row, "5 MIN", btn_w + gap, btn_w);
    signals_make_reply_button(row, "PULSE", 2 * (btn_w + gap), btn_w);
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
         * about 5 before needing to scroll) without ever clipping/hiding
         * an item outright — CLAUDE.md's honesty rule applies to
         * visibility too, not just values. */
        lv_obj_t *list = lv_obj_create(parent);
        lv_obj_remove_style_all(list);
        lv_obj_set_pos(list, 0, FF_SIGNALS_LIST_TOP_Y);
        lv_obj_set_size(list, FF_THEME_PUCK_PX, FF_SIGNALS_LIST_BOT_Y - FF_SIGNALS_LIST_TOP_Y);
        lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

        int32_t y = 0;
        for (uint8_t i = 0; i < signals->n_items && i < FF_APP_SIGNALS_MAX_ITEMS; i++) {
            signals_build_row(list, &signals->items[i], y);
            y += FF_SIGNALS_ROW_H + FF_SIGNALS_ROW_GAP;
        }
    }

    signals_build_reply_row(parent);
}
