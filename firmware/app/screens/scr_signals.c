/**
 * scr_signals.c — see scr_signals.h. The reworked Signals face (S22 b):
 * a pure projection of the core view-model `ff_sigview_t`.
 *
 * ## Round-glass layout — chrome ABOVE a bottom-anchored scroll list
 * The header, the send-target line, and the three action buttons are all
 * PINNED, above one bottom-anchored vertically-scrolling list. That order
 * (chrome on top, scroll list last) is not the canvas mockup's — it drew
 * the target line + actions docked BELOW the list — but it is the only
 * order `targets/sim/tests/test_face_hit_targets.c` accepts once every
 * list row is a tap target, and it is the exact shape S21's Settings face
 * already uses for the same reason:
 *
 *   The sweep's adjacency pass reads each clickable's raw, scroll-0
 *   absolute rect (a vertical scroll shifts every row equally, so a
 *   pair's edge-to-edge gap is scroll-invariant and the raw rects encode
 *   it). A list taller than its viewport therefore leaves its overflow
 *   rows at raw y BELOW the viewport at scroll 0. If a clickable control
 *   were docked there (a footer), those overflow rows would sit right on
 *   top of it and trip the 8px adjacency floor — a real geometric fact,
 *   not a false positive the sweep should be taught to ignore. Putting the
 *   list last makes its overflow spill into the empty space by the bottom
 *   pole, next to nothing clickable (the nav page-dots are not tap
 *   targets), so it collides with nothing.
 *
 * Every band's horizontal inset is derived from its own worst-case
 * (farthest-from-center) y through `ff_layout_safe_margin_x`
 * (app/screens/ff_layout.h) — the same shared primitive scr_compose.c /
 * scr_settings.c use — never a flat offset against the square bounding
 * box (PR #25's shipped-once bug). The scroll list is checked against its
 * VIEWPORT by the S21 scroll-aware sweep, so its inset is taken at the
 * viewport's lower edge (nearest the pole), where the chord is narrowest.
 *
 * ## Clickable-by-omission (PR #86 lesson)
 * `lv_obj_create` defaults CLICKABLE, so every decorative container below
 * explicitly clears the flag, and only the genuine tap targets (selectable
 * rows, the target-line ✕, the three action buttons) keep it. The
 * relevant rows opt IN; nothing relies on "no one add_flag'd it".
 */
#include "scr_signals.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ff_crew.h"   /* ff_fmt_age — the one shared age formatter (S22 honesty note) */
#include "ff_intent.h" /* the emit seam */
#include "ff_layout.h"
#include "ff_theme.h"

/* ---------------------------------------------------------------------
 * Layout constants — local to this face (its own design; the geometry
 * MATH they feed, ff_layout.h, is the shared/tested part — same split as
 * scr_compose.c / scr_settings.c).
 * ------------------------------------------------------------------- */

#define FF_SIGNALS_SAFETY_PX 10.0f /* see scr_compose.c's FF_COMPOSE_SAFETY_PX — same rationale */

/* Header: "SIGNALS" title + an unread-count badge to its right. */
#define FF_SIGNALS_HEADER_Y 34
#define FF_SIGNALS_HEADER_H 30

/* Docked target line: "who does a send go to". */
#define FF_SIGNALS_TARGET_Y 78
#define FF_SIGNALS_TARGET_H FF_THEME_MIN_HIT_PX /* 44 — its ✕ must be a real hit target */

/* Docked action row: RALLY / PULSE / COMPOSE. */
#define FF_SIGNALS_ACTIONS_Y 132
#define FF_SIGNALS_ACTIONS_H 58
#define FF_SIGNALS_ACTIONS_GAP 12

/* Bottom-anchored scroll list. 200..356 mirrors scr_settings.c's viewport
 * lower edge (356) — where the chord is ~262px wide after the safety
 * inset, ample for a row — with its overflow spilling below into the empty
 * space by the bottom pole (the nav page-dots at center-dy 186 ≈ y392 are
 * not clickable, so nothing there is an adjacency partner). */
