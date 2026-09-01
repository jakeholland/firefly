/**
 * scr_signals.c — see scr_signals.h. The S24 Signals INBOX + THREAD
 * screens (spec: docs/specs/S24-signals-inbox.md, slices b/c): a pure
 * projection of `ff_app_signals_t` — the sub-view selector, the core
 * `ff_inbox_t` conversation model, and the open thread's
 * `ff_inbox_thread_t` messages — replacing the S22 unified-list screen.
 *
 * ## Round-glass layout — chrome above a bottom-anchored scroll list
 * The (non-clickable) header sits above one vertically-scrolling
 * conversation list, the shape S21 settled and S22 kept: a list taller
 * than its viewport leaves its overflow rows at raw y below the viewport
 * at scroll 0, so nothing CLICKABLE may be docked beneath the list where
 * those raw rects land (`test_face_hit_targets.c`'s scroll-invariant
 * adjacency pass measures them there — a real geometric fact, not a
 * sweep quirk). The one clickable thing near the bottom is the FAB's tap
 * target, which sits to the RIGHT of every row's hit-rect (see
 * FF_SIGNALS_ROW_HIT_CLEAR_X below) precisely so overflow rows can never
 * collide with it at any scroll offset.
 *
 * ## Touching rows vs. the adjacency floor (design/sweep reconciliation)
 * The design canvas draws conversation rows FULL-BLEED and TOUCHING
 * (68px pitch, no gap). The sweep's 8px adjacency floor is about HIT
 * rects, not paint: each row's visual container is non-clickable and
 * touches its neighbors exactly as drawn, while the row's actual tap
 * target (a transparent overlay) is inset FF_SIGNALS_ROW_HIT_INSET_Y
 * (4px) top and bottom — adjacent tap targets therefore keep an 8px
 * dead band a thumb cannot straddle, and every target still measures
 * 60px tall, comfortably over the 44px floor. Same trade the spec's own
 * "44px escapes / 8px adjacency" interaction bar asks for.
 *
 * ## The corner FAB vs. the round glass
 * The `+` FAB is a big solid-amber disc BLEEDING off the bottom-right
 * rim (a corner slice). Its off-glass spill is masked back to the
 * letterbox black by a border-only RIM RING drawn over it (NOT by
 * `style_clip_corner`, which hangs this project's software renderer —
 * see signals_build_fab's section comment for the measured bisect), so
 * the sim golden shows exactly what the round panel shows. Its TAP
 * TARGET is a separate 48px transparent button placed fully on-glass
 * over the disc's inner edge — the sweep's circle-containment check
 * applies to the tap target, and the visible disc is deco.
 *
 * ## Clickable-by-omission (PR #86 lesson)
 * `lv_obj_create` defaults CLICKABLE, so every decorative container
 * below explicitly clears the flag; only the genuine tap targets (row
 * overlays, the FAB button, the picker rows, the back buttons) opt IN.
 *
 * ## Press-down feedback (S24 AC7)
 * Every tappable control gets an LV_STATE_PRESSED treatment mirroring
 * the composer's device-polish convention (`compose_key_press_feedback`,
 * the on-glass "presses must register" finding): an amber flash for
 * dark/transparent controls, a dim for the already-amber FAB.
 */
#include "scr_signals.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ff_crew.h"   /* ff_fmt_age — the one shared age formatter (S22-a honesty note) */
#include "ff_intent.h" /* the emit seam */
#include "ff_layout.h"
#include "ff_theme.h"

/* ---------------------------------------------------------------------
 * Layout constants — local to this face (its own design; the geometry
 * MATH they feed, ff_layout.h, is the shared/tested part).
 * ------------------------------------------------------------------- */

#define FF_SIGNALS_SAFETY_PX 10.0f /* see scr_compose.c's FF_COMPOSE_SAFETY_PX — same rationale */

/* Header: centered "SIGNALS" caption + the numbered unread badge. Not a
 * control — nothing here is clickable, so no adjacency partner for the
 * list's first row. */
#define FF_SIGNALS_HEADER_Y 34

/* Sub-screen (picker/thread) pinned back button — the spec's ">=44px
 * escape, off the rim". Its band's own chord margin is computed below. */
#define FF_SIGNALS_BACK_Y 30
#define FF_SIGNALS_BACK_PX FF_THEME_MIN_HIT_PX

/* The inbox conversation list: full-bleed 68px rows, touching (design
 * canvas), inside a viewport whose lower edge stays above the pole. */
#define FF_SIGNALS_LIST_TOP_Y 74
#define FF_SIGNALS_LIST_H     268
#define FF_SIGNALS_LIST_BOT_Y (FF_SIGNALS_LIST_TOP_Y + FF_SIGNALS_LIST_H)

#define FF_SIGNALS_ROW_H 68 /* the canvas's full-bleed pitch — rows touch */
/* Hit-rect vertical inset: rows touch visually; tap targets keep the 8px
 * adjacency floor between them (2 * inset) and still clear the 44px size
 * floor. See this file's header comment. */
#define FF_SIGNALS_ROW_HIT_INSET_Y 4
_Static_assert(FF_SIGNALS_ROW_H - 2 * FF_SIGNALS_ROW_HIT_INSET_Y >= FF_THEME_MIN_HIT_PX,
               "signals row tap target must clear the 44px hit floor");
_Static_assert(2 * FF_SIGNALS_ROW_HIT_INSET_Y >= FF_HIT_MIN_GAP_PX,
               "adjacent signals row tap targets must clear the 8px adjacency floor");

/* The FAB. Deco disc geometry is the design canvas's (a 240px amber
 * circle whose center sits off-glass past the bottom-right rim); the TAP
 * TARGET is a 48px square placed fully on-glass — its far corner
 * (350,350) sits 203.6px from the puck center, inside the 206px radius,
 * verified by the sweep, not only by this math. */
#define FF_SIGNALS_FAB_DECO_D  240
#define FF_SIGNALS_FAB_DECO_X  278
#define FF_SIGNALS_FAB_DECO_Y  280
#define FF_SIGNALS_FAB_HIT_PX  48
#define FF_SIGNALS_FAB_HIT_X   302
#define FF_SIGNALS_FAB_HIT_Y   302
_Static_assert(FF_SIGNALS_FAB_HIT_PX >= FF_THEME_MIN_HIT_PX, "FAB tap target must clear the hit floor");

/* Every inbox row's hit-rect stops this far left of the FAB's tap
 * target, so a row and the FAB can never violate the 8px adjacency floor
 * — including OVERFLOW rows, whose raw scroll-0 rects land below the
 * viewport in exactly the FAB's y-range (see this file's header). The
 * row's right-hand content (age, badge) is deliberately outside the tap
 * target; the row is generously tappable everywhere else. */
#define FF_SIGNALS_ROW_HIT_CLEAR_X (FF_SIGNALS_FAB_HIT_X - FF_HIT_MIN_GAP_PX)

/* The FAB's `+` glyph center — the CENTER of the 48px tap target
 * (302..350 -> 326), not the deco disc's own centroid: the glyph is the
 * user's aim point, so it must mark where the tap actually registers
 * (slice-b UX review nit, fixed here in slice c). */
#define FF_SIGNALS_FAB_GLYPH_C (FF_SIGNALS_FAB_HIT_X + FF_SIGNALS_FAB_HIT_PX / 2)

/* Row internals. */
#define FF_SIGNALS_AVATAR_PX 46
#define FF_SIGNALS_BADGE_H   20

/* ---------------------------------------------------------------------
 * Thread sub-view geometry (S24 slice c — design canvas ThreadGroup /
 * ThreadPerson artboards). One scrollable message list under the header
 * band; the CREW thread's list runs to the fade, the 1:1 list stops
 * earlier to make room for the quick-chip strip. Message rows are pure
 * deco (bubbles are not tappable), so — unlike the inbox — overflow rows
 * can never collide with anything clickable at any scroll offset.
 * ------------------------------------------------------------------- */

#define FF_SIGNALS_THREAD_LIST_TOP_Y   84
#define FF_SIGNALS_THREAD_LIST_H_CREW  190 /* -> y 274; fade + FAB own the pole */
#define FF_SIGNALS_THREAD_LIST_H_1TO1  172 /* -> y 256; the chip strip sits below */