#define FF_SIGNALS_LIST_TOP_Y 200
#define FF_SIGNALS_LIST_H     156
#define FF_SIGNALS_LIST_BOT_Y (FF_SIGNALS_LIST_TOP_Y + FF_SIGNALS_LIST_H)

#define FF_SIGNALS_ROW_H   54 /* > FF_THEME_MIN_HIT_PX (44) */
#define FF_SIGNALS_ROW_GAP 10 /* > FF_HIT_MIN_GAP_PX (8) */
_Static_assert(FF_SIGNALS_ROW_H >= FF_THEME_MIN_HIT_PX, "signals row must clear the hit floor");
_Static_assert(FF_SIGNALS_ROW_GAP >= FF_HIT_MIN_GAP_PX, "signals row gap must clear the adjacency floor");
#define FF_SIGNALS_DIVIDER_H 28 /* non-clickable separator */
#define FF_SIGNALS_ICON_PX   30
#define FF_SIGNALS_DOT_PX    14

/* signals_safe_margin_x — int32/ceil wrapper around ff_layout_safe_margin_x,
 * bound to this puck's center/radius and this file's safety buffer (twin of
 * scr_compose.c's compose_safe_margin_x / scr_settings.c's own). */
static int32_t signals_safe_margin_x(int32_t top_y, int32_t h)
{
    float margin = ff_layout_safe_margin_x((float)top_y, (float)h, (float)FF_THEME_PUCK_RADIUS_PX,
                                            (float)FF_THEME_PUCK_RADIUS_PX, FF_SIGNALS_SAFETY_PX);
    return (int32_t)ceilf(margin);
}

/* ---------------------------------------------------------------------
 * Intent emitters (the whole seam between this screen and the shell).
 * ------------------------------------------------------------------- */

/* A selectable row -> FF_INTENT_SIG_SELECT_MEMBER carrying the row's crew
 * node id (threaded through user_data, the same pattern compose_key_click_cb
 * uses for a key index). The shell validates it against the roster before it
 * becomes the target (a pure-render screen must not decide targeting). */
static void signals_select_member_cb(lv_event_t *e)
{
    uintptr_t node = (uintptr_t)lv_event_get_user_data(e);
    ff_intent_t in = {.kind = FF_INTENT_SIG_SELECT_MEMBER, .u = {0}};
    in.u.node_id = (uint32_t)node;
    ff_intent_emit(&in);
}

/* Target-line ✕ -> FF_INTENT_SIG_CLEAR_TARGET (back to WHOLE CREW). */
static void signals_clear_target_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_SIG_CLEAR_TARGET, .u = {0}};
    ff_intent_emit(&in);
}

/* The three action buttons -> their intents. The action kind travels
 * through user_data as an ff_intent_kind_t (each of RALLY/PULSE/COMPOSE is
 * payload-free — it acts on the shell's current target). */
static void signals_action_cb(lv_event_t *e)
{
    uintptr_t kind = (uintptr_t)lv_event_get_user_data(e);
    ff_intent_t in = {.kind = (ff_intent_kind_t)kind, .u = {0}};
    ff_intent_emit(&in);
}

/* ---------------------------------------------------------------------
 * Per-kind icon badge — a colored circle with a small pictograph inside,
 * built from plain LVGL primitives (circles + rects), no custom draw
 * callback (so no static point-storage / re-render hazard — see
 * scr_radar.c's top comment for the hazard that does NOT apply here).
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

static void signals_child_deco(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

/* PULSE — concentric rings ("thinking of you" ping): outer ring outline +
 * a small filled inner dot. */
static void signals_icon_pulse(lv_obj_t *parent)
{
    lv_obj_t *badge = signals_icon_badge(parent, FF_THEME_COLOR_SURFACE);

    lv_obj_t *ring = lv_obj_create(badge);
    signals_child_deco(ring);
    lv_obj_set_size(ring, FF_SIGNALS_ICON_PX - 6, FF_SIGNALS_ICON_PX - 6);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_center(ring);

    lv_obj_t *dot = lv_obj_create(badge);
    signals_child_deco(dot);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_center(dot);
}

/* RALLY — a small flag: a thin pole with a filled pennant near its top
 * (the mockup's "rally flag"). */
static void signals_icon_rally(lv_obj_t *parent)
{
    lv_obj_t *badge = signals_icon_badge(parent, FF_THEME_COLOR_SURFACE);

    lv_obj_t *pole = lv_obj_create(badge);
    signals_child_deco(pole);
    lv_obj_set_size(pole, 3, 18);
    lv_obj_set_style_bg_color(pole, lv_color_hex(FF_THEME_CREW_VIOLET), 0);
    lv_obj_set_style_bg_opa(pole, LV_OPA_COVER, 0);
    lv_obj_align(pole, LV_ALIGN_CENTER, -5, 0);

    lv_obj_t *flag = lv_obj_create(badge);
    signals_child_deco(flag);
    lv_obj_set_size(flag, 11, 8);
    lv_obj_set_style_radius(flag, 1, 0);
    lv_obj_set_style_bg_color(flag, lv_color_hex(FF_THEME_CREW_VIOLET), 0);
    lv_obj_set_style_bg_opa(flag, LV_OPA_COVER, 0);
    lv_obj_align(flag, LV_ALIGN_CENTER, 2, -5);
}

/* TEXT — a rounded-rect message bubble. */
static void signals_icon_text(lv_obj_t *parent)
{
    lv_obj_t *badge = signals_icon_badge(parent, FF_THEME_COLOR_SURFACE);

    lv_obj_t *bubble = lv_obj_create(badge);
    signals_child_deco(bubble);
    lv_obj_set_size(bubble, 18, 13);
    lv_obj_set_style_radius(bubble, 4, 0);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(FF_THEME_CREW_TEAL), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_center(bubble);
}

/* STATUS — a plain solid dot. */
static void signals_icon_status(lv_obj_t *parent)
{
    lv_obj_t *badge = signals_icon_badge(parent, FF_THEME_COLOR_SURFACE);

    lv_obj_t *dot = lv_obj_create(badge);
    signals_child_deco(dot);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(FF_THEME_COLOR_LIVE_GREEN), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_center(dot);
}

/* FLARE — a small "+" spark/burst, alert-colored. */
static void signals_icon_flare(lv_obj_t *parent)
{
    lv_obj_t *badge = signals_icon_badge(parent, FF_THEME_COLOR_SURFACE);

    lv_obj_t *bar_h = lv_obj_create(badge);
    signals_child_deco(bar_h);
    lv_obj_set_size(bar_h, 16, 3);
    lv_obj_set_style_bg_color(bar_h, lv_color_hex(FF_THEME_COLOR_STALE_AMBER), 0);
    lv_obj_set_style_bg_opa(bar_h, LV_OPA_COVER, 0);
    lv_obj_center(bar_h);

    lv_obj_t *bar_v = lv_obj_create(badge);
    signals_child_deco(bar_v);
    lv_obj_set_size(bar_v, 3, 16);
    lv_obj_set_style_bg_color(bar_v, lv_color_hex(FF_THEME_COLOR_STALE_AMBER), 0);
    lv_obj_set_style_bg_opa(bar_v, LV_OPA_COVER, 0);
    lv_obj_center(bar_v);
}

static void signals_build_icon(lv_obj_t *parent, ff_feed_kind_t kind)
{
    switch (kind) {
    case FEED_PULSE: signals_icon_pulse(parent); break;
    case FEED_RALLY: signals_icon_rally(parent); break;
    case FEED_TEXT: signals_icon_text(parent); break;
    case FEED_STATUS: signals_icon_status(parent); break;
    case FEED_FLARE: signals_icon_flare(parent); break;
    default: signals_icon_status(parent); break;
    }
}