/* Quick-reply chips (1:1 only): OMW / IN 5 MIN / PULSE, one row, capped
 * on the right so the strip and the FAB's tap target keep the adjacency
 * floor (the same clearance rule as FF_SIGNALS_ROW_HIT_CLEAR_X). */
#define FF_SIGNALS_CHIP_Y         264
#define FF_SIGNALS_CHIP_H         44
#define FF_SIGNALS_CHIP_GAP       8
#define FF_SIGNALS_CHIP_MAX_RIGHT (FF_SIGNALS_FAB_HIT_X - FF_HIT_MIN_GAP_PX)
_Static_assert(FF_SIGNALS_CHIP_H >= FF_THEME_MIN_HIT_PX, "quick chips must clear the 44px hit floor");
_Static_assert(FF_SIGNALS_CHIP_GAP >= FF_HIT_MIN_GAP_PX,
               "adjacent quick chips must clear the 8px adjacency floor");

/* Message-row internals (heights follow the canvas's single-line rows;
 * long content ellipsizes rather than wraps, so the pitch stays fixed
 * and the newest message's scroll anchor is stable). */
#define FF_SIGNALS_MSG_SENDER_H 18  /* dot + name + age line above a CREW inbound bubble */
#define FF_SIGNALS_MSG_BUBBLE_H 34  /* one-line text bubble */
#define FF_SIGNALS_MSG_RALLY_H  46  /* RALLY badge + place callout */
#define FF_SIGNALS_MSG_EVENT_H  22  /* pulse/flare one-liner */
#define FF_SIGNALS_MSG_AGE_H    16  /* age line under a bubble (rows with no sender line) */
#define FF_SIGNALS_MSG_GAP      10
#define FF_SIGNALS_MSG_MAX_W    236 /* bubble width cap; text past it ellipsizes */

/* signals_safe_margin_x — int32/ceil wrapper around ff_layout_safe_margin_x,
 * bound to this puck's center/radius and this file's safety buffer (twin of
 * scr_compose.c's compose_safe_margin_x). */
static int32_t signals_safe_margin_x(int32_t top_y, int32_t h)
{
    float margin = ff_layout_safe_margin_x((float)top_y, (float)h, (float)FF_THEME_PUCK_RADIUS_PX,
                                            (float)FF_THEME_PUCK_RADIUS_PX, FF_SIGNALS_SAFETY_PX);
    return (int32_t)ceilf(margin);
}

static void signals_child_deco(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

/* ---------------------------------------------------------------------
 * Press-down feedback (S24 AC7) — mirrors the composer's convention
 * (compose_key_press_feedback): a press lights the control amber with
 * dark ink the instant the finger is down; LVGL clears it on release.
 * The FAB variant dims instead (it is already amber).
 * ------------------------------------------------------------------- */

static void signals_press_feedback(lv_obj_t *btn)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_AMBER), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_PRESSED);
}

/* ---------------------------------------------------------------------
 * Intent emitters (the whole seam between this screen and the shell).
 * ------------------------------------------------------------------- */

/* A conversation row -> FF_INTENT_INBOX_OPEN_THREAD carrying the row's
 * conversation key (0 = CREW, else the member's node id) through
 * user_data — the compose_key_click_cb pattern. The shell validates a
 * member key against the roster before navigating (a pure-render screen
 * must not decide membership). */
static void signals_open_thread_cb(lv_event_t *e)
{
    uintptr_t node = (uintptr_t)lv_event_get_user_data(e);
    ff_intent_t in = {.kind = FF_INTENT_INBOX_OPEN_THREAD, .u = {0}};
    in.u.node_id = (uint32_t)node;
    ff_intent_emit(&in);
}

/* A recipient-picker row -> FF_INTENT_INBOX_PICK (same key convention).
 * Distinct from OPEN_THREAD so slice (d) can re-route picks to the
 * action popup in the shell alone — see ff_intent.h. */
static void signals_pick_cb(lv_event_t *e)
{
    uintptr_t node = (uintptr_t)lv_event_get_user_data(e);
    ff_intent_t in = {.kind = FF_INTENT_INBOX_PICK, .u = {0}};
    in.u.node_id = (uint32_t)node;
    ff_intent_emit(&in);
}

/* The `+` FAB -> FF_INTENT_INBOX_NEW (no payload). */
static void signals_fab_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_INBOX_NEW, .u = {0}};
    ff_intent_emit(&in);
}

/* Picker/thread back "<" -> FF_INTENT_BACK; the shell pops the sub-view
 * (thread/picker -> inbox). Which screen is revealed is never this
 * file's decision. */
static void signals_back_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_BACK, .u = {0}};
    ff_intent_emit(&in);
}

/* A 1:1 quick chip (OMW / IN 5 MIN) -> FF_INTENT_CANNED_REPLY, carrying
 * which canned reply through user_data (the compose_key_click_cb
 * pattern). The DESTINATION is never this screen's claim: the shell
 * resolves the open thread's scope (S24 "the open thread IS the send
 * scope" — see the CANNED_REPLY handler in ff_shell.c). */
static void signals_chip_reply_cb(lv_event_t *e)
{
    uintptr_t which = (uintptr_t)lv_event_get_user_data(e);
    ff_intent_t in = {.kind = FF_INTENT_CANNED_REPLY, .u = {0}};
    in.u.reply = (ff_wiring_canned_reply_t)which;
    ff_intent_emit(&in);
}

/* The 1:1 PULSE chip -> FF_INTENT_SIG_PULSE (the S22(d) send intent; the
 * shell aims it at the open thread's scope). */
static void signals_chip_pulse_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_SIG_PULSE, .u = {0}};
    ff_intent_emit(&in);
}

/* ---------------------------------------------------------------------
 * Small shared builders.
 * ------------------------------------------------------------------- */

/* The pinned back button for the picker/thread sub-screens: a real
 * FF_THEME_MIN_HIT_PX target at its band's own chord margin. */