static char const *signals_feed_kind_label(ff_feed_kind_t kind)
{
    switch (kind) {
    case FEED_PULSE: return "PULSE";
    case FEED_RALLY: return "RALLY";
    case FEED_TEXT: return "TEXT";
    case FEED_STATUS: return "STATUS";
    case FEED_FLARE: return "FLARE";
    default: return "SIGNAL";
    }
}

/* ---------------------------------------------------------------------
 * Rows.
 * ------------------------------------------------------------------- */

/* A row container of `h`, inset to `margin_x`, non-scrollable. `node_id`
 * != 0 makes it a SELECT tap target; 0 (an unknown-identity RECENT row)
 * leaves it inert — an explicitly-unknown sender is shown but not a
 * selectable recipient. */
static lv_obj_t *signals_row_container(lv_obj_t *parent, int32_t y, int32_t margin_x, int32_t h, uint32_t node_id)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, FF_THEME_PUCK_PX - 2 * margin_x, h);
    lv_obj_set_pos(row, margin_x, y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE); /* default is clickable — start inert */

    if (node_id != 0) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, signals_select_member_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)node_id);
    }
    return row;
}

/* RECENT row: icon badge · name · kind subtitle · mono age · (unread) an
 * amber accent bar on the left edge + an amber dot at the right. */
static void signals_build_recent_row(lv_obj_t *parent, ff_sigrow_t const *r, int32_t y, int32_t margin_x)
{
    lv_obj_t *row = signals_row_container(parent, y, margin_x, FF_SIGNALS_ROW_H, r->node_id);
    int32_t row_w = FF_THEME_PUCK_PX - 2 * margin_x;

    if (r->unread) {
        /* Amber accent bar down the left edge — an at-a-glance unread cue
         * that needs no reading (ux-raver checklist item 4). */
        lv_obj_t *bar = lv_obj_create(row);
        signals_child_deco(bar);
        lv_obj_set_size(bar, 3, FF_SIGNALS_ROW_H - 12);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_align(bar, LV_ALIGN_LEFT_MID, 0, 0);
    }

    lv_obj_t *icon_parent = lv_obj_create(row);
    signals_child_deco(icon_parent);
    lv_obj_set_size(icon_parent, FF_SIGNALS_ICON_PX, FF_SIGNALS_ICON_PX);
    lv_obj_align(icon_parent, LV_ALIGN_LEFT_MID, 10, 0);
    signals_build_icon(icon_parent, r->feed_kind);

    int32_t text_x = 10 + FF_SIGNALS_ICON_PX + 12;

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(name, row_w - text_x - 56);
    lv_label_set_text(name, (r->identity_known && r->name[0] != '\0') ? r->name : "UNKNOWN");
    lv_obj_set_style_text_font(name, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_set_pos(name, text_x, 8);

    lv_obj_t *sub = lv_label_create(row);
    lv_label_set_text(sub, signals_feed_kind_label(r->feed_kind));
    lv_obj_set_style_text_font(sub, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_set_pos(sub, text_x, 30);

    char age_buf[FF_APP_STR_SHORT];
    ff_fmt_age(age_buf, sizeof(age_buf), r->age_ms);
    lv_obj_t *age = lv_label_create(row);
    lv_label_set_text(age, age_buf);
    lv_obj_set_style_text_font(age, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(age, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_align(age, LV_ALIGN_RIGHT_MID, -4, r->unread ? -8 : 0);

    if (r->unread) {
        lv_obj_t *dot = lv_obj_create(row);
        signals_child_deco(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_RIGHT_MID, -4, 12);
    }
}

/* Presence label text + tint (S22 AC2) — the honest last-heard read.
 * SEEN -> "SEEN <age>"; LOST -> "LOST" (stale tint); LINKED -> "LINKED".
 * The age is formatted only for SEEN, via the one shared `ff_fmt_age`. */
static void signals_presence_text(ff_sigrow_t const *r, char *buf, size_t n, uint32_t *out_color)
{
    switch (r->presence) {
    case FF_PRESENCE_SEEN: {
        char age_buf[FF_APP_STR_SHORT];
        ff_fmt_age(age_buf, sizeof(age_buf), r->age_ms);
        snprintf(buf, n, "SEEN %s", age_buf);
        *out_color = FF_THEME_COLOR_MUTED;
        break;
    }
    case FF_PRESENCE_LOST:
        snprintf(buf, n, "LOST");
        *out_color = FF_THEME_COLOR_STALE_AMBER;
        break;
    case FF_PRESENCE_LINKED:
    default:
        snprintf(buf, n, "LINKED");
        *out_color = FF_THEME_COLOR_DIM;
        break;
    }
}

/* CREW_QUIET row: dimmed, a crew-color dot + name + honest presence label.
 * Always a paired member (identity_known) -> always selectable. */
static void signals_build_quiet_row(lv_obj_t *parent, ff_sigrow_t const *r, int32_t y, int32_t margin_x,
                                     bool colorblind)
{
    lv_obj_t *row = signals_row_container(parent, y, margin_x, FF_SIGNALS_ROW_H, r->node_id);
    int32_t row_w = FF_THEME_PUCK_PX - 2 * margin_x;
    lv_obj_set_style_opa(row, LV_OPA_70, 0); /* quiet crew read dimmer than recent */

    lv_obj_t *dot = lv_obj_create(row);
    signals_child_deco(dot);
    lv_obj_set_size(dot, FF_SIGNALS_DOT_PX, FF_SIGNALS_DOT_PX);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(ff_theme_crew_color(r->color_idx, colorblind)), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 10 + (FF_SIGNALS_ICON_PX - FF_SIGNALS_DOT_PX) / 2, 0);

    int32_t text_x = 10 + FF_SIGNALS_ICON_PX + 12;

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(name, row_w - text_x - 12);
    lv_label_set_text(name, (r->name[0] != '\0') ? r->name : "CREW");
    lv_obj_set_style_text_font(name, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_pos(name, text_x, 8);

    char pres_buf[FF_APP_STR_SHORT + 8];
    uint32_t pres_color = FF_THEME_COLOR_DIM;
    signals_presence_text(r, pres_buf, sizeof(pres_buf), &pres_color);
    lv_obj_t *pres = lv_label_create(row);
    lv_label_set_text(pres, pres_buf);
    lv_obj_set_style_text_font(pres, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(pres, lv_color_hex(pres_color), 0);
    lv_obj_set_pos(pres, text_x, 30);
}

/* The "· CREW ·" divider — a centered "CREW" caption flanked by two thin
 * rules (drawn with rects, not a middot glyph the vendored Montserrat
 * subset lacks — that rendered as tofu). Drawn only when quiet crew follow. */
static void signals_build_divider(lv_obj_t *parent, int32_t y, int32_t margin_x)
{
    int32_t row_w = FF_THEME_PUCK_PX - 2 * margin_x;

    lv_obj_t *row = lv_obj_create(parent);
    signals_child_deco(row);
    lv_obj_set_size(row, row_w, FF_SIGNALS_DIVIDER_H);
    lv_obj_set_pos(row, margin_x, y);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, "CREW");
    lv_obj_set_style_text_font(label, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_center(label);

    int32_t rule_w = (row_w - 60) / 2; /* leave ~60px in the middle for "CREW" */
    if (rule_w > 8) {
        for (int side = 0; side < 2; side++) {
            lv_obj_t *rule = lv_obj_create(row);
            signals_child_deco(rule);
            lv_obj_set_size(rule, rule_w, 1);
            lv_obj_set_style_bg_color(rule, lv_color_hex(FF_THEME_COLOR_DIM), 0);
            lv_obj_set_style_bg_opa(rule, LV_OPA_50, 0);
            lv_obj_align(rule, side == 0 ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }
}

static void signals_build_empty_state(lv_obj_t *parent)
{
    /* Honest empty state (CLAUDE.md) — nothing has happened yet, said
     * plainly, never a fake sample row. Placed in the list band. */
    lv_obj_t *headline = lv_label_create(parent);
    lv_label_set_text(headline, "NO SIGNALS YET");
    lv_obj_set_style_text_font(headline, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(headline, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_align(headline, LV_ALIGN_TOP_MID, 0, FF_SIGNALS_LIST_TOP_Y + 30);

    lv_obj_t *sub = lv_label_create(parent);
    lv_label_set_text(sub, "Pulses, rallies, and texts show up here");
    lv_obj_set_style_text_font(sub, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_set_width(sub, 260);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, FF_SIGNALS_LIST_TOP_Y + 64);
}

/* ---------------------------------------------------------------------
 * Header: "SIGNALS" + unread-count badge.
 * ------------------------------------------------------------------- */

static void signals_build_header(lv_obj_t *parent, uint16_t unread)
{
    int32_t margin = signals_safe_margin_x(FF_SIGNALS_HEADER_Y, FF_SIGNALS_HEADER_H);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "SIGNALS");
    lv_obj_set_style_text_font(title, FF_THEME_FONT_NAME, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_set_pos(title, margin, FF_SIGNALS_HEADER_Y);

    if (unread > 0) {
        /* Small amber pill just right of the title — the unread count. Not
         * a control (an indicator), so non-clickable. */
        lv_obj_t *badge = lv_obj_create(parent);
        signals_child_deco(badge);
        lv_obj_set_size(badge, 24, 20);
        lv_obj_set_style_radius(badge, 10, 0);
        lv_obj_set_style_bg_color(badge, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        /* Title width is font-dependent; anchor the badge a fixed step in
         * from the title's left rather than measuring the label. */
        lv_obj_set_pos(badge, margin + 120, FF_SIGNALS_HEADER_Y + 2);

        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)(unread > 99 ? 99 : unread));
        lv_obj_t *num = lv_label_create(badge);
        lv_label_set_text(num, buf);
        lv_obj_set_style_text_font(num, FF_THEME_FONT_CHIP, 0);
        lv_obj_set_style_text_color(num, lv_color_hex(0x14141C), 0); /* dark ink on amber */
        lv_obj_center(num);
    }
}

/* ---------------------------------------------------------------------
 * Target line: the always-visible "who a send goes to".
 * ------------------------------------------------------------------- */

/* Find the row carrying `node_id` so the target line can render that
 * member's name + crew color WITHOUT a second data source — a selected
 * member is paired, so it is always present as a RECENT or CREW_QUIET row.
 * Returns NULL only in the transient window before its row exists. */
static ff_sigrow_t const *signals_find_member_row(ff_sigview_t const *v, uint32_t node_id)
{
    uint16_t n = ff_sigview_row_count(v);
    for (uint16_t i = 0; i < n; i++) {
        ff_sigrow_t const *r = ff_sigview_row_at(v, i);
        if (r != NULL && r->identity_known && r->node_id == node_id) {
            return r;
        }
    }
    return NULL;
}

static void signals_build_target_line(lv_obj_t *parent, ff_sigview_t const *v, bool colorblind)
{
    int32_t margin = signals_safe_margin_x(FF_SIGNALS_TARGET_Y, FF_SIGNALS_TARGET_H);
    int32_t row_w = FF_THEME_PUCK_PX - 2 * margin;

    lv_obj_t *line = lv_obj_create(parent);
    signals_child_deco(line);
    lv_obj_set_size(line, row_w, FF_SIGNALS_TARGET_H);
    lv_obj_set_pos(line, margin, FF_SIGNALS_TARGET_Y);
    lv_obj_set_style_radius(line, FF_SIGNALS_TARGET_H / 2, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    /* "TO" caption so a send never fires blind (the target line's whole job). */
    lv_obj_t *cap = lv_label_create(line);
    lv_label_set_text(cap, "TO");
    lv_obj_set_style_text_font(cap, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_align(cap, LV_ALIGN_LEFT_MID, 16, 0);

    if (ff_sigview_target_kind(v) == FF_TARGET_WHOLE_CREW) {
        /* WHOLE CREW: a small cluster glyph (three overlapping dots) + label.
         * No ✕ — whole crew IS the cleared state. */
        lv_obj_t *cluster = lv_obj_create(line);
        signals_child_deco(cluster);
        lv_obj_set_size(cluster, 22, FF_SIGNALS_DOT_PX);
        lv_obj_set_style_bg_opa(cluster, LV_OPA_TRANSP, 0);
        lv_obj_align(cluster, LV_ALIGN_LEFT_MID, 48, 0);
        for (int i = 0; i < 3; i++) {
            lv_obj_t *d = lv_obj_create(cluster);
            signals_child_deco(d);
            lv_obj_set_size(d, 10, 10);
            lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(d, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
            lv_obj_align(d, LV_ALIGN_LEFT_MID, i * 6, 0);
        }

        lv_obj_t *name = lv_label_create(line);
        lv_label_set_text(name, "WHOLE CREW");
        lv_obj_set_style_text_font(name, FF_THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(FF_THEME_COLOR_INK), 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 80, 0);
        return;
    }

    /* A single member: crew-color dot + name + a clear (✕) affordance. */
    uint32_t                node = ff_sigview_target_node(v);
    ff_sigrow_t const *member = signals_find_member_row(v, node);

    lv_obj_t *dot = lv_obj_create(line);
    signals_child_deco(dot);
    lv_obj_set_size(dot, FF_SIGNALS_DOT_PX, FF_SIGNALS_DOT_PX);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(
        dot, lv_color_hex(ff_theme_crew_color(member != NULL ? member->color_idx : 0, colorblind)), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 48, 0);

    lv_obj_t *name = lv_label_create(line);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(name, row_w - 72 - FF_THEME_MIN_HIT_PX);
    lv_label_set_text(name, (member != NULL && member->name[0] != '\0') ? member->name : "MEMBER");
    lv_obj_set_style_text_font(name, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 72, 0);

    /* ✕ clear button — a real FF_THEME_MIN_HIT_PX hit target on the right. */
    lv_obj_t *clear = lv_button_create(line);
    lv_obj_remove_style_all(clear);
    lv_obj_set_size(clear, FF_THEME_MIN_HIT_PX, FF_THEME_MIN_HIT_PX);
    lv_obj_align(clear, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(clear, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(clear, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(clear, signals_clear_target_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *x = lv_label_create(clear);
    lv_label_set_text(x, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(x, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(x, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_center(x);
}

/* ---------------------------------------------------------------------
 * Action buttons: RALLY (violet) · PULSE (amber) · COMPOSE (green).
 * ------------------------------------------------------------------- */

/* `armed` (S22 slice d, AC4) draws the RALLY button in its armed
 * "tap again to send the loud broadcast" state: a bright ink ring plus the
 * "RALLY?" caption the caller passes. Only ever true for the RALLY button
 * on a WHOLE_CREW target; false renders the plain filled button. */
static void signals_make_action(lv_obj_t *parent, char const *text, uint32_t color_hex, int32_t x, int32_t w,
                                 ff_intent_kind_t intent, bool armed)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, FF_SIGNALS_ACTIONS_H);
    lv_obj_set_pos(btn, x, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color_hex), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    if (armed) {
        /* A bright ink ring around the fill — an unmistakable "armed, tap
         * again" cue that also answers the earlier review note that the
         * action buttons gave no feedback. */
        lv_obj_set_style_border_color(btn, lv_color_hex(FF_THEME_COLOR_INK), 0);
        lv_obj_set_style_border_width(btn, 3, 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    }
    lv_obj_add_event_cb(btn, signals_action_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)intent);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x14141C), 0); /* dark ink on the color fill */
    lv_obj_center(label);
}

static void signals_build_actions(lv_obj_t *parent, ff_sigview_t const *v)
{
    int32_t margin = signals_safe_margin_x(FF_SIGNALS_ACTIONS_Y, FF_SIGNALS_ACTIONS_H);
    int32_t row_w = FF_THEME_PUCK_PX - 2 * margin;

    lv_obj_t *row = lv_obj_create(parent);
    signals_child_deco(row);
    lv_obj_set_size(row, row_w, FF_SIGNALS_ACTIONS_H);
    lv_obj_set_pos(row, margin, FF_SIGNALS_ACTIONS_Y);

    bool const rally_armed = ff_sigview_rally_confirm_armed(v);
    int32_t btn_w = (row_w - 2 * FF_SIGNALS_ACTIONS_GAP) / 3;
    signals_make_action(row, rally_armed ? "RALLY?" : "RALLY", FF_THEME_CREW_VIOLET, 0, btn_w,
                        FF_INTENT_SIG_RALLY, rally_armed);
    signals_make_action(row, "PULSE", FF_THEME_COLOR_AMBER, btn_w + FF_SIGNALS_ACTIONS_GAP, btn_w,
                        FF_INTENT_SIG_PULSE, false);
    signals_make_action(row, "COMPOSE", FF_THEME_COLOR_LIVE_GREEN, 2 * (btn_w + FF_SIGNALS_ACTIONS_GAP), btn_w,
                        FF_INTENT_SIG_COMPOSE, false);
}

/* ---------------------------------------------------------------------
 * Public helpers + entry point.
 * ------------------------------------------------------------------- */

uint16_t ff_scr_signals_unread_count(ff_sigview_t const *v)
{
    uint16_t n = ff_sigview_row_count(v);
    uint16_t unread = 0;
    for (uint16_t i = 0; i < n; i++) {
        ff_sigrow_t const *r = ff_sigview_row_at(v, i);
        if (r != NULL && r->kind == FF_SIGROW_RECENT && r->unread) {
            unread++;
        }
    }
    return unread;
}

void ff_scr_signals_build(lv_obj_t *parent, ff_sigview_t const *v, bool colorblind)
{
    if (parent == NULL || v == NULL) {
        return;
    }

    signals_build_header(parent, ff_scr_signals_unread_count(v));
    signals_build_target_line(parent, v, colorblind);
    signals_build_actions(parent, v);

    uint16_t n = ff_sigview_row_count(v);

    /* Anything to list? (recent rows or quiet-crew rows — the divider alone
     * is not content.) */
    bool has_content = false;
    for (uint16_t i = 0; i < n; i++) {
        ff_sigrow_t const *r = ff_sigview_row_at(v, i);
        if (r != NULL && (r->kind == FF_SIGROW_RECENT || r->kind == FF_SIGROW_CREW_QUIET)) {
            has_content = true;
            break;
        }
    }
    if (!has_content) {
        signals_build_empty_state(parent);
        return;
    }

    /* Bottom-anchored scroll list — full puck width so the scroll gesture
     * isn't narrowed; each ROW is inset to the list's own chord-derived
     * margin (computed once against the viewport's lower edge, its
     * worst-case). Clips its own children, so overflow rows are reachable
     * by scroll and never render past the viewport. */
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
    for (uint16_t i = 0; i < n; i++) {
        ff_sigrow_t const *r = ff_sigview_row_at(v, i);
        if (r == NULL) {
            continue;
        }
        switch (r->kind) {
        case FF_SIGROW_RECENT:
            signals_build_recent_row(list, r, y, list_margin);
            y += FF_SIGNALS_ROW_H + FF_SIGNALS_ROW_GAP;
            break;
        case FF_SIGROW_CREW_QUIET:
            signals_build_quiet_row(list, r, y, list_margin, colorblind);
            y += FF_SIGNALS_ROW_H + FF_SIGNALS_ROW_GAP;
            break;
        case FF_SIGROW_DIVIDER:
            /* Draw the divider only when quiet crew actually follow it. */
            if (i + 1 < n) {
                signals_build_divider(list, y, list_margin);
                y += FF_SIGNALS_DIVIDER_H + FF_SIGNALS_ROW_GAP;
            }
            break;
        default:
            break;
        }
    }
}