static void signals_build_back(lv_obj_t *parent)
{
    int32_t margin = signals_safe_margin_x(FF_SIGNALS_BACK_Y, FF_SIGNALS_BACK_PX);

    lv_obj_t *back = lv_button_create(parent);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, FF_SIGNALS_BACK_PX, FF_SIGNALS_BACK_PX);
    lv_obj_set_pos(back, margin, FF_SIGNALS_BACK_Y);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(back, signals_back_cb, LV_EVENT_CLICKED, NULL);
    signals_press_feedback(back);

    lv_obj_t *glyph = lv_label_create(back);
    lv_label_set_text(glyph, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(glyph, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(glyph, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_center(glyph);
}

/* A numbered amber unread badge pill (99+ saturates the display, never
 * the count itself — the model's number stays honest). */
static void signals_build_badge(lv_obj_t *parent, uint16_t count, lv_align_t align, int32_t dx, int32_t dy)
{
    char buf[8];
    if (count > 99u) {
        snprintf(buf, sizeof(buf), "99+");
    } else {
        snprintf(buf, sizeof(buf), "%u", (unsigned)count);
    }

    lv_obj_t *badge = lv_obj_create(parent);
    signals_child_deco(badge);
    int32_t w = (count > 9u) ? 30 : 22;
    lv_obj_set_size(badge, w, FF_SIGNALS_BADGE_H);
    lv_obj_set_style_radius(badge, FF_SIGNALS_BADGE_H / 2, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_align(badge, align, dx, dy);

    lv_obj_t *num = lv_label_create(badge);
    lv_label_set_text(num, buf);
    lv_obj_set_style_text_font(num, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(num, lv_color_hex(FF_THEME_COLOR_BG), 0); /* dark ink on amber */
    lv_obj_center(num);
}

/* The CREW cluster avatar: a dark disc holding three overlapping crew-
 * color dots (the canvas's communal-thread mark). */
static void signals_build_crew_avatar(lv_obj_t *parent, int32_t x, bool colorblind)
{
    lv_obj_t *disc = lv_obj_create(parent);
    signals_child_deco(disc);
    lv_obj_set_size(disc, FF_SIGNALS_AVATAR_PX, FF_SIGNALS_AVATAR_PX);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_align(disc, LV_ALIGN_LEFT_MID, x, 0);

    static const int8_t offs[3][2] = {{-7, -3}, {6, -6}, {0, 7}};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *dot = lv_obj_create(disc);
        signals_child_deco(dot);
        lv_obj_set_size(dot, 16, 16);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(ff_theme_crew_color((uint8_t)i, colorblind)), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(FF_THEME_COLOR_BG), 0);
        lv_obj_set_style_border_width(dot, 2, 0);
        lv_obj_set_style_border_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_CENTER, offs[i][0], offs[i][1]);
    }
}

/* A member avatar: crew-color disc with the member's initial in dark
 * ink; a member with no known initial gets an honest empty disc. */
static void signals_build_member_avatar(lv_obj_t *parent, int32_t x, char initial, uint8_t color_idx,
                                        bool colorblind)
{
    lv_obj_t *disc = lv_obj_create(parent);
    signals_child_deco(disc);
    lv_obj_set_size(disc, FF_SIGNALS_AVATAR_PX, FF_SIGNALS_AVATAR_PX);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(ff_theme_crew_color(color_idx, colorblind)), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_align(disc, LV_ALIGN_LEFT_MID, x, 0);

    if (initial != '\0') {
        char buf[2] = {initial, '\0'};
        lv_obj_t *ch = lv_label_create(disc);
        lv_label_set_text(ch, buf);
        lv_obj_set_style_text_font(ch, FF_THEME_FONT_HEADLINE, 0);
        lv_obj_set_style_text_color(ch, lv_color_hex(FF_THEME_COLOR_BG), 0);
        lv_obj_center(ch);
    }
}

/* ---------------------------------------------------------------------
 * Preview / presence text (formatting only — every FACT comes from the
 * core model; ages go through the one shared ff_fmt_age).
 * ------------------------------------------------------------------- */

static char const *signals_kind_word(ff_feed_kind_t kind)
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

/**
 * The one-line preview under a conversation's name. Direction and sender
 * facts come from the model and are never guessed at:
 *   - an OUTGOING newest item is prefixed "YOU:" (preview_dir OUT);
 *   - a CREW-row inbound item is prefixed with its joined sender name
 *     when the model knows one (preview_from_known — the spec's "sender
 *     prefix for CREW"); an unjoined sender simply gets no prefix (no
 *     claim), never a fabricated name;
 *   - a member row's inbound sender IS the row identity — no prefix;
 *   - TEXT shows its text; PULSE/FLARE their kind word; RALLY/STATUS
 *     "KIND · text" when text exists (a rally's place name).
 */
static void signals_preview_text(ff_inbox_conv_t const *cv, char *buf, size_t n)
{
    char body[FF_FEED_TEXT_LEN + 12];
    switch (cv->preview_kind) {
    case FEED_TEXT:
        snprintf(body, sizeof(body), "%s", cv->preview_text);
        break;
    case FEED_RALLY:
    case FEED_STATUS:
        /* "KIND - text": a plain ASCII separator, not the canvas's middot
         * (U+00B7) — the vendored Montserrat subset lacks that glyph and
         * renders it as tofu, the exact trap S22's divider already hit. */
        if (cv->preview_text[0] != '\0') {
            snprintf(body, sizeof(body), "%s - %s", signals_kind_word(cv->preview_kind),
                     cv->preview_text);
        } else {
            snprintf(body, sizeof(body), "%s", signals_kind_word(cv->preview_kind));
        }
        break;
    default:
        snprintf(body, sizeof(body), "%s", signals_kind_word(cv->preview_kind));
        break;
    }

    if (cv->preview_dir == FEED_DIR_OUT) {
        snprintf(buf, n, "YOU: %s", body);
    } else if (cv->kind == FF_CONV_CREW && cv->preview_from_known && cv->preview_from_name[0] != '\0') {
        snprintf(buf, n, "%s: %s", cv->preview_from_name, body);
    } else {
        snprintf(buf, n, "%s", body);
    }
}

/* Honest presence line (S24 AC3, the ux review's blocker 1): the LEGIBLE
 * stale tier — SEEN/LOST render in stale-amber mono, never the dimmest
 * gray; LINKED (paired, never sighted — no honest age) renders muted but
 * still comfortably legible. The age is formatted only for SEEN, via the
 * one shared ff_fmt_age. */
static void signals_presence_text(ff_inbox_conv_t const *cv, char *buf, size_t n, uint32_t *out_color)
{
    switch (cv->presence) {
    case FF_PRESENCE_SEEN: {
        char age_buf[FF_APP_STR_SHORT];
        ff_fmt_age(age_buf, sizeof(age_buf), cv->presence_age_ms);
        snprintf(buf, n, "SEEN %s", age_buf);
        *out_color = FF_THEME_COLOR_STALE_AMBER;
        break;
    }
    case FF_PRESENCE_LOST:
        snprintf(buf, n, "LOST");
        *out_color = FF_THEME_COLOR_STALE_AMBER;
        break;
    case FF_PRESENCE_LINKED:
    default:
        snprintf(buf, n, "LINKED");
        *out_color = FF_THEME_COLOR_MUTED;
        break;
    }
}

/* ---------------------------------------------------------------------
 * Rows.
 * ------------------------------------------------------------------- */

/**
 * One big conversation/picker row at list-relative `y`, visually
 * full-bleed (touching its neighbors) with an inset transparent tap
 * target wired to `cb` carrying `node` (see this file's header for the
 * touching-vs-adjacency reconciliation). `hit_clear_right` caps the tap
 * target's absolute right edge (the inbox passes the FAB clearance;
 * the picker, with no FAB, passes 0 = no cap). Returns the row container
 * (deco); the tap overlay is its last child.
 */
static lv_obj_t *signals_row_container(lv_obj_t *parent, int32_t y, int32_t margin_x, uint32_t node,
                                       lv_event_cb_t cb, int32_t hit_clear_right)
{
    int32_t row_w = FF_THEME_PUCK_PX - 2 * margin_x;

    lv_obj_t *row = lv_obj_create(parent);
    signals_child_deco(row);
    lv_obj_set_size(row, row_w, FF_SIGNALS_ROW_H);
    lv_obj_set_pos(row, margin_x, y);

    /* The tap target: inset vertically for the adjacency floor, capped
     * on the right when a clearance is given. Added as the FIRST child
     * so row content (all non-clickable) paints above it; LVGL's hit
     * test doesn't care about paint order, only the CLICKABLE flag. */
    int32_t hit_w = row_w;
    if (hit_clear_right > 0) {
        int32_t max_w = hit_clear_right - margin_x;
        if (hit_w > max_w) hit_w = max_w;
    }
    lv_obj_t *hit = lv_button_create(row);
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, hit_w, FF_SIGNALS_ROW_H - 2 * FF_SIGNALS_ROW_HIT_INSET_Y);
    lv_obj_set_pos(hit, 0, FF_SIGNALS_ROW_HIT_INSET_Y);
    lv_obj_set_style_radius(hit, 14, 0);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(hit, cb, LV_EVENT_CLICKED, (void *)(uintptr_t)node);
    signals_press_feedback(hit);

    return row;
}

/* One INBOX conversation row, rendered from the model row alone. */
static void signals_build_conv_row(lv_obj_t *parent, ff_inbox_conv_t const *cv, int32_t y, int32_t margin_x,
                                   bool colorblind)
{
    lv_obj_t *row = signals_row_container(parent, y, margin_x, cv->node_id, signals_open_thread_cb,
                                          FF_SIGNALS_ROW_HIT_CLEAR_X);
    int32_t row_w = FF_THEME_PUCK_PX - 2 * margin_x;

    if (cv->unread > 0) {
        /* Soft amber wash behind an unread conversation (the canvas's
         * rgba(amber, 0.08)) — an at-a-glance cue that needs no reading. */
        lv_obj_t *wash = lv_obj_create(row);
        signals_child_deco(wash);
        lv_obj_set_size(wash, row_w, FF_SIGNALS_ROW_H - 2);
        lv_obj_set_pos(wash, 0, 1);
        lv_obj_set_style_radius(wash, 16, 0);
        lv_obj_set_style_bg_color(wash, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
        lv_obj_set_style_bg_opa(wash, 20, 0); /* ~8% */
    }

    if (cv->kind == FF_CONV_CREW) {
        signals_build_crew_avatar(row, 8, colorblind);
    } else {
        signals_build_member_avatar(row, 8, cv->initial, cv->color_idx, colorblind);
    }

    int32_t text_x = 8 + FF_SIGNALS_AVATAR_PX + 12;
    int32_t right_w = 58; /* age / badge column */
    bool const quiet = (cv->item_count == 0 && cv->kind == FF_CONV_MEMBER);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(name, row_w - text_x - right_w);
    lv_label_set_text(name, (cv->kind == FF_CONV_CREW) ? "CREW"
                                                       : (cv->name[0] != '\0' ? cv->name : "MEMBER"));
    lv_obj_set_style_text_font(name, FF_THEME_FONT_HEADLINE, 0);
    /* Quiet members read at reduced-but-legible emphasis: the NAME dims
     * one step; the presence line below keeps its own full-legibility
     * tier (fade is de-emphasis, never the encoding of staleness). */
    lv_obj_set_style_text_color(name, lv_color_hex(quiet ? FF_THEME_COLOR_MUTED : FF_THEME_COLOR_INK), 0);
    lv_obj_set_pos(name, text_x, 12);

    /* Second line: the newest-item preview, or (a traffic-less row) the
     * honest state — presence for a member, "NO SIGNALS YET" for CREW. */
    char line[FF_FEED_TEXT_LEN + 32];
    uint32_t line_color = FF_THEME_COLOR_MUTED;
    if (cv->has_preview) {
        signals_preview_text(cv, line, sizeof(line));
    } else if (cv->kind == FF_CONV_CREW) {
        snprintf(line, sizeof(line), "NO SIGNALS YET");
        line_color = FF_THEME_COLOR_DIM;
    } else {
        signals_presence_text(cv, line, sizeof(line), &line_color);
    }

    /* One line, ellipsized: DOTS mode only truncates when the label's
     * HEIGHT is bounded too — width alone makes LVGL wrap instead (the
     * two-line preview the first render showed). */
    lv_obj_t *sub = lv_label_create(row);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(sub, row_w - text_x - right_w, 18);
    lv_label_set_text(sub, line);
    lv_obj_set_style_text_font(sub, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(line_color), 0);
    lv_obj_set_pos(sub, text_x, 38);

    /* Right column: mono age (of the newest item) + the numbered unread
     * badge. Quiet rows have neither (their presence line already says
     * everything honest there is to say). */
    if (cv->has_preview) {
        char age_buf[FF_APP_STR_SHORT];
        ff_fmt_age(age_buf, sizeof(age_buf), cv->preview_age_ms);
        lv_obj_t *age = lv_label_create(row);
        lv_label_set_text(age, age_buf);
        lv_obj_set_style_text_font(age, FF_THEME_FONT_CHIP, 0);
        lv_obj_set_style_text_color(age,
                                    lv_color_hex(cv->unread > 0 ? FF_THEME_COLOR_MUTED : FF_THEME_COLOR_DIM), 0);
        lv_obj_align(age, LV_ALIGN_RIGHT_MID, -2, cv->unread > 0 ? -12 : 0);
    }
    if (cv->unread > 0) {
        signals_build_badge(row, cv->unread, LV_ALIGN_RIGHT_MID, -2, 12);
    }
}

/* ---------------------------------------------------------------------
 * The bottom scroll fade — deco (the canvas's linear-gradient), painted
 * OVER the list's lower edge: transparent at its top, the puck bg color
 * at its bottom. A static const grad descriptor: LVGL stores the
 * pointer, so it must outlive the object tree — file-static.
 * ------------------------------------------------------------------- */

static void signals_build_bottom_fade(lv_obj_t *parent, int32_t bot_y)
{
    static lv_grad_dsc_t grad; /* zero-initialized once; filled on first use */
    grad.dir = LV_GRAD_DIR_VER;
    grad.stops_count = 2;
    grad.stops[0].color = lv_color_hex(FF_THEME_COLOR_BG);
    grad.stops[0].opa = LV_OPA_TRANSP;
    grad.stops[0].frac = 0;
    grad.stops[1].color = lv_color_hex(FF_THEME_COLOR_BG);
    grad.stops[1].opa = LV_OPA_COVER;
    grad.stops[1].frac = 255;

    lv_obj_t *fade = lv_obj_create(parent);
    signals_child_deco(fade);
    lv_obj_set_size(fade, FF_THEME_PUCK_PX, 40);
    lv_obj_set_pos(fade, 0, bot_y - 40);
    lv_obj_set_style_bg_opa(fade, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad(fade, &grad, 0);
}

/* ---------------------------------------------------------------------
 * The corner FAB (deco disc bleeding off the rim + on-glass tap target).
 *
 * ## Why a RIM-MASK RING, not style_clip_corner
 * The obvious way to clip the disc's bleed to the glass silhouette —
 * `lv_obj_set_style_clip_corner` on a circular container — HANGS this
 * project's headless software renderer (measured, not theorized: the
 * bisect that found it is in this slice's PR body; lv_refr.c's
 * clip_corner path composits the children through full-width ARGB
 * layers, which never completes in this configuration). So the bleed is
 * masked the other way around: the disc paints freely, and a BORDER-ONLY
 * ring object — inner edge exactly on the glass circle (radius 206),
 * thick enough to cover everything the disc can reach inside the window
 * — paints pure black (the screen's own letterbox color) over the
 * off-glass annulus, on top of the disc. Same pixels a real clip would
 * produce; only rects/borders, no layer compositing. The glyph and the
 * tap target sit fully on-glass and are drawn after the mask.
 * ------------------------------------------------------------------- */

/* Ring mask geometry: the disc's farthest visible point from the puck
 * center is the window corner (412,412), at ~291px; the ring's border
 * covers radius 206..(206+RING_T), so RING_T must reach past 291-206=85. */
#define FF_SIGNALS_FAB_RING_T 96

static void signals_build_fab(lv_obj_t *parent)
{
    /* Holder for the deco stack (disc under mask), full-puck, inert. */
    lv_obj_t *glass = lv_obj_create(parent);
    signals_child_deco(glass);
    lv_obj_set_size(glass, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_set_pos(glass, 0, 0);
    lv_obj_set_style_bg_opa(glass, LV_OPA_TRANSP, 0);

    lv_obj_t *disc = lv_obj_create(glass);
    signals_child_deco(disc);
    lv_obj_set_size(disc, FF_SIGNALS_FAB_DECO_D, FF_SIGNALS_FAB_DECO_D);
    lv_obj_set_pos(disc, FF_SIGNALS_FAB_DECO_X, FF_SIGNALS_FAB_DECO_Y);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);

    /* The rim mask (see the section comment): a border-only circle whose
     * inner edge is the glass circle, centered on the puck. Drawn AFTER
     * the disc, so it paints the disc's off-glass spill back to the
     * letterbox black the sim/device frame shows outside the glass. */
    lv_obj_t *ring = lv_obj_create(glass);
    signals_child_deco(ring);
    lv_obj_set_size(ring, FF_THEME_PUCK_PX + 2 * FF_SIGNALS_FAB_RING_T,
                    FF_THEME_PUCK_PX + 2 * FF_SIGNALS_FAB_RING_T);
    lv_obj_set_pos(ring, -FF_SIGNALS_FAB_RING_T, -FF_SIGNALS_FAB_RING_T);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, FF_SIGNALS_FAB_RING_T, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0x000000), 0); /* the letterbox black outside the glass */
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);

    /* The + glyph, dark ink, fully on-glass (drawn after the mask),
     * centered on the TAP TARGET's center (FF_SIGNALS_FAB_GLYPH_C — the
     * slice-b UX nit: the glyph is the aim point, so it marks where the
     * tap registers, not the deco disc's centroid). */
    lv_obj_t *bar_h = lv_obj_create(glass);
    signals_child_deco(bar_h);
    lv_obj_set_size(bar_h, 26, 4);
    lv_obj_set_pos(bar_h, FF_SIGNALS_FAB_GLYPH_C - 13, FF_SIGNALS_FAB_GLYPH_C - 2);
    lv_obj_set_style_radius(bar_h, 2, 0);
    lv_obj_set_style_bg_color(bar_h, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(bar_h, LV_OPA_COVER, 0);
    lv_obj_t *bar_v = lv_obj_create(glass);
    signals_child_deco(bar_v);
    lv_obj_set_size(bar_v, 4, 26);
    lv_obj_set_pos(bar_v, FF_SIGNALS_FAB_GLYPH_C - 2, FF_SIGNALS_FAB_GLYPH_C - 13);
    lv_obj_set_style_radius(bar_v, 2, 0);
    lv_obj_set_style_bg_color(bar_v, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(bar_v, LV_OPA_COVER, 0);

    /* The tap target — on-glass, 48px, over the disc's inner edge. The
     * already-amber control dims on press (the composer SEND precedent)
     * rather than lighting amber-on-amber. */
    lv_obj_t *hit = lv_button_create(parent);
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, FF_SIGNALS_FAB_HIT_PX, FF_SIGNALS_FAB_HIT_PX);
    lv_obj_set_pos(hit, FF_SIGNALS_FAB_HIT_X, FF_SIGNALS_FAB_HIT_Y);
    lv_obj_set_style_radius(hit, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(hit, lv_color_hex(FF_THEME_COLOR_BG), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(hit, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_add_event_cb(hit, signals_fab_cb, LV_EVENT_CLICKED, NULL);
}

/* ---------------------------------------------------------------------
 * Sub-view: INBOX.
 * ------------------------------------------------------------------- */

static void signals_build_header(lv_obj_t *parent, uint16_t unread)
{
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "SIGNALS");
    lv_obj_set_style_text_font(title, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, unread > 0 ? -16 : 0, FF_SIGNALS_HEADER_Y);

    if (unread > 0) {
        signals_build_badge(parent, unread, LV_ALIGN_TOP_MID, 42, FF_SIGNALS_HEADER_Y - 3);
    }
}

/* Honest no-crew hint (S24 AC9): shown beneath the CREW row when the
 * model holds no member conversations at all — never a blank face, and
 * pairing itself stays out of scope (the S22 note stands). */
static void signals_build_no_crew_hint(lv_obj_t *list, int32_t y, int32_t margin_x)
{
    lv_obj_t *box = lv_obj_create(list);
    signals_child_deco(box);
    lv_obj_set_size(box, FF_THEME_PUCK_PX - 2 * margin_x, 76);
    lv_obj_set_pos(box, margin_x, y + 10);

    lv_obj_t *headline = lv_label_create(box);
    lv_label_set_text(headline, "NO CREW LINKED YET");
    lv_obj_set_style_text_font(headline, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(headline, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_align(headline, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *sub = lv_label_create(box);
    lv_label_set_text(sub, "Paired friends show up here");
    lv_obj_set_style_text_font(sub, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 34);
}

static void signals_build_inbox(lv_obj_t *parent, ff_app_signals_t const *v, bool colorblind)
{
    signals_build_header(parent, ff_scr_signals_unread_count(v));

    /* Bottom-anchored scroll list — full puck width so the scroll
     * gesture isn't narrowed; rows are inset to the list's own
     * chord-derived margin (computed once against the viewport band's
     * worst-case edge). Clips its children, so overflow rows are
     * reachable by scroll and never paint past the viewport. */
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

    uint8_t n = ff_inbox_conv_count(&v->inbox);
    int32_t y = 0;
    for (uint8_t i = 0; i < n; i++) {
        ff_inbox_conv_t const *cv = ff_inbox_conv_at(&v->inbox, i);
        if (cv == NULL) continue;
        signals_build_conv_row(list, cv, y, list_margin, colorblind);
        y += FF_SIGNALS_ROW_H;
    }

    /* No paired members at all (only the ever-present CREW row): say so
     * honestly right in the list band (AC9). */
    if (n <= 1) {
        signals_build_no_crew_hint(list, y, list_margin);
    }

    signals_build_bottom_fade(parent, FF_SIGNALS_LIST_BOT_Y);
    signals_build_fab(parent);
}

/* ---------------------------------------------------------------------
 * Sub-view: RECIPIENT PICKER (the FAB's scope step).
 * ------------------------------------------------------------------- */

#define FF_SIGNALS_PICKER_LIST_TOP_Y 82
#define FF_SIGNALS_PICKER_LIST_H     260

static void signals_build_picker(lv_obj_t *parent, ff_app_signals_t const *v, bool colorblind)
{
    signals_build_back(parent);

    /* Caption sits to the back button's RIGHT (a centered caption would
     * collide with the button at this band's chord margin — seen in the
     * first render, not theorized). */
    int32_t back_margin = signals_safe_margin_x(FF_SIGNALS_BACK_Y, FF_SIGNALS_BACK_PX);
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "NEW SIGNAL");
    lv_obj_set_style_text_font(title, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_set_pos(title, back_margin + FF_SIGNALS_BACK_PX + 14, FF_SIGNALS_BACK_Y + 15);

    int32_t list_margin = signals_safe_margin_x(FF_SIGNALS_PICKER_LIST_TOP_Y, FF_SIGNALS_PICKER_LIST_H);

    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, FF_SIGNALS_PICKER_LIST_TOP_Y);
    lv_obj_set_size(list, FF_THEME_PUCK_PX, FF_SIGNALS_PICKER_LIST_H);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    /* CREW pinned first, then each paired member in the inbox's own
     * order — two passes over the one model list, no second source. */
    uint8_t n = ff_inbox_conv_count(&v->inbox);
    int32_t y = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (uint8_t i = 0; i < n; i++) {
            ff_inbox_conv_t const *cv = ff_inbox_conv_at(&v->inbox, i);
            if (cv == NULL) continue;
            bool const is_crew = (cv->kind == FF_CONV_CREW);
            if ((pass == 0) != is_crew) continue;

            lv_obj_t *row = signals_row_container(list, y, list_margin, cv->node_id, signals_pick_cb, 0);
            int32_t row_w = FF_THEME_PUCK_PX - 2 * list_margin;

            if (is_crew) {
                signals_build_crew_avatar(row, 8, colorblind);
            } else {
                signals_build_member_avatar(row, 8, cv->initial, cv->color_idx, colorblind);
            }

            int32_t text_x = 8 + FF_SIGNALS_AVATAR_PX + 12;
            lv_obj_t *name = lv_label_create(row);
            lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_width(name, row_w - text_x - 8);
            lv_label_set_text(name, is_crew ? "WHOLE CREW" : (cv->name[0] != '\0' ? cv->name : "MEMBER"));
            lv_obj_set_style_text_font(name, FF_THEME_FONT_HEADLINE, 0);
            lv_obj_set_style_text_color(name, lv_color_hex(FF_THEME_COLOR_INK), 0);
            lv_obj_set_pos(name, text_x, 12);

            /* Member rows carry their honest presence so the scope pick
             * is informed ("is Dana even reachable?"); the CREW row's
             * subtitle names what it is. */
            char line[32];
            uint32_t line_color = FF_THEME_COLOR_DIM;
            if (is_crew) {
                snprintf(line, sizeof(line), "EVERYONE PAIRED");
            } else {
                signals_presence_text(cv, line, sizeof(line), &line_color);
            }
            lv_obj_t *sub = lv_label_create(row);
            lv_label_set_text(sub, line);
            lv_obj_set_style_text_font(sub, FF_THEME_FONT_CHIP, 0);
            lv_obj_set_style_text_color(sub, lv_color_hex(line_color), 0);
            lv_obj_set_pos(sub, text_x, 38);

            y += FF_SIGNALS_ROW_H;
        }
    }
}

/* ---------------------------------------------------------------------
 * Sub-view: THREAD (S24 slice c) — the CREW + 1:1 message screens.
 *
 * One scrollable list of message rows (oldest at the top, NEWEST at the
 * bottom — the list is scrolled to its end after build), rendered purely
 * from `v->thread` (`ff_inbox_msg_t`): the screen sides each bubble by
 * the model's direction fact (FEED_DIR_OUT = my side, amber, right;
 * everything else = theirs, dark, left — an UNKNOWN direction renders
 * like inbound but its wording never claims an address, see
 * signals_msg_event_text), and names a sender ONLY when the model joined
 * one (`identity_known`; an unjoined sender renders an honest "UNKNOWN",
 * never a guessed name). Bubbles are DECO — not tappable — so the
 * message list has no hit-rects to collide with the chips/FAB at any
 * scroll offset. Long content ellipsizes (fixed row pitch; DOTS needs
 * bounded width AND height, the slice-b lesson).
 * ------------------------------------------------------------------- */

/* signals_thread_conv — the open conversation's row in the inbox model
 * (header presence / identity), or NULL (e.g. a fixture that authors no
 * convs — every dependent element renders honestly absent). */
static ff_inbox_conv_t const *signals_thread_conv(ff_app_signals_t const *v)
{
    uint8_t n = ff_inbox_conv_count(&v->inbox);
    for (uint8_t i = 0; i < n; i++) {
        ff_inbox_conv_t const *c = ff_inbox_conv_at(&v->inbox, i);
        if (c == NULL) continue;
        bool const want_crew = (v->thread_node == 0u);
        if ((want_crew && c->kind == FF_CONV_CREW) ||
            (!want_crew && c->kind == FF_CONV_MEMBER && c->node_id == v->thread_node)) {
            return c;
        }
    }
    return NULL;
}

/* The honest display name for a message's sender: the joined identity
 * when the model has one ("MEMBER" for a joined member whose name never
 * arrived — the inbox row precedent), an explicit "UNKNOWN" when it
 * does not. Never a guess. */
static char const *signals_msg_sender_name(ff_inbox_msg_t const *m)
{
    if (!m->identity_known) return "UNKNOWN";
    return (m->name[0] != '\0') ? m->name : "MEMBER";
}

/* A small label helper: font + color + text, returns the label. */
static lv_obj_t *signals_mk_label(lv_obj_t *parent, char const *text, lv_font_t const *font,
                                  uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

/* Clamp a freshly-created single-line label to `max_w`, ellipsizing when
 * it does not fit. Returns the final (clamped) width. */
static int32_t signals_label_clamp(lv_obj_t *label, int32_t max_w)
{
    lv_obj_update_layout(label);
    int32_t w = lv_obj_get_width(label);
    if (w > max_w) {
        w = max_w;
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(label, w, 18);
    }
    return w;
}

/* The CREW-thread inbound sender line: crew dot + name + mono age. */
static void signals_msg_sender_line(lv_obj_t *row, ff_inbox_msg_t const *m, char const *age,
                                    bool colorblind)
{
    uint32_t const name_color =
        m->identity_known ? ff_theme_crew_color(m->color_idx, colorblind) : FF_THEME_COLOR_MUTED;

    if (m->identity_known) {
        lv_obj_t *dot = lv_obj_create(row);
        signals_child_deco(dot);
        lv_obj_set_size(dot, 7, 7);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(name_color), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_pos(dot, 0, 5);
    }
    int32_t x = m->identity_known ? 13 : 0;

    lv_obj_t *name = signals_mk_label(row, signals_msg_sender_name(m), FF_THEME_FONT_CHIP, name_color);
    lv_obj_set_pos(name, x, 0);
    x += signals_label_clamp(name, 120) + 8;

    lv_obj_t *age_l = signals_mk_label(row, age, FF_THEME_FONT_CHIP, FF_THEME_COLOR_DIM);
    lv_obj_set_pos(age_l, x, 1);
}

/* The mono age line under a bubble (rows with no sender line above). */
static void signals_msg_age_below(lv_obj_t *row, char const *age, int32_t y, bool out, int32_t row_w)
{
    lv_obj_t *l = signals_mk_label(row, age, FF_THEME_FONT_CHIP, FF_THEME_COLOR_DIM);
    lv_obj_update_layout(l);
    lv_obj_set_pos(l, out ? (row_w - lv_obj_get_width(l) - 4) : 4, y);
}

/* A one-line bubble (TEXT / STATUS / the 1:1 pulse callout / an OUT
 * pulse): dark surface for theirs, solid amber with dark ink for mine.
 * Returns the bubble container (its content laid out by the caller when
 * `text` is NULL). */
static lv_obj_t *signals_msg_bubble(lv_obj_t *row, char const *text, int32_t y, bool out,
                                    int32_t row_w)
{
    lv_obj_t *bub = lv_obj_create(row);
    signals_child_deco(bub);
    lv_obj_set_style_radius(bub, 12, 0);
    lv_obj_set_style_bg_color(bub, lv_color_hex(out ? FF_THEME_COLOR_AMBER : FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(bub, LV_OPA_COVER, 0);

    int32_t w = FF_SIGNALS_MSG_MAX_W;
    if (text != NULL) {
        lv_obj_t *l = signals_mk_label(bub, text, FF_THEME_FONT_CHIP,
                                       out ? FF_THEME_COLOR_BG : FF_THEME_COLOR_INK);
        int32_t const tw = signals_label_clamp(l, FF_SIGNALS_MSG_MAX_W - 26);
        lv_obj_set_pos(l, 13, 8);
        w = tw + 26;
    }
    lv_obj_set_size(bub, w, FF_SIGNALS_MSG_BUBBLE_H);
    lv_obj_set_pos(bub, out ? (row_w - w) : 0, y);
    return bub;
}

/* The RALLY callout: violet-tinted box, RALLY caption + place name. */
static void signals_msg_rally(lv_obj_t *row, ff_inbox_msg_t const *m, int32_t y, bool out,
                              int32_t row_w)
{
    lv_obj_t *box = lv_obj_create(row);
    signals_child_deco(box);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(FF_THEME_CREW_VIOLET), 0);
    lv_obj_set_style_bg_opa(box, 30, 0); /* ~12% — the canvas's violet wash */
    lv_obj_set_style_border_color(box, lv_color_hex(FF_THEME_CREW_VIOLET), 0);
    lv_obj_set_style_border_opa(box, LV_OPA_30, 0);
    lv_obj_set_style_border_width(box, 1, 0);

    lv_obj_t *cap = signals_mk_label(box, "RALLY", FF_THEME_FONT_CHIP, FF_THEME_CREW_VIOLET);
    lv_obj_set_style_text_letter_space(cap, 2, 0);
    lv_obj_set_pos(cap, 12, 5);

    /* The place is the wire name verbatim (m->text); "" renders as the
     * honest absence of a place claim, never an invented one. */
    lv_obj_t *place = signals_mk_label(box, m->text, FF_THEME_FONT_CHIP, FF_THEME_COLOR_INK);
    int32_t const pw = signals_label_clamp(place, FF_SIGNALS_MSG_MAX_W - 24);
    lv_obj_set_pos(place, 12, 23);

    int32_t w = pw + 24;
    if (w < 96) w = 96; /* never narrower than its own RALLY caption */
    lv_obj_set_size(box, w, FF_SIGNALS_MSG_RALLY_H);
    lv_obj_set_pos(box, out ? (row_w - w) : 0, y);
}

/* Honest event wording for PULSE/FLARE — the address half of the
 * sentence comes from the DIRECTION FACT, never from which thread is
 * showing: a BROADCAST pulse "pulsed the crew", a DIRECT one "pulsed
 * you", an OUT one names my own act, and an UNKNOWN direction claims no
 * address at all (bare "pulsed"/"flared"). */
static char const *signals_msg_event_text(ff_inbox_msg_t const *m, bool crew_thread)
{
    bool const pulse = (m->kind == FEED_PULSE);
    if (m->dir == FEED_DIR_OUT) {
        if (pulse) {
            return crew_thread ? "You pulsed the crew" : "You pulsed";
        }
        return "You flared";
    }
    switch (m->dir) {
    case FEED_DIR_BROADCAST: return pulse ? "pulsed the crew" : "flared the crew";
    case FEED_DIR_DIRECT: return pulse ? "pulsed you" : "flared you";
    case FEED_DIR_UNKNOWN:
    default: return pulse ? "pulsed" : "flared"; /* no address claim */
    }
}

/* The small amber pulse mark (dot + ring). */
static void signals_msg_pulse_mark(lv_obj_t *parent, int32_t x, int32_t y)
{
    lv_obj_t *ring = lv_obj_create(parent);
    signals_child_deco(ring);
    lv_obj_set_size(ring, 13, 13);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_60, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_pos(ring, x, y);

    lv_obj_t *dot = lv_obj_create(parent);
    signals_child_deco(dot);
    lv_obj_set_size(dot, 5, 5);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(dot, x + 4, y + 4);
}

/* An inbound CREW-thread event one-liner (pulse/flare): crew dot + mark
 * + "<NAME> <event>" + age at the right. */
static void signals_msg_event_line(lv_obj_t *row, ff_inbox_msg_t const *m, char const *age,
                                   bool colorblind, int32_t row_w)
{
    uint32_t const name_color =
        m->identity_known ? ff_theme_crew_color(m->color_idx, colorblind) : FF_THEME_COLOR_MUTED;

    int32_t x = 0;
    if (m->identity_known) {
        lv_obj_t *dot = lv_obj_create(row);
        signals_child_deco(dot);
        lv_obj_set_size(dot, 7, 7);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(name_color), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_pos(dot, 0, 7);
        x = 13;
    }
    if (m->kind == FEED_PULSE) {
        signals_msg_pulse_mark(row, x, 4);
        x += 19;
    }
    lv_obj_t *name = signals_mk_label(row, signals_msg_sender_name(m), FF_THEME_FONT_CHIP, name_color);
    lv_obj_set_pos(name, x, 1);
    x += signals_label_clamp(name, 110) + 6;

    lv_obj_t *what = signals_mk_label(row, signals_msg_event_text(m, true), FF_THEME_FONT_CHIP,
                                      (m->kind == FEED_FLARE) ? FF_THEME_COLOR_STALE_AMBER
                                                              : FF_THEME_COLOR_INK);
    lv_obj_set_pos(what, x, 1);
    (void)signals_label_clamp(what, row_w - x - 58);

    lv_obj_t *age_l = signals_mk_label(row, age, FF_THEME_FONT_CHIP, FF_THEME_COLOR_DIM);
    lv_obj_update_layout(age_l);
    lv_obj_set_pos(age_l, row_w - lv_obj_get_width(age_l) - 4, 1);
}

/**
 * One message row at list-relative `y`. Returns the row's total pitch
 * (content + gap) so the caller can stack the next one. `crew_thread`
 * picks the CREW layout (inbound rows carry a sender line / event line
 * with the sender named) vs the 1:1 layout (the header already names the
 * peer, so inbound rows are bubble + age only — the canvas's shapes).
 */
static int32_t signals_build_msg(lv_obj_t *list, ff_inbox_msg_t const *m, int32_t y,
                                 int32_t margin_x, bool crew_thread, bool colorblind)
{
    int32_t const row_w = FF_THEME_PUCK_PX - 2 * margin_x;
    bool const out = (m->dir == FEED_DIR_OUT);
    bool const sender_line = crew_thread && !out;

    char age[FF_APP_STR_SHORT];
    ff_fmt_age(age, sizeof(age), m->age_ms);

    /* Content height by kind/shape. */
    int32_t content_h;
    bool event_line = false; /* the CREW one-liner shape (no bubble, no age-below) */
    switch (m->kind) {
    case FEED_RALLY:
        content_h = FF_SIGNALS_MSG_RALLY_H;
        break;
    case FEED_PULSE:
    case FEED_FLARE:
        if (sender_line) {
            event_line = true;
            content_h = FF_SIGNALS_MSG_EVENT_H;
        } else {
            content_h = FF_SIGNALS_MSG_BUBBLE_H;
        }
        break;
    default: /* TEXT / STATUS */
        content_h = FF_SIGNALS_MSG_BUBBLE_H;
        break;
    }
    int32_t const head_h = (sender_line && !event_line) ? FF_SIGNALS_MSG_SENDER_H : 0;
    int32_t const tail_h = (!sender_line) ? FF_SIGNALS_MSG_AGE_H : 0;
    int32_t const row_h = head_h + content_h + tail_h;

    lv_obj_t *row = lv_obj_create(list);
    signals_child_deco(row);
    lv_obj_set_size(row, row_w, row_h);
    lv_obj_set_pos(row, margin_x, y);

    if (event_line) {
        /* CREW inbound pulse/flare: single line with the sender named. */
        signals_msg_event_line(row, m, age, colorblind, row_w);
        return row_h + FF_SIGNALS_MSG_GAP;
    }

    if (sender_line) {
        signals_msg_sender_line(row, m, age, colorblind);
    }

    switch (m->kind) {
    case FEED_RALLY:
        signals_msg_rally(row, m, head_h, out, row_w);
        break;
    case FEED_PULSE:
    case FEED_FLARE: {
        /* 1:1 pulse callout / any OUT pulse/flare: a bubble carrying the
         * honest event sentence (inbound names the sender; OUT names my
         * own act). */
        lv_obj_t *bub = signals_msg_bubble(row, NULL, head_h, out, row_w);
        int32_t x = 13;
        if (m->kind == FEED_PULSE) {
            signals_msg_pulse_mark(bub, x, 10);
            x += 19;
        }
        uint32_t const ink = out ? FF_THEME_COLOR_BG : FF_THEME_COLOR_INK;
        int32_t w;
        if (!out) {
            uint32_t const name_color = m->identity_known
                                            ? ff_theme_crew_color(m->color_idx, colorblind)
                                            : FF_THEME_COLOR_MUTED;
            lv_obj_t *name = signals_mk_label(bub, signals_msg_sender_name(m), FF_THEME_FONT_CHIP,
                                              name_color);
            lv_obj_set_pos(name, x, 8);
            x += signals_label_clamp(name, 100) + 6;
            lv_obj_t *what = signals_mk_label(bub, signals_msg_event_text(m, crew_thread),
                                              FF_THEME_FONT_CHIP, ink);
            lv_obj_set_pos(what, x, 8);
            w = x + signals_label_clamp(what, FF_SIGNALS_MSG_MAX_W - x - 13) + 13;
        } else {
            lv_obj_t *what = signals_mk_label(bub, signals_msg_event_text(m, crew_thread),
                                              FF_THEME_FONT_CHIP, ink);
            lv_obj_set_pos(what, x, 8);
            w = x + signals_label_clamp(what, FF_SIGNALS_MSG_MAX_W - x - 13) + 13;
        }
        lv_obj_set_size(bub, w, FF_SIGNALS_MSG_BUBBLE_H);
        lv_obj_set_pos(bub, out ? (row_w - w) : 0, head_h);
        break;
    }
    default: { /* TEXT / STATUS */
        char line[FF_FEED_TEXT_LEN + 12];
        if (m->kind == FEED_STATUS) {
            /* "STATUS - text": the inbox preview's own ASCII-separator
             * convention (the vendored font has no middot). */
            snprintf(line, sizeof(line), "STATUS - %s", m->text);
        } else {
            snprintf(line, sizeof(line), "%s", m->text);
        }
        (void)signals_msg_bubble(row, line, head_h, out, row_w);
        break;
    }
    }

    if (!sender_line) {
        signals_msg_age_below(row, age, head_h + content_h + 2, out, row_w);
    }
    return row_h + FF_SIGNALS_MSG_GAP;
}

/* The thread header. CREW: centered "CREW" + the roster fact "N CREW"
 * (paired members = the model's member conversations — a roster count,
 * never a present-tense "N here"). 1:1: crew dot + name, and the honest
 * presence line ("SEEN <age>" stale-amber / LOST / LINKED) beneath —
 * the legible stale tier, same formatter as the inbox rows. */
static void signals_build_thread_header(lv_obj_t *parent, ff_app_signals_t const *v,
                                        ff_inbox_conv_t const *cv, bool colorblind)
{
    if (v->thread_node == 0u) {
        lv_obj_t *title = signals_mk_label(parent, "CREW", FF_THEME_FONT_HEADLINE, FF_THEME_COLOR_INK);
        lv_obj_set_style_text_letter_space(title, 1, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

        /* Member conversations == paired members (ff_inbox: CREW + one
         * per paired member) — a roster fact read off the model, not a
         * second source. */
        uint8_t const n = ff_inbox_conv_count(&v->inbox);
        char buf[16];
        snprintf(buf, sizeof(buf), "%u CREW", (n > 0u) ? (unsigned)(n - 1u) : 0u);
        lv_obj_t *sub = signals_mk_label(parent, buf, FF_THEME_FONT_CHIP, FF_THEME_COLOR_DIM);
        lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 58);
        return;
    }

    char const *name_txt = (v->thread_name[0] != '\0') ? v->thread_name : "MEMBER";
    lv_obj_t *name = signals_mk_label(parent, name_txt, FF_THEME_FONT_HEADLINE, FF_THEME_COLOR_INK);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 8, 34);
    /* Ellipsize a long name, then hug the crew dot to the MEASURED text
     * edge (a fixed offset either collides with the back button or
     * floats detached on a short name — measured, not guessed). */
    int32_t const name_w = signals_label_clamp(name, 140);

    lv_obj_t *dot = lv_obj_create(parent);
    signals_child_deco(dot);
    lv_obj_set_size(dot, 9, 9);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(ff_theme_crew_color(v->thread_color_idx, colorblind)), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 8 - name_w / 2 - 12, 40);

    if (cv != NULL && cv->presence_valid) {
        char pres[32];
        uint32_t pres_color = FF_THEME_COLOR_MUTED;
        signals_presence_text(cv, pres, sizeof(pres), &pres_color);
        lv_obj_t *pl = signals_mk_label(parent, pres, FF_THEME_FONT_CHIP, pres_color);
        lv_obj_align(pl, LV_ALIGN_TOP_MID, 0, 58);
    }
}

/* One quick chip. `which` rides user_data for the canned replies;
 * `amber_text` marks the PULSE chip's accent. */
static void signals_build_chip(lv_obj_t *parent, int32_t x, int32_t w, char const *text,
                               bool amber_text, lv_event_cb_t cb, uintptr_t which)
{
    lv_obj_t *chip = lv_button_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, w, FF_SIGNALS_CHIP_H);
    lv_obj_set_pos(chip, x, FF_SIGNALS_CHIP_Y);
    lv_obj_set_style_radius(chip, FF_SIGNALS_CHIP_H / 2, 0);
    lv_obj_set_style_bg_color(chip, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(chip, cb, LV_EVENT_CLICKED, (void *)which);
    signals_press_feedback(chip);

    lv_obj_t *l = signals_mk_label(chip, text, FF_THEME_FONT_CHIP,
                                   amber_text ? FF_THEME_COLOR_AMBER : FF_THEME_COLOR_INK);
    lv_obj_center(l);
}

/* The 1:1 quick-chip strip: OMW / IN 5 MIN / PULSE, centered in the band
 * left of the FAB clearance (FF_SIGNALS_CHIP_MAX_RIGHT). */
static void signals_build_chips(lv_obj_t *parent)
{
    static const int32_t w_omw = 66, w_5min = 96, w_pulse = 74;
    int32_t const margin = signals_safe_margin_x(FF_SIGNALS_CHIP_Y, FF_SIGNALS_CHIP_H);
    int32_t const strip_w = w_omw + w_5min + w_pulse + 2 * FF_SIGNALS_CHIP_GAP;
    int32_t const avail = FF_SIGNALS_CHIP_MAX_RIGHT - margin;
    int32_t x = margin + (avail > strip_w ? (avail - strip_w) / 2 : 0);

    signals_build_chip(parent, x, w_omw, "OMW", false, signals_chip_reply_cb,
                       (uintptr_t)FF_WIRING_REPLY_OMW);
    x += w_omw + FF_SIGNALS_CHIP_GAP;
    signals_build_chip(parent, x, w_5min, "IN 5 MIN", false, signals_chip_reply_cb,
                       (uintptr_t)FF_WIRING_REPLY_5MIN);
    x += w_5min + FF_SIGNALS_CHIP_GAP;
    signals_build_chip(parent, x, w_pulse, "PULSE", true, signals_chip_pulse_cb, 0u);
}

static void signals_build_thread(lv_obj_t *parent, ff_app_signals_t const *v, bool colorblind)
{
    bool const crew_thread = (v->thread_node == 0u);
    ff_inbox_conv_t const *cv = signals_thread_conv(v);

    signals_build_back(parent);
    signals_build_thread_header(parent, v, cv, colorblind);

    int32_t const list_h = crew_thread ? FF_SIGNALS_THREAD_LIST_H_CREW : FF_SIGNALS_THREAD_LIST_H_1TO1;
    int32_t const list_margin = signals_safe_margin_x(FF_SIGNALS_THREAD_LIST_TOP_Y, list_h);

    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, FF_SIGNALS_THREAD_LIST_TOP_Y);
    lv_obj_set_size(list, FF_THEME_PUCK_PX, list_h);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    /* Bottom padding: the scroll-to-newest below parks the LAST message
     * this far above the hard clip, so the newest signal reads at full
     * strength instead of dying under the fade. */
    lv_obj_set_style_pad_bottom(list, 32, 0);

    uint8_t const n = ff_inbox_thread_count(&v->thread);
    int32_t y = 0;
    for (uint8_t i = 0; i < n; i++) {
        ff_inbox_msg_t const *m = ff_inbox_thread_at(&v->thread, i);
        if (m == NULL) continue;
        y += signals_build_msg(list, m, y, list_margin, crew_thread, colorblind);
    }

    if (n == 0) {
        /* Honest empty thread (a quiet member / a traffic-less crew). */
        lv_obj_t *empty = signals_mk_label(list, "NO SIGNALS YET", FF_THEME_FONT_CHIP,
                                           FF_THEME_COLOR_DIM);
        lv_obj_set_style_text_letter_space(empty, 2, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, -20);
    }

    /* Newest at the bottom: scroll to the end (LVGL clamps to the
     * scrollable range; a short thread stays put at the top). */
    lv_obj_update_layout(list);
    int32_t const overflow = lv_obj_get_scroll_bottom(list);
    if (overflow > 0) {
        lv_obj_scroll_to_y(list, overflow, LV_ANIM_OFF);
    }

    signals_build_bottom_fade(parent, FF_SIGNALS_THREAD_LIST_TOP_Y + list_h);
    if (!crew_thread) {
        signals_build_chips(parent);
    }
    signals_build_fab(parent);
}

/* ---------------------------------------------------------------------
 * Public helpers + entry point.
 * ------------------------------------------------------------------- */

uint16_t ff_scr_signals_unread_count(ff_app_signals_t const *v)
{
    if (v == NULL) {
        return 0;
    }
    uint32_t total = 0;
    uint8_t n = ff_inbox_conv_count(&v->inbox);
    for (uint8_t i = 0; i < n; i++) {
        ff_inbox_conv_t const *cv = ff_inbox_conv_at(&v->inbox, i);
        if (cv != NULL) {
            total += cv->unread;
        }
    }
    return (total > UINT16_MAX) ? UINT16_MAX : (uint16_t)total;
}

void ff_scr_signals_build(lv_obj_t *parent, ff_app_signals_t const *v, bool colorblind)
{
    if (parent == NULL || v == NULL) {
        return;
    }

    switch (v->subview) {
    case FF_SIG_SUB_PICKER:
        signals_build_picker(parent, v, colorblind);
        return;
    case FF_SIG_SUB_THREAD:
        signals_build_thread(parent, v, colorblind);
        return;
    case FF_SIG_SUB_INBOX:
    case FF_SIG_SUB_POPUP: /* slice (d) surface — unrouted; falls back to the inbox */
    case FF_SIG_SUB_RALLY: /* slice (d) surface — unrouted; falls back to the inbox */
    default:
        signals_build_inbox(parent, v, colorblind);
        return;
    }
}
