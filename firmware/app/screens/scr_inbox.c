/**
 * scr_inbox.c — see scr_inbox.h. The S24 Signals INBOX + THREAD
 * screens (spec: docs/specs/S24-signals-inbox.md, slices b/c): a pure
 * projection of `ff_app_inbox_t` — the sub-view selector, the core
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
 * FF_INBOX_ROW_HIT_CLEAR_X below) precisely so overflow rows can never
 * collide with it at any scroll offset.
 *
 * ## Touching rows vs. the adjacency floor (design/sweep reconciliation)
 * The design canvas draws conversation rows FULL-BLEED and TOUCHING
 * (68px pitch, no gap). The sweep's 8px adjacency floor is about HIT
 * rects, not paint: each row's visual container is non-clickable and
 * touches its neighbors exactly as drawn, while the row's actual tap
 * target (a transparent overlay) is inset FF_INBOX_ROW_HIT_INSET_Y
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
 * see inbox_build_fab's section comment for the measured bisect), so
 * the sim golden shows exactly what the round panel shows. Its TAP
 * TARGET is a separate transparent button covering the WHOLE visible
 * amber lens (corner-anchored to the window's bottom-right corner); its
 * off-glass excess is the masked letterbox corner, so the sweep excludes
 * it from circle-containment (the corner-bleed exclusion, alongside the
 * whole-puck one) while still enforcing the size and adjacency floors.
 * The visible disc is deco; the tap target doubles as the press overlay.
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
#include "scr_inbox.h"

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

#define FF_INBOX_SAFETY_PX 10.0f /* see scr_compose.c's FF_COMPOSE_SAFETY_PX — same rationale */

/* Header: centered "INBOX" caption (renamed 2026-09-01; was "SIGNALS") + the numbered unread badge. Not a
 * control — nothing here is clickable, so no adjacency partner for the
 * list's first row. */
#define FF_INBOX_HEADER_Y 34

/* Sub-screen (picker/thread) pinned back button — the spec's ">=44px
 * escape, off the rim". Its band's own chord margin is computed below. */
#define FF_INBOX_BACK_Y 30
#define FF_INBOX_BACK_PX FF_THEME_MIN_HIT_PX

/* The inbox conversation list: full-bleed 68px rows, touching (design
 * canvas), inside a viewport whose lower edge stays above the pole. */
#define FF_INBOX_LIST_TOP_Y 74
#define FF_INBOX_LIST_H     268
#define FF_INBOX_LIST_BOT_Y (FF_INBOX_LIST_TOP_Y + FF_INBOX_LIST_H)

#define FF_INBOX_ROW_H 68 /* the canvas's full-bleed pitch — rows touch */
/* Hit-rect vertical inset: rows touch visually; tap targets keep the 8px
 * adjacency floor between them (2 * inset) and still clear the 44px size
 * floor. See this file's header comment. */
#define FF_INBOX_ROW_HIT_INSET_Y 4
_Static_assert(FF_INBOX_ROW_H - 2 * FF_INBOX_ROW_HIT_INSET_Y >= FF_THEME_MIN_HIT_PX,
               "signals row tap target must clear the 44px hit floor");
_Static_assert(2 * FF_INBOX_ROW_HIT_INSET_Y >= FF_HIT_MIN_GAP_PX,
               "adjacent signals row tap targets must clear the 8px adjacency floor");

/* The FAB. Deco disc geometry is the design canvas's (a 240px amber
 * circle whose center sits off-glass past the bottom-right rim); the TAP
 * TARGET now covers the WHOLE visible amber lens (the maintainer's "the
 * add button is hard to tap / misaligned"): a corner-anchored square from
 * (300,300) to the window corner (412,412), so every point on the visible
 * amber is tappable — not the old tiny 48px patch over the disc's inner
 * edge. Its off-glass excess (past the round rim, toward 412,412) is the
 * masked letterbox corner — no physically-touchable surface on the round
 * device — so the hit-target sweep excludes it from the circle-containment
 * check the way it excludes the whole-puck gesture region (a NEW, narrow
 * corner-bleed exclusion in test_face_hit_targets.c; the size floor and the
 * adjacency floor still apply, and the FAB keeps its 8px clearance from the
 * rows/chips via FF_INBOX_ROW_HIT_CLEAR_X below). The already-transparent
 * hit doubles as the press overlay: its LV_STATE_PRESSED dark wash now
 * spans the whole lens, so the ENTIRE visible amber dims on press (the
 * maintainer's "the touchdown should fill the entire shape"). */
#define FF_INBOX_FAB_DECO_D  240
#define FF_INBOX_FAB_DECO_X  278
#define FF_INBOX_FAB_DECO_Y  280
#define FF_INBOX_FAB_HIT_X   300
#define FF_INBOX_FAB_HIT_Y   300
#define FF_INBOX_FAB_HIT_PX  (FF_THEME_PUCK_PX - FF_INBOX_FAB_HIT_X) /* -> 112: reaches the window corner */
_Static_assert(FF_INBOX_FAB_HIT_PX >= FF_THEME_MIN_HIT_PX, "FAB tap target must clear the hit floor");

/* Every inbox row's hit-rect stops this far left of the FAB's tap
 * target, so a row and the FAB can never violate the 8px adjacency floor
 * — including OVERFLOW rows, whose raw scroll-0 rects land below the
 * viewport in exactly the FAB's y-range (see this file's header). The
 * row's right-hand content (age, badge) is deliberately outside the tap
 * target; the row is generously tappable everywhere else. */
#define FF_INBOX_ROW_HIT_CLEAR_X (FF_INBOX_FAB_HIT_X - FF_HIT_MIN_GAP_PX)

/* The FAB's `+` glyph center — the centroid of the VISIBLE amber lens
 * (the deco disc ∩ the round glass), on the 45-degree diagonal by
 * symmetry (both circle centers lie on it). Measured, not the tap
 * target's own center: the enlarged hit deliberately runs off-glass to
 * the window corner, so its geometric center (356,356) is past the rim —
 * the glyph must sit on the amber the user actually sees and aims at
 * (the maintainer's "the + reads misaligned"). ~334 is the on-diagonal
 * midpoint between the disc's innermost on-glass arc point (~314) and the
 * glass rim (~352). */
#define FF_INBOX_FAB_GLYPH_C 334

/* Row internals. */
#define FF_INBOX_AVATAR_PX 46
#define FF_INBOX_BADGE_H   20

/* ---------------------------------------------------------------------
 * Thread sub-view geometry (S24 slice c — design canvas ThreadGroup /
 * ThreadPerson artboards). One scrollable message list under the header
 * band; the CREW thread's list runs to the fade, the 1:1 list stops
 * earlier to make room for the quick-chip strip. Message rows are pure
 * deco (bubbles are not tappable), so — unlike the inbox — overflow rows
 * can never collide with anything clickable at any scroll offset.
 * ------------------------------------------------------------------- */

#define FF_INBOX_THREAD_LIST_TOP_Y   80
#define FF_INBOX_THREAD_LIST_H_CREW  200 /* -> y 280, EXACTLY FF_INBOX_FAB_DECO_Y — PR #149 review
 * round 3: the band must never extend past the FAB's visible top edge.
 * A deeper band (round 2's 262) put the NEWEST message — the one that
 * must never be hidden — at risk under the slice on every rebuild that
 * restores an arbitrary saved scroll_y (PR #143's scroll-preservation:
 * a rebuild within the SAME thread restores whatever offset the user
 * last settled on, not just "newest at bottom"), which means a fully
 * correct per-row FAB inset would have to protect EVERY row (any one
 * could end up settled at the viewport's bottom edge), collapsing to
 * the same width cap as never entering the FAB zone at all — so
 * stopping the band here is both the SIMPLEST fix and not actually
 * narrower than a "fully correct" per-row inset would have been. `list`
 * clips its children to its own bounds (LVGL default), so at this
 * exact boundary there is no ambiguity: nothing in the list can ever
 * paint past y=280 regardless of x. */
#define FF_INBOX_THREAD_LIST_H_1TO1  182 /* -> y 262; the chip strip sits below (2px clearance, not the 8px hit floor — the list's own touch target is the exempt FLOATING catcher, Exclusion 4, so this gap is a visual choice, not a hit-adjacency requirement) */
#define FF_INBOX_THREAD_FADE_H       16  /* PR #149 review round 3: short, so it never meaningfully dims the newest message (was the shared 40px default) */

/* Quick-reply chips (1:1 only): OMW / IN 5 MIN / FLARE, one row, capped
 * on the right so the strip and the FAB's tap target keep the adjacency
 * floor (the same clearance rule as FF_INBOX_ROW_HIT_CLEAR_X). */
#define FF_INBOX_CHIP_Y         264
#define FF_INBOX_CHIP_H         44
#define FF_INBOX_CHIP_GAP       8
#define FF_INBOX_CHIP_MAX_RIGHT (FF_INBOX_FAB_HIT_X - FF_HIT_MIN_GAP_PX)
_Static_assert(FF_INBOX_CHIP_H >= FF_THEME_MIN_HIT_PX, "quick chips must clear the 44px hit floor");
_Static_assert(FF_INBOX_CHIP_GAP >= FF_HIT_MIN_GAP_PX,
               "adjacent quick chips must clear the 8px adjacency floor");

/* ---------------------------------------------------------------------
 * Action popup (S24 slice d) — the design canvas ActionPopup artboard.
 * Three big color-coded action rows over the (opaque-keyed) thread scope,
 * plus a >=44px close. All values lifted from ActionPopup.dc.html.
 * ------------------------------------------------------------------- */
#define FF_INBOX_POPUP_ROW_X   66
#define FF_INBOX_POPUP_ROW_W   280
#define FF_INBOX_POPUP_ROW_H   66
#define FF_INBOX_POPUP_ROW1_Y  92
#define FF_INBOX_POPUP_ROW2_Y  170
#define FF_INBOX_POPUP_ROW3_Y  248
#define FF_INBOX_POPUP_CLOSE_Y 330
#define FF_INBOX_POPUP_CLOSE_PX 54
_Static_assert(FF_INBOX_POPUP_ROW_H >= FF_THEME_MIN_HIT_PX, "popup rows must clear the 44px hit floor");
_Static_assert(FF_INBOX_POPUP_ROW2_Y - (FF_INBOX_POPUP_ROW1_Y + FF_INBOX_POPUP_ROW_H) >= FF_HIT_MIN_GAP_PX,
               "popup rows must clear the 8px adjacency floor");
_Static_assert(FF_INBOX_POPUP_CLOSE_PX >= FF_THEME_MIN_HIT_PX, "popup close must clear the 44px hit floor");

/* ---------------------------------------------------------------------
 * Rally screen (S24 slice d) — the design canvas Rally artboard. A
 * scrollable WHERE radio list (On Me + festpack landmark rows) above a
 * pinned WHEN chip + Send footer. Values follow Rally.dc.html, with the
 * row pitch opened past the canvas's 6px so adjacent TAP TARGETS clear
 * the 8px floor (the inbox row inset trade). The WHERE list holds the
 * demo/real pack's handful of landmarks without overflowing into the
 * footer; a bigger pack (a real festpack's 5 stages + Camp + On Me, easily
 * past this list's ~206px band) scrolls, its off-viewport rows clipped by
 * the list.
 *
 * Bugfix (maintainer report: "I can't scroll to select all the locations
 * on the send rally screen"): unlike the inbox/picker lists, each WHERE
 * row already carries its OWN clickable hit button (`inbox_build_rally_
 * row`'s inset `hit`, needed so a row stays individually tappable), so a
 * drag started ON a row's own hit rect already reaches `list`'s
 * LV_DIR_VER scroll fine (LVGL's lv_indev_find_scroll_obj walks UP from
 * whatever was hit and finds `list` as the nearest LV_DIR_VER-scrollable
 * ancestor — confirmed, not assumed: a real-indev drag starting on the
 * "On Me" row's hit target reaches the bottom of a 7-row list in one
 * drag). But `list` itself is deliberately NOT CLICKABLE (the inbox/
 * picker convention scr_inbox.c follows throughout — see this file's
 * header comment near inbox_build_thread), and — unlike a row's own
 * hit rect — the PLACES divider band, the ~8px gap between rows, a row's
 * own 4px top/bottom hit-inset, and a disabled "On Me" row (no GPS fix:
 * `inbox_build_rally_row` adds NO hit child at all when `!enabled`,
 * intentionally — "On Me without a fix is not tappable") carry no
 * clickable descendant whatsoever. A press there resolves to no object
 * inside `list` at all (falls through to a non-scrollable ancestor,
 * exactly PR #143's thread root cause, LVGL's lv_indev_search_obj/
 * lv_indev_find_scroll_obj) and the drag is never seen as a drag on
 * `list` — measured with the same real-indev harness PR #143 used
 * (started on the PLACES divider band: scroll_y stayed 0). Concretely
 * reachable in normal use: "On Me" (the FIRST row, the natural first
 * touch point for a scroll gesture) has NO hit target at all whenever
 * the puck has no GPS fix yet — a common festival-field state.
 *
 * Fix, mirroring PR #143's shape exactly: ONE inert, FLOATING hit
 * target sized to `list`'s own VIEWPORT (not the scrolled content — the
 * same "sized to content" shape PR #143 measured to fail, colliding
 * with whatever sits past the viewport edge; here that would be the
 * pinned WHEN/Send footer instead of the thread's header), added as
 * `list`'s FIRST child (index 0) — LVGL's lv_indev_search_obj walks a
 * node's children in REVERSE index order (src/indev/lv_indev.c), so a
 * LATER-added row (and its own hit button) is always checked BEFORE
 * this floating catcher: a tap that lands on a real row's hit target
 * still resolves to that row (taps keep working, unaffected), and only
 * a press that matches NOTHING else inside `list` — every dead zone
 * above — falls through to the catcher, which relays it to `list`'s own
 * scroll exactly like PR #143's thread fix. No CLICKED handler is ever
 * bound to it, so it adds zero tap behavior of its own — see
 * test_face_hit_targets.c's Exclusion 4 (a callback-less FLOATING hit
 * target is not adjacency-checked against the real rows it necessarily
 * overlaps at every scroll offset; unlike the thread, Rally's rows ARE
 * individually clickable, so this overlap is real and new, not the
 * thread's "nothing else is clickable" case). Gated on genuine overflow
 * (computed analytically from `r->place_count` — every WHERE row has a
 * fixed height, no measured-content variance to account for) so a short
 * pack (today's 2-place demo/test fixture) adds nothing extra.
 * ------------------------------------------------------------------- */
#define FF_INBOX_RALLY_LIST_TOP_Y   82 /* clears the pinned close/back (bottom y74) by the 8px floor */
#define FF_INBOX_RALLY_LIST_H       206
#define FF_INBOX_RALLY_ROW_H        52
#define FF_INBOX_RALLY_ROW_HIT_INSET_Y 4
#define FF_INBOX_RALLY_DIVIDER_H    22
#define FF_INBOX_RALLY_FOOTER_Y     300
#define FF_INBOX_RALLY_FOOTER_H     56
#define FF_INBOX_RALLY_WHEN_W       86
#define FF_INBOX_RALLY_FOOTER_GAP   10
_Static_assert(FF_INBOX_RALLY_ROW_H - 2 * FF_INBOX_RALLY_ROW_HIT_INSET_Y >= FF_THEME_MIN_HIT_PX,
               "rally WHERE rows must clear the 44px hit floor");
_Static_assert(2 * FF_INBOX_RALLY_ROW_HIT_INSET_Y >= FF_HIT_MIN_GAP_PX,
               "adjacent rally WHERE rows must clear the 8px adjacency floor");
_Static_assert(FF_INBOX_RALLY_FOOTER_H >= FF_THEME_MIN_HIT_PX, "rally footer controls must clear the 44px hit floor");
_Static_assert(FF_INBOX_RALLY_FOOTER_GAP >= FF_HIT_MIN_GAP_PX,
               "rally WHEN chip and Send must clear the 8px adjacency floor");
/* An 8px+ gap between the scroll list's viewport bottom and the pinned
 * footer, so a row parked at the viewport edge never crowds the footer. */
_Static_assert(FF_INBOX_RALLY_FOOTER_Y - (FF_INBOX_RALLY_LIST_TOP_Y + FF_INBOX_RALLY_LIST_H) >= FF_HIT_MIN_GAP_PX,
               "rally footer must clear the WHERE list viewport by the 8px floor");

/* Message-row internals (heights follow the canvas's single-line rows;
 * long content ellipsizes rather than wraps, so the pitch stays fixed
 * and the newest message's scroll anchor is stable). */
#define FF_INBOX_MSG_SENDER_H 16  /* dot + name + age line above a CREW inbound bubble (FF_THEME_FONT_CHIP's own 16px line height, zero slack) */
#define FF_INBOX_MSG_BUBBLE_H 34  /* one-line text bubble */
#define FF_INBOX_MSG_RALLY_H  46  /* RALLY badge + place callout — design canvas ThreadGroup.dc.html's own box: 8px v-padding + 12px caption + 18px place line, no inner gap (46 total) */
#define FF_INBOX_MSG_EVENT_H  22  /* pulse/flare one-liner */
#define FF_INBOX_MSG_AGE_H    16  /* age line under a bubble (rows with no sender line) */
#define FF_INBOX_MSG_GAP      0   /* PR #149 review round 2/3: tightened from 10 to fit more rows at rest (all message kinds; CREW at rest needs every px to reach 4 full rows within the FAB-safe band) */
#define FF_INBOX_MSG_MAX_W    294 /* bubble width cap; text past it ellipsizes. PR #149 review round 2:
 * widened from 236 for the 16px body font (the pre-PR 14px cap, proportionally
 * would be ~270, but the DEEPENED CREW list_h — needed for the 5-rows-visible
 * AC — narrows the round-glass-safe row width to ~295px at its worst extent,
 * which is the actual binding constraint: 294 is the largest cap that both (a)
 * fits every existing fixture's longest message ("sounds good, heading there
 * now", measured natural width 268px at FF_THEME_FONT_MSG_BODY, needs
 * MAX_W-26 >= 268) without truncating, and (b) stays on-glass at every scroll
 * offset in the deepened CREW band (measured row width ~295px, worst case). */

/* inbox_safe_margin_x — int32/ceil wrapper around ff_layout_safe_margin_x,
 * bound to this puck's center/radius and this file's safety buffer (twin of
 * scr_compose.c's compose_safe_margin_x). */
static int32_t inbox_safe_margin_x(int32_t top_y, int32_t h)
{
    float margin = ff_layout_safe_margin_x((float)top_y, (float)h, (float)FF_THEME_PUCK_RADIUS_PX,
                                            (float)FF_THEME_PUCK_RADIUS_PX, FF_INBOX_SAFETY_PX);
    return (int32_t)ceilf(margin);
}

static void inbox_child_deco(lv_obj_t *o)
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

static void inbox_press_feedback(lv_obj_t *btn)
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
static void inbox_open_thread_cb(lv_event_t *e)
{
    uintptr_t node = (uintptr_t)lv_event_get_user_data(e);
    ff_intent_t in = {.kind = FF_INTENT_INBOX_OPEN_THREAD, .u = {0}};
    in.u.node_id = (uint32_t)node;
    ff_intent_emit(&in);
}

/* A recipient-picker row -> FF_INTENT_INBOX_PICK (same key convention).
 * Distinct from OPEN_THREAD so slice (d) can re-route picks to the
 * action popup in the shell alone — see ff_intent.h. */
static void inbox_pick_cb(lv_event_t *e)
{
    uintptr_t node = (uintptr_t)lv_event_get_user_data(e);
    ff_intent_t in = {.kind = FF_INTENT_INBOX_PICK, .u = {0}};
    in.u.node_id = (uint32_t)node;
    ff_intent_emit(&in);
}

/* The `+` FAB -> FF_INTENT_INBOX_NEW (no payload). */
static void inbox_fab_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_INBOX_NEW, .u = {0}};
    ff_intent_emit(&in);
}

/* Picker/thread back "<" -> FF_INTENT_BACK; the shell pops the sub-view
 * (thread/picker -> inbox). Which screen is revealed is never this
 * file's decision. */
static void inbox_back_cb(lv_event_t *e)
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
static void inbox_chip_reply_cb(lv_event_t *e)
{
    uintptr_t which = (uintptr_t)lv_event_get_user_data(e);
    ff_intent_t in = {.kind = FF_INTENT_CANNED_REPLY, .u = {0}};
    in.u.reply = (ff_wiring_canned_reply_t)which;
    ff_intent_emit(&in);
}

/* The 1:1 FLARE chip -> FF_INTENT_INBOX_FLARE (the outbound quick signal is a
 * flare, "come find me", not a pulse; the shell aims it at the open thread's
 * scope through the S22(d) send seam). */
static void inbox_chip_flare_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_INBOX_FLARE, .u = {0}};
    ff_intent_emit(&in);
}

/* ---------------------------------------------------------------------
 * Action popup + Rally sub-view emitters (S24 slice d).
 * ------------------------------------------------------------------- */

/* One emitter per popup action row — the shell owns the scope. */
static void inbox_popup_compose_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_INBOX_POPUP_COMPOSE, .u = {0}};
    ff_intent_emit(&in);
}
static void inbox_popup_rally_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_INBOX_POPUP_RALLY, .u = {0}};
    ff_intent_emit(&in);
}
static void inbox_popup_flare_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_INBOX_POPUP_FLARE, .u = {0}};
    ff_intent_emit(&in);
}

/* A Rally WHERE radio row -> FF_INTENT_RALLY_SELECT_PLACE, carrying the
 * selection index (0 = On Me, 1.. = landmark) through user_data. */
static void inbox_rally_place_cb(lv_event_t *e)
{
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    ff_intent_t in = {.kind = FF_INTENT_RALLY_SELECT_PLACE, .u = {0}};
    in.u.rally_idx = (uint8_t)idx;
    ff_intent_emit(&in);
}

/* The Rally WHEN chip -> FF_INTENT_RALLY_CYCLE_WHEN. */
static void inbox_rally_when_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_RALLY_CYCLE_WHEN, .u = {0}};
    ff_intent_emit(&in);
}

/* The Rally Send button -> FF_INTENT_RALLY_SEND. */
static void inbox_rally_send_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_RALLY_SEND, .u = {0}};
    ff_intent_emit(&in);
}

/* ---------------------------------------------------------------------
 * Small shared builders.
 * ------------------------------------------------------------------- */

/* The pinned back button for the picker/thread sub-screens: a real
 * FF_THEME_MIN_HIT_PX target at its band's own chord margin. */
static void inbox_build_back(lv_obj_t *parent)
{
    int32_t margin = inbox_safe_margin_x(FF_INBOX_BACK_Y, FF_INBOX_BACK_PX);

    lv_obj_t *back = lv_button_create(parent);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, FF_INBOX_BACK_PX, FF_INBOX_BACK_PX);
    lv_obj_set_pos(back, margin, FF_INBOX_BACK_Y);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(back, inbox_back_cb, LV_EVENT_CLICKED, NULL);
    inbox_press_feedback(back);

    lv_obj_t *glyph = lv_label_create(back);
    lv_label_set_text(glyph, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(glyph, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(glyph, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_center(glyph);
}

/* A numbered amber unread badge pill (99+ saturates the display, never
 * the count itself — the model's number stays honest). */
static void inbox_build_badge(lv_obj_t *parent, uint16_t count, lv_align_t align, int32_t dx, int32_t dy)
{
    char buf[8];
    if (count > 99u) {
        snprintf(buf, sizeof(buf), "99+");
    } else {
        snprintf(buf, sizeof(buf), "%u", (unsigned)count);
    }

    lv_obj_t *badge = lv_obj_create(parent);
    inbox_child_deco(badge);
    int32_t w = (count > 9u) ? 30 : 22;
    lv_obj_set_size(badge, w, FF_INBOX_BADGE_H);
    lv_obj_set_style_radius(badge, FF_INBOX_BADGE_H / 2, 0);
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
static void inbox_build_crew_avatar(lv_obj_t *parent, int32_t x, bool colorblind)
{
    lv_obj_t *disc = lv_obj_create(parent);
    inbox_child_deco(disc);
    lv_obj_set_size(disc, FF_INBOX_AVATAR_PX, FF_INBOX_AVATAR_PX);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_align(disc, LV_ALIGN_LEFT_MID, x, 0);

    static const int8_t offs[3][2] = {{-7, -3}, {6, -6}, {0, 7}};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *dot = lv_obj_create(disc);
        inbox_child_deco(dot);
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
static void inbox_build_member_avatar(lv_obj_t *parent, int32_t x, char initial, uint8_t color_idx,
                                        bool colorblind)
{
    lv_obj_t *disc = lv_obj_create(parent);
    inbox_child_deco(disc);
    lv_obj_set_size(disc, FF_INBOX_AVATAR_PX, FF_INBOX_AVATAR_PX);
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

static char const *inbox_kind_word(ff_feed_kind_t kind)
{
    switch (kind) {
    case FEED_PULSE: return "PULSE";
    case FEED_RALLY: return "RALLY";
    case FEED_TEXT: return "TEXT";
    case FEED_STATUS: return "STATUS";
    case FEED_FLARE: return "FLARE";
    default: return "MESSAGE"; /* renamed 2026-09-01, was "SIGNAL" — reviewer PR #144 round 2: live user-facing
                                 * text even though FEED_PULSE/RALLY/TEXT/STATUS/FLARE cover every kind
                                 * currently produced, so this default is presently unreachable. */
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
static void inbox_preview_text(ff_inbox_conv_t const *cv, char *buf, size_t n)
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
            snprintf(body, sizeof(body), "%s - %s", inbox_kind_word(cv->preview_kind),
                     cv->preview_text);
        } else {
            snprintf(body, sizeof(body), "%s", inbox_kind_word(cv->preview_kind));
        }
        break;
    default:
        snprintf(body, sizeof(body), "%s", inbox_kind_word(cv->preview_kind));
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
static void inbox_presence_text(ff_inbox_conv_t const *cv, char *buf, size_t n, uint32_t *out_color)
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

/* The full-row press HIGHLIGHT (fix: "the touch-down state is not over all
 * the row — should be across the full thing"). A row's tap target must stay
 * inset 4px top/bottom to keep the 8px adjacency floor between neighbouring
 * rows (a full-height clickable would put two thread targets 0px apart), so
 * the pressed state can't just be the tap target's own bg — that leaves the
 * old 4px-short, right-capped pill the maintainer saw. Instead a deco
 * highlight spans the FULL row (full height, full width) BEHIND the content,
 * and press/release events on the (inset) tap target toggle it. */
static void inbox_row_press_ev(lv_event_t *e)
{
    lv_obj_t *hl = (lv_obj_t *)lv_event_get_user_data(e);
    if (hl == NULL) return;
    lv_opa_t const opa = (lv_event_get_code(e) == LV_EVENT_PRESSED) ? LV_OPA_40 : LV_OPA_TRANSP;
    lv_obj_set_style_bg_opa(hl, opa, 0);
}

/**
 * One big conversation/picker row at list-relative `y`, visually
 * full-bleed (touching its neighbors). A full-row deco highlight (lit on
 * press by inbox_row_press_ev) sits behind the content, and an inset
 * transparent tap target wired to `cb` carrying `node` sits on top (see
 * this file's header for the touching-vs-adjacency reconciliation). The tap
 * target keeps the 4px vertical inset for the row-to-row adjacency floor;
 * the highlight does not (it is deco, never a hit rect the sweep pairs).
 * `hit_clear_right` caps the tap target's AND the highlight's absolute right
 * edge — the inbox passes the FAB clearance ONLY for rows whose scroll-0
 * band actually reaches the FAB (inbox_row_overlaps_fab); every other row,
 * and every picker row (no FAB), passes 0 = full width. Returns the row
 * container (deco).
 */
static lv_obj_t *inbox_row_container(lv_obj_t *parent, int32_t y, int32_t margin_x, uint32_t node,
                                       lv_event_cb_t cb, int32_t hit_clear_right)
{
    int32_t row_w = FF_THEME_PUCK_PX - 2 * margin_x;

    lv_obj_t *row = lv_obj_create(parent);
    inbox_child_deco(row);
    lv_obj_set_size(row, row_w, FF_INBOX_ROW_H);
    lv_obj_set_pos(row, margin_x, y);

    int32_t hit_w = row_w;
    if (hit_clear_right > 0) {
        int32_t max_w = hit_clear_right - margin_x;
        if (hit_w > max_w) hit_w = max_w;
    }

    /* The tap target — created as the row's FIRST child (index 0): a stable
     * contract the touch tests locate it by (test_scr_intent.c's
     * find_row_hit_by_name). Inset vertically for the row-to-row adjacency
     * floor, capped on the right only when a clearance is given. The
     * CLICKABLE flag alone drives hit-testing (paint order is irrelevant),
     * so it paints at the bottom, under the highlight and content. CLICKED
     * carries the intent; the PRESSED/RELEASED/PRESS_LOST trio lights the
     * full-row highlight (wired below, once `hl` exists). */
    lv_obj_t *hit = lv_button_create(row);
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, hit_w, FF_INBOX_ROW_H - 2 * FF_INBOX_ROW_HIT_INSET_Y);
    lv_obj_set_pos(hit, 0, FF_INBOX_ROW_HIT_INSET_Y);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);

    /* The full-row highlight, above the (transparent) tap target and BELOW
     * the row content added by the caller. Amber, transparent until pressed.
     * Full ROW height (no inset) and hit_w wide (so on a FAB-overlapping row
     * it stops at the FAB clearance, never painting under the FAB — "only
     * clip the rows the FAB overlaps"). */
    lv_obj_t *hl = lv_obj_create(row);
    inbox_child_deco(hl);
    lv_obj_set_size(hl, hit_w, FF_INBOX_ROW_H);
    lv_obj_set_pos(hl, 0, 0);
    lv_obj_set_style_radius(hl, 12, 0);
    lv_obj_set_style_bg_color(hl, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_bg_opa(hl, LV_OPA_TRANSP, 0);

    lv_obj_add_event_cb(hit, cb, LV_EVENT_CLICKED, (void *)(uintptr_t)node);
    lv_obj_add_event_cb(hit, inbox_row_press_ev, LV_EVENT_PRESSED, hl);
    lv_obj_add_event_cb(hit, inbox_row_press_ev, LV_EVENT_RELEASED, hl);
    lv_obj_add_event_cb(hit, inbox_row_press_ev, LV_EVENT_PRESS_LOST, hl);

    return row;
}

/* Does a row at list-relative `y` sit close enough to the FAB (at scroll 0,
 * the position the hit-target sweep measures) that its tap target must be
 * clipped to keep the 8px clearance from the FAB? Conservative: uses the
 * whole ROW band (a superset of the inset hit) expanded by the adjacency
 * floor, so a row is clipped whenever it comes within 8px of the FAB's
 * y-range — and left FULL WIDTH otherwise. On glass a row scrolled up into
 * the FAB band is still safe: the FAB is a later sibling (drawn on top), so
 * a touch in the overlap resolves to the FAB, never ambiguously to the row
 * beneath it (LVGL's deepest/topmost-hit-wins — the same fact the sweep's
 * ancestor/whole-puck exclusions rest on). */
static bool inbox_row_overlaps_fab(int32_t y)
{
    int32_t const abs_top = FF_INBOX_LIST_TOP_Y + y; /* scroll 0 */
    int32_t const abs_bot = abs_top + FF_INBOX_ROW_H;
    int32_t const fab_top = FF_INBOX_FAB_HIT_Y - FF_HIT_MIN_GAP_PX;
    int32_t const fab_bot = FF_INBOX_FAB_HIT_Y + FF_INBOX_FAB_HIT_PX + FF_HIT_MIN_GAP_PX;
    return abs_bot > fab_top && abs_top < fab_bot;
}

/* One INBOX conversation row, rendered from the model row alone. */
static void inbox_build_conv_row(lv_obj_t *parent, ff_inbox_conv_t const *cv, int32_t y, int32_t margin_x,
                                   bool colorblind)
{
    /* Clip the tap target on the right ONLY when this row's scroll-0 band
     * actually reaches the FAB (fix 2: "clip the rows the FAB overlaps, not
     * all rows"); every other row is full width so taps anywhere on it
     * register and its press highlight spans the whole row. */
    int32_t const clear = inbox_row_overlaps_fab(y) ? FF_INBOX_ROW_HIT_CLEAR_X : 0;
    lv_obj_t *row = inbox_row_container(parent, y, margin_x, cv->node_id, inbox_open_thread_cb, clear);
    int32_t row_w = FF_THEME_PUCK_PX - 2 * margin_x;

    if (cv->unread > 0) {
        /* Soft amber wash behind an unread conversation (the canvas's
         * rgba(amber, 0.08)) — an at-a-glance cue that needs no reading. */
        lv_obj_t *wash = lv_obj_create(row);
        inbox_child_deco(wash);
        lv_obj_set_size(wash, row_w, FF_INBOX_ROW_H - 2);
        lv_obj_set_pos(wash, 0, 1);
        lv_obj_set_style_radius(wash, 16, 0);
        lv_obj_set_style_bg_color(wash, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
        lv_obj_set_style_bg_opa(wash, 20, 0); /* ~8% */
    }

    if (cv->kind == FF_CONV_CREW) {
        inbox_build_crew_avatar(row, 8, colorblind);
    } else {
        inbox_build_member_avatar(row, 8, cv->initial, cv->color_idx, colorblind);
    }

    int32_t text_x = 8 + FF_INBOX_AVATAR_PX + 12;
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
     * honest state — presence for a member, "NO MESSAGES YET" for CREW
     * (renamed 2026-09-01, was "NO SIGNALS YET" — reviewer PR #144
     * round 2: "signal" is confusing to the person holding the puck
     * here too, not just in the header title). */
    char line[FF_FEED_TEXT_LEN + 32];
    uint32_t line_color = FF_THEME_COLOR_MUTED;
    if (cv->has_preview) {
        inbox_preview_text(cv, line, sizeof(line));
    } else if (cv->kind == FF_CONV_CREW) {
        snprintf(line, sizeof(line), "NO MESSAGES YET");
        line_color = FF_THEME_COLOR_DIM;
    } else {
        inbox_presence_text(cv, line, sizeof(line), &line_color);
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
        inbox_build_badge(row, cv->unread, LV_ALIGN_RIGHT_MID, -2, 12);
    }
}

/* ---------------------------------------------------------------------
 * The bottom scroll fade — deco (the canvas's linear-gradient), painted
 * OVER the list's lower edge: transparent at its top, the puck bg color
 * at its bottom. A static const grad descriptor: LVGL stores the
 * pointer, so it must outlive the object tree — file-static.
 * ------------------------------------------------------------------- */

static void inbox_build_bottom_fade_h(lv_obj_t *parent, int32_t bot_y, int32_t h)
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
    inbox_child_deco(fade);
    lv_obj_set_size(fade, FF_THEME_PUCK_PX, h);
    lv_obj_set_pos(fade, 0, bot_y - h);
    lv_obj_set_style_bg_opa(fade, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad(fade, &grad, 0);
}

/* Inbox/picker/Rally's own fade height, unchanged from before this file's
 * PR #149 round 3 — only the THREAD call site (inbox_build_thread) needs
 * a shorter fade now (see FF_INBOX_THREAD_FADE_H's own comment). */
static void inbox_build_bottom_fade(lv_obj_t *parent, int32_t bot_y)
{
    inbox_build_bottom_fade_h(parent, bot_y, 40);
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
#define FF_INBOX_FAB_RING_T 96

static void inbox_build_fab(lv_obj_t *parent)
{
    /* Holder for the deco stack (disc under mask), full-puck, inert. */
    lv_obj_t *glass = lv_obj_create(parent);
    inbox_child_deco(glass);
    lv_obj_set_size(glass, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_set_pos(glass, 0, 0);
    lv_obj_set_style_bg_opa(glass, LV_OPA_TRANSP, 0);

    lv_obj_t *disc = lv_obj_create(glass);
    inbox_child_deco(disc);
    lv_obj_set_size(disc, FF_INBOX_FAB_DECO_D, FF_INBOX_FAB_DECO_D);
    lv_obj_set_pos(disc, FF_INBOX_FAB_DECO_X, FF_INBOX_FAB_DECO_Y);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);

    /* The whole-shape PRESS DIM (the maintainer's "the touchdown should
     * fill the entire shape"): a dark disc EXACTLY coincident with the
     * amber deco disc, transparent at rest, lit to a dark wash by the tap
     * target's press/release (wired below via inbox_row_press_ev — the
     * full-row-highlight precedent). Drawn AFTER the amber disc so it dims
     * it, and BEFORE the rim mask so its off-glass excess is masked back to
     * black exactly like the disc's — so the ENTIRE visible amber lens
     * dims on touch-down, not just the hit rect's own square (the old
     * LV_STATE_PRESSED on `hit` left the amber above y=300 / the top arc
     * undimmed, which is what read as "not the whole shape"). */
    lv_obj_t *press_dim = lv_obj_create(glass);
    inbox_child_deco(press_dim);
    lv_obj_set_size(press_dim, FF_INBOX_FAB_DECO_D, FF_INBOX_FAB_DECO_D);
    lv_obj_set_pos(press_dim, FF_INBOX_FAB_DECO_X, FF_INBOX_FAB_DECO_Y);
    lv_obj_set_style_radius(press_dim, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(press_dim, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(press_dim, LV_OPA_TRANSP, 0);

    /* The rim mask (see the section comment): a border-only circle whose
     * inner edge is the glass circle, centered on the puck. Drawn AFTER
     * the disc, so it paints the disc's off-glass spill back to the
     * letterbox black the sim/device frame shows outside the glass. */
    lv_obj_t *ring = lv_obj_create(glass);
    inbox_child_deco(ring);
    lv_obj_set_size(ring, FF_THEME_PUCK_PX + 2 * FF_INBOX_FAB_RING_T,
                    FF_THEME_PUCK_PX + 2 * FF_INBOX_FAB_RING_T);
    lv_obj_set_pos(ring, -FF_INBOX_FAB_RING_T, -FF_INBOX_FAB_RING_T);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, FF_INBOX_FAB_RING_T, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0x000000), 0); /* the letterbox black outside the glass */
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);

    /* The + glyph, dark ink, fully on-glass (drawn after the mask),
     * centered on the TAP TARGET's center (FF_INBOX_FAB_GLYPH_C — the
     * slice-b UX nit: the glyph is the aim point, so it marks where the
     * tap registers, not the deco disc's centroid). */
    lv_obj_t *bar_h = lv_obj_create(glass);
    inbox_child_deco(bar_h);
    lv_obj_set_size(bar_h, 26, 4);
    lv_obj_set_pos(bar_h, FF_INBOX_FAB_GLYPH_C - 13, FF_INBOX_FAB_GLYPH_C - 2);
    lv_obj_set_style_radius(bar_h, 2, 0);
    lv_obj_set_style_bg_color(bar_h, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(bar_h, LV_OPA_COVER, 0);
    lv_obj_t *bar_v = lv_obj_create(glass);
    inbox_child_deco(bar_v);
    lv_obj_set_size(bar_v, 4, 26);
    lv_obj_set_pos(bar_v, FF_INBOX_FAB_GLYPH_C - 2, FF_INBOX_FAB_GLYPH_C - 13);
    lv_obj_set_style_radius(bar_v, 2, 0);
    lv_obj_set_style_bg_color(bar_v, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(bar_v, LV_OPA_COVER, 0);

    /* The tap target — corner-anchored, covering the whole visible amber
     * lens (see the FAB constants). Drawn AFTER the deco stack (later
     * sibling = on top), so it doubles as the full-slice PRESS OVERLAY: a
     * dark LV_STATE_PRESSED wash (the composer SEND precedent — dim an
     * already-amber control rather than light amber-on-amber) that now
     * spans the ENTIRE visible amber, not a 48px patch of it. Square (no
     * radius): the wash is transparent except where it overlaps amber, so
     * only the lens visibly dims; the off-glass corner it also covers is
     * the masked letterbox black and stays black. */
    lv_obj_t *hit = lv_button_create(parent);
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, FF_INBOX_FAB_HIT_PX, FF_INBOX_FAB_HIT_PX);
    lv_obj_set_pos(hit, FF_INBOX_FAB_HIT_X, FF_INBOX_FAB_HIT_Y);
    lv_obj_set_style_radius(hit, 0, 0);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
    /* The press wash is now on the whole amber disc (press_dim above),
     * lit by this tap target's PRESSED/RELEASED — not a wash on the hit
     * rect's own square (which stopped short of the amber's top arc). */
    lv_obj_add_event_cb(hit, inbox_fab_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(hit, inbox_row_press_ev, LV_EVENT_PRESSED, press_dim);
    lv_obj_add_event_cb(hit, inbox_row_press_ev, LV_EVENT_RELEASED, press_dim);
    lv_obj_add_event_cb(hit, inbox_row_press_ev, LV_EVENT_PRESS_LOST, press_dim);
}

/* ---------------------------------------------------------------------
 * Sub-view: INBOX.
 * ------------------------------------------------------------------- */

/* S26 slice e renames (2026-09-01): "Signals" -> "Inbox" everywhere the
 * user can read it (see the PR body for the full list of renamed
 * strings). This header title is the primary occurrence; the screen
 * file, code identifiers, fixtures and test names stay "signals" —
 * that mechanical rename is explicitly out of scope (a separate PR if
 * wanted).
 *
 * ROUND 2 (reviewer PR #144, same date): "signal" reads as confusing
 * jargon to the person holding the puck everywhere they read it on
 * THIS screen, not only in the header title — reversing this file's
 * earlier "signal is domain vocabulary, not the screen name" call.
 * Every user-visible "SIGNAL"/"Signal" string in this file is now
 * "MESSAGE" instead: "NEW SIGNAL" -> "NEW MESSAGE", "NO SIGNALS YET"
 * -> "NO MESSAGES YET" (both occurrences), and inbox_kind_word's
 * default fallback "SIGNAL" -> "MESSAGE". ff_fmt_age's "now" (an age
 * under a minute) is a different word in a different sense and is
 * untouched, per CLAUDE.md/AGENTS.md's standing "now" caution. */
static void inbox_build_header(lv_obj_t *parent, uint16_t unread)
{
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "INBOX");
    lv_obj_set_style_text_font(title, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, unread > 0 ? -16 : 0, FF_INBOX_HEADER_Y);

    if (unread > 0) {
        inbox_build_badge(parent, unread, LV_ALIGN_TOP_MID, 42, FF_INBOX_HEADER_Y - 3);
    }
}

/* Honest no-crew hint (S24 AC9): shown beneath the CREW row when the
 * model holds no member conversations at all — never a blank face, and
 * pairing itself stays out of scope (the S22 note stands). */
static void inbox_build_no_crew_hint(lv_obj_t *list, int32_t y, int32_t margin_x)
{
    lv_obj_t *box = lv_obj_create(list);
    inbox_child_deco(box);
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

static void inbox_build_inbox(lv_obj_t *parent, ff_app_inbox_t const *v, bool colorblind)
{
    inbox_build_header(parent, ff_scr_inbox_unread_count(v));

    /* Bottom-anchored scroll list — full puck width so the scroll
     * gesture isn't narrowed; rows are inset to the list's own
     * chord-derived margin (computed once against the viewport band's
     * worst-case edge). Clips its children, so overflow rows are
     * reachable by scroll and never paint past the viewport. */
    int32_t list_margin = inbox_safe_margin_x(FF_INBOX_LIST_TOP_Y, FF_INBOX_LIST_H);

    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, FF_INBOX_LIST_TOP_Y);
    lv_obj_set_size(list, FF_THEME_PUCK_PX, FF_INBOX_LIST_H);
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
        inbox_build_conv_row(list, cv, y, list_margin, colorblind);
        y += FF_INBOX_ROW_H;
    }

    /* No paired members at all (only the ever-present CREW row): say so
     * honestly right in the list band (AC9). */
    if (n <= 1) {
        inbox_build_no_crew_hint(list, y, list_margin);
    }

    inbox_build_bottom_fade(parent, FF_INBOX_LIST_BOT_Y);
    inbox_build_fab(parent);
}

/* ---------------------------------------------------------------------
 * Sub-view: RECIPIENT PICKER (the FAB's scope step).
 * ------------------------------------------------------------------- */

#define FF_INBOX_PICKER_LIST_TOP_Y 82
#define FF_INBOX_PICKER_LIST_H     260

static void inbox_build_picker(lv_obj_t *parent, ff_app_inbox_t const *v, bool colorblind)
{
    inbox_build_back(parent);

    /* Caption sits to the back button's RIGHT (a centered caption would
     * collide with the button at this band's chord margin — seen in the
     * first render, not theorized). */
    int32_t back_margin = inbox_safe_margin_x(FF_INBOX_BACK_Y, FF_INBOX_BACK_PX);
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "NEW MESSAGE");
    lv_obj_set_style_text_font(title, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_set_pos(title, back_margin + FF_INBOX_BACK_PX + 14, FF_INBOX_BACK_Y + 15);

    int32_t list_margin = inbox_safe_margin_x(FF_INBOX_PICKER_LIST_TOP_Y, FF_INBOX_PICKER_LIST_H);

    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, FF_INBOX_PICKER_LIST_TOP_Y);
    lv_obj_set_size(list, FF_THEME_PUCK_PX, FF_INBOX_PICKER_LIST_H);
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

            lv_obj_t *row = inbox_row_container(list, y, list_margin, cv->node_id, inbox_pick_cb, 0);
            int32_t row_w = FF_THEME_PUCK_PX - 2 * list_margin;

            if (is_crew) {
                inbox_build_crew_avatar(row, 8, colorblind);
            } else {
                inbox_build_member_avatar(row, 8, cv->initial, cv->color_idx, colorblind);
            }

            int32_t text_x = 8 + FF_INBOX_AVATAR_PX + 12;
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
                inbox_presence_text(cv, line, sizeof(line), &line_color);
            }
            lv_obj_t *sub = lv_label_create(row);
            lv_label_set_text(sub, line);
            lv_obj_set_style_text_font(sub, FF_THEME_FONT_CHIP, 0);
            lv_obj_set_style_text_color(sub, lv_color_hex(line_color), 0);
            lv_obj_set_pos(sub, text_x, 38);

            y += FF_INBOX_ROW_H;
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
 * inbox_msg_event_text), and names a sender ONLY when the model joined
 * one (`identity_known`; an unjoined sender renders an honest "UNKNOWN",
 * never a guessed name). Bubbles are DECO — not individually tappable —
 * so the message list has no PER-ROW hit-rects to collide with the
 * chips/FAB at any scroll offset. `list` itself still needs exactly ONE
 * clickable surface for touch to ever resolve a drag to its LV_DIR_VER
 * scroll at all (LVGL's hit-test requires CLICKABLE on SOMETHING under
 * the finger): a single invisible hit target the full height of the
 * message content, scrolled right along with it, added after the loop
 * below — see its own comment for why one full-content overlay (not
 * per-row, not the list container itself) is what keeps the hit-target
 * sweep's circle-containment AND adjacency checks passing at every
 * scroll offset. Long content ellipsizes (fixed row pitch; DOTS needs
 * bounded width AND height, the slice-b lesson).
 * ------------------------------------------------------------------- */

/* inbox_thread_conv — the open conversation's row in the inbox model
 * (header presence / identity), or NULL (e.g. a fixture that authors no
 * convs — every dependent element renders honestly absent). */
static ff_inbox_conv_t const *inbox_thread_conv(ff_app_inbox_t const *v)
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
static char const *inbox_msg_sender_name(ff_inbox_msg_t const *m)
{
    if (!m->identity_known) return "UNKNOWN";
    return (m->name[0] != '\0') ? m->name : "MEMBER";
}

/* A small label helper: font + color + text, returns the label. */
static lv_obj_t *inbox_mk_label(lv_obj_t *parent, char const *text, lv_font_t const *font,
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
static int32_t inbox_label_clamp(lv_obj_t *label, int32_t max_w)
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
static void inbox_msg_sender_line(lv_obj_t *row, ff_inbox_msg_t const *m, char const *age,
                                    bool colorblind)
{
    uint32_t const name_color =
        m->identity_known ? ff_theme_crew_color(m->color_idx, colorblind) : FF_THEME_COLOR_MUTED;

    if (m->identity_known) {
        lv_obj_t *dot = lv_obj_create(row);
        inbox_child_deco(dot);
        lv_obj_set_size(dot, 7, 7);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(name_color), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_pos(dot, 0, 5);
    }
    int32_t x = m->identity_known ? 13 : 0;

    lv_obj_t *name = inbox_mk_label(row, inbox_msg_sender_name(m), FF_THEME_FONT_CHIP, name_color);
    lv_obj_set_pos(name, x, 0);
    x += inbox_label_clamp(name, 120) + 8;

    lv_obj_t *age_l = inbox_mk_label(row, age, FF_THEME_FONT_CHIP, FF_THEME_COLOR_DIM);
    lv_obj_set_pos(age_l, x, 1);
}

/* The mono age line under a bubble (rows with no sender line above). */
static void inbox_msg_age_below(lv_obj_t *row, char const *age, int32_t y, bool out, int32_t row_w)
{
    lv_obj_t *l = inbox_mk_label(row, age, FF_THEME_FONT_CHIP, FF_THEME_COLOR_DIM);
    lv_obj_update_layout(l);
    lv_obj_set_pos(l, out ? (row_w - lv_obj_get_width(l) - 4) : 4, y);
}

/* A one-line bubble (TEXT / STATUS / the 1:1 pulse callout / an OUT
 * pulse): dark surface for theirs, solid amber with dark ink for mine.
 * Returns the bubble container (its content laid out by the caller when
 * `text` is NULL). */
static lv_obj_t *inbox_msg_bubble(lv_obj_t *row, char const *text, int32_t y, bool out,
                                    int32_t row_w)
{
    lv_obj_t *bub = lv_obj_create(row);
    inbox_child_deco(bub);
    lv_obj_set_style_radius(bub, 14, 0); /* design canvas ThreadGroup/ThreadPerson bubble radius */
    lv_obj_set_style_bg_color(bub, lv_color_hex(out ? FF_THEME_COLOR_AMBER : FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(bub, LV_OPA_COVER, 0);

    int32_t w = FF_INBOX_MSG_MAX_W;
    if (text != NULL) {
        lv_obj_t *l = inbox_mk_label(bub, text, FF_THEME_FONT_MSG_BODY,
                                       out ? FF_THEME_COLOR_BG : FF_THEME_COLOR_INK);
        int32_t const tw = inbox_label_clamp(l, FF_INBOX_MSG_MAX_W - 26);
        lv_obj_set_pos(l, 13, 8);
        w = tw + 26;
    }
    lv_obj_set_size(bub, w, FF_INBOX_MSG_BUBBLE_H);
    lv_obj_set_pos(bub, out ? (row_w - w) : 0, y);
    return bub;
}

/* The RALLY callout: violet-tinted box, RALLY caption + place name. */
static void inbox_msg_rally(lv_obj_t *row, ff_inbox_msg_t const *m, int32_t y, bool out,
                              int32_t row_w)
{
    lv_obj_t *box = lv_obj_create(row);
    inbox_child_deco(box);
    lv_obj_set_style_radius(box, 14, 0); /* design canvas rally-callout radius */
    lv_obj_set_style_bg_color(box, lv_color_hex(FF_THEME_CREW_VIOLET), 0);
    lv_obj_set_style_bg_opa(box, 30, 0); /* ~12% — the canvas's violet wash */
    lv_obj_set_style_border_color(box, lv_color_hex(FF_THEME_CREW_VIOLET), 0);
    lv_obj_set_style_border_opa(box, LV_OPA_30, 0);
    lv_obj_set_style_border_width(box, 1, 0);

    lv_obj_t *cap = inbox_mk_label(box, "RALLY", FF_THEME_FONT_CHIP, FF_THEME_CREW_VIOLET);
    lv_obj_set_style_text_letter_space(cap, 2, 0);
    lv_obj_set_pos(cap, 12, 5);

    /* The place is the wire name verbatim (m->text); "" renders as the
     * honest absence of a place claim, never an invented one. */
    lv_obj_t *place = inbox_mk_label(box, m->text, FF_THEME_FONT_MSG_BODY, FF_THEME_COLOR_INK);
    int32_t const pw = inbox_label_clamp(place, FF_INBOX_MSG_MAX_W - 24);
    lv_obj_set_pos(place, 12, 23);

    int32_t w = pw + 24;
    if (w < 96) w = 96; /* never narrower than its own RALLY caption */
    lv_obj_set_size(box, w, FF_INBOX_MSG_RALLY_H);
    lv_obj_set_pos(box, out ? (row_w - w) : 0, y);
}

/* Honest event wording for PULSE/FLARE — the address half of the
 * sentence comes from the DIRECTION FACT, never from which thread is
 * showing: a BROADCAST pulse "pulsed the crew", a DIRECT one "pulsed
 * you", an OUT one names my own act, and an UNKNOWN direction claims no
 * address at all (bare "pulsed"/"flared"). */
static char const *inbox_msg_event_text(ff_inbox_msg_t const *m, bool crew_thread)
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
static void inbox_msg_pulse_mark(lv_obj_t *parent, int32_t x, int32_t y)
{
    lv_obj_t *ring = lv_obj_create(parent);
    inbox_child_deco(ring);
    lv_obj_set_size(ring, 13, 13);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_60, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_pos(ring, x, y);

    lv_obj_t *dot = lv_obj_create(parent);
    inbox_child_deco(dot);
    lv_obj_set_size(dot, 5, 5);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(dot, x + 4, y + 4);
}

/* An inbound CREW-thread event one-liner (pulse/flare): crew dot + mark
 * + "<NAME> <event>" + age at the right. */
static void inbox_msg_event_line(lv_obj_t *row, ff_inbox_msg_t const *m, char const *age,
                                   bool colorblind, int32_t row_w)
{
    uint32_t const name_color =
        m->identity_known ? ff_theme_crew_color(m->color_idx, colorblind) : FF_THEME_COLOR_MUTED;

    int32_t x = 0;
    if (m->identity_known) {
        lv_obj_t *dot = lv_obj_create(row);
        inbox_child_deco(dot);
        lv_obj_set_size(dot, 7, 7);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(name_color), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_pos(dot, 0, 7);
        x = 13;
    }
    if (m->kind == FEED_PULSE) {
        inbox_msg_pulse_mark(row, x, 4);
        x += 19;
    }
    lv_obj_t *name = inbox_mk_label(row, inbox_msg_sender_name(m), FF_THEME_FONT_CHIP, name_color);
    lv_obj_set_pos(name, x, 1);
    x += inbox_label_clamp(name, 110) + 6;

    lv_obj_t *what = inbox_mk_label(row, inbox_msg_event_text(m, true), FF_THEME_FONT_CHIP,
                                      (m->kind == FEED_FLARE) ? FF_THEME_COLOR_STALE_AMBER
                                                              : FF_THEME_COLOR_INK);
    lv_obj_set_pos(what, x, 1);
    (void)inbox_label_clamp(what, row_w - x - 58);

    lv_obj_t *age_l = inbox_mk_label(row, age, FF_THEME_FONT_CHIP, FF_THEME_COLOR_DIM);
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
static int32_t inbox_build_msg(lv_obj_t *list, ff_inbox_msg_t const *m, int32_t y,
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
        content_h = FF_INBOX_MSG_RALLY_H;
        break;
    case FEED_PULSE:
    case FEED_FLARE:
        if (sender_line) {
            event_line = true;
            content_h = FF_INBOX_MSG_EVENT_H;
        } else {
            content_h = FF_INBOX_MSG_BUBBLE_H;
        }
        break;
    default: /* TEXT / STATUS */
        content_h = FF_INBOX_MSG_BUBBLE_H;
        break;
    }
    int32_t const head_h = (sender_line && !event_line) ? FF_INBOX_MSG_SENDER_H : 0;
    int32_t const tail_h = (!sender_line) ? FF_INBOX_MSG_AGE_H : 0;
    int32_t const row_h = head_h + content_h + tail_h;

    lv_obj_t *row = lv_obj_create(list);
    inbox_child_deco(row);
    lv_obj_set_size(row, row_w, row_h);
    lv_obj_set_pos(row, margin_x, y);

    if (event_line) {
        /* CREW inbound pulse/flare: single line with the sender named. */
        inbox_msg_event_line(row, m, age, colorblind, row_w);
        return row_h + FF_INBOX_MSG_GAP;
    }

    if (sender_line) {
        inbox_msg_sender_line(row, m, age, colorblind);
    }

    switch (m->kind) {
    case FEED_RALLY:
        inbox_msg_rally(row, m, head_h, out, row_w);
        break;
    case FEED_PULSE:
    case FEED_FLARE: {
        /* 1:1 pulse callout / any OUT pulse/flare: a bubble carrying the
         * honest event sentence (inbound names the sender; OUT names my
         * own act). */
        lv_obj_t *bub = inbox_msg_bubble(row, NULL, head_h, out, row_w);
        int32_t x = 13;
        if (m->kind == FEED_PULSE) {
            inbox_msg_pulse_mark(bub, x, 10);
            x += 19;
        }
        uint32_t const ink = out ? FF_THEME_COLOR_BG : FF_THEME_COLOR_INK;
        int32_t w;
        if (!out) {
            uint32_t const name_color = m->identity_known
                                            ? ff_theme_crew_color(m->color_idx, colorblind)
                                            : FF_THEME_COLOR_MUTED;
            lv_obj_t *name = inbox_mk_label(bub, inbox_msg_sender_name(m), FF_THEME_FONT_MSG_BODY,
                                              name_color);
            lv_obj_set_pos(name, x, 8);
            x += inbox_label_clamp(name, 100) + 6;
            lv_obj_t *what = inbox_mk_label(bub, inbox_msg_event_text(m, crew_thread),
                                              FF_THEME_FONT_MSG_BODY, ink);
            lv_obj_set_pos(what, x, 8);
            w = x + inbox_label_clamp(what, FF_INBOX_MSG_MAX_W - x - 13) + 13;
        } else {
            lv_obj_t *what = inbox_mk_label(bub, inbox_msg_event_text(m, crew_thread),
                                              FF_THEME_FONT_MSG_BODY, ink);
            lv_obj_set_pos(what, x, 8);
            w = x + inbox_label_clamp(what, FF_INBOX_MSG_MAX_W - x - 13) + 13;
        }
        lv_obj_set_size(bub, w, FF_INBOX_MSG_BUBBLE_H);
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
        (void)inbox_msg_bubble(row, line, head_h, out, row_w);
        break;
    }
    }

    if (!sender_line) {
        inbox_msg_age_below(row, age, head_h + content_h + 2, out, row_w);
    }
    return row_h + FF_INBOX_MSG_GAP;
}

/* The thread header. CREW: centered "CREW" + the roster fact "N CREW"
 * (paired members = the model's member conversations — a roster count,
 * never a present-tense "N here"). 1:1: crew dot + name, and the honest
 * presence line ("SEEN <age>" stale-amber / LOST / LINKED) beneath —
 * the legible stale tier, same formatter as the inbox rows. */
static void inbox_build_thread_header(lv_obj_t *parent, ff_app_inbox_t const *v,
                                        ff_inbox_conv_t const *cv, bool colorblind)
{
    if (v->thread_node == 0u) {
        lv_obj_t *title = inbox_mk_label(parent, "CREW", FF_THEME_FONT_HEADLINE, FF_THEME_COLOR_INK);
        lv_obj_set_style_text_letter_space(title, 1, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

        /* Member conversations == paired members (ff_inbox: CREW + one
         * per paired member) — a roster fact read off the model, not a
         * second source. */
        uint8_t const n = ff_inbox_conv_count(&v->inbox);
        char buf[16];
        snprintf(buf, sizeof(buf), "%u CREW", (n > 0u) ? (unsigned)(n - 1u) : 0u);
        lv_obj_t *sub = inbox_mk_label(parent, buf, FF_THEME_FONT_CHIP, FF_THEME_COLOR_DIM);
        lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 58);
        return;
    }

    char const *name_txt = (v->thread_name[0] != '\0') ? v->thread_name : "MEMBER";
    lv_obj_t *name = inbox_mk_label(parent, name_txt, FF_THEME_FONT_HEADLINE, FF_THEME_COLOR_INK);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 8, 34);
    /* Ellipsize a long name, then hug the crew dot to the MEASURED text
     * edge (a fixed offset either collides with the back button or
     * floats detached on a short name — measured, not guessed). */
    int32_t const name_w = inbox_label_clamp(name, 140);

    lv_obj_t *dot = lv_obj_create(parent);
    inbox_child_deco(dot);
    lv_obj_set_size(dot, 9, 9);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(ff_theme_crew_color(v->thread_color_idx, colorblind)), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 8 - name_w / 2 - 12, 40);

    if (cv != NULL && cv->presence_valid) {
        char pres[32];
        uint32_t pres_color = FF_THEME_COLOR_MUTED;
        inbox_presence_text(cv, pres, sizeof(pres), &pres_color);
        lv_obj_t *pl = inbox_mk_label(parent, pres, FF_THEME_FONT_CHIP, pres_color);
        lv_obj_align(pl, LV_ALIGN_TOP_MID, 0, 58);
    }
}

/* One quick chip. `which` rides user_data for the canned replies;
 * `amber_text` marks the FLARE chip's accent. */
static void inbox_build_chip(lv_obj_t *parent, int32_t x, int32_t w, char const *text,
                               bool amber_text, lv_event_cb_t cb, uintptr_t which)
{
    lv_obj_t *chip = lv_button_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, w, FF_INBOX_CHIP_H);
    lv_obj_set_pos(chip, x, FF_INBOX_CHIP_Y);
    lv_obj_set_style_radius(chip, FF_INBOX_CHIP_H / 2, 0);
    lv_obj_set_style_bg_color(chip, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(chip, cb, LV_EVENT_CLICKED, (void *)which);
    inbox_press_feedback(chip);

    lv_obj_t *l = inbox_mk_label(chip, text, FF_THEME_FONT_CHIP,
                                   amber_text ? FF_THEME_COLOR_AMBER : FF_THEME_COLOR_INK);
    lv_obj_center(l);
}

/* The 1:1 quick-chip strip: OMW / IN 5 MIN / FLARE, centered in the band
 * left of the FAB clearance (FF_INBOX_CHIP_MAX_RIGHT). */
static void inbox_build_chips(lv_obj_t *parent)
{
    static const int32_t w_omw = 66, w_5min = 96, w_flare = 74;
    int32_t const margin = inbox_safe_margin_x(FF_INBOX_CHIP_Y, FF_INBOX_CHIP_H);
    int32_t const strip_w = w_omw + w_5min + w_flare + 2 * FF_INBOX_CHIP_GAP;
    int32_t const avail = FF_INBOX_CHIP_MAX_RIGHT - margin;
    int32_t x = margin + (avail > strip_w ? (avail - strip_w) / 2 : 0);

    inbox_build_chip(parent, x, w_omw, "OMW", false, inbox_chip_reply_cb,
                       (uintptr_t)FF_WIRING_REPLY_OMW);
    x += w_omw + FF_INBOX_CHIP_GAP;
    inbox_build_chip(parent, x, w_5min, "IN 5 MIN", false, inbox_chip_reply_cb,
                       (uintptr_t)FF_WIRING_REPLY_5MIN);
    x += w_5min + FF_INBOX_CHIP_GAP;
    inbox_build_chip(parent, x, w_flare, "FLARE", true, inbox_chip_flare_cb, 0u);
}

/* ---------------------------------------------------------------------
 * Thread scroll-position preservation across the S24 render-key rebuild
 * (ff_shell.c's render-key doc comment: an age-bucket crossing is the
 * spec's own legitimate rebuild trigger for a live thread — it does
 * lv_obj_clean + a fresh inbox_build_thread). Without this, a user
 * scrolled up mid-read was silently snapped back to newest on the very
 * next age tick. Mirrors scr_settings.c's s_list/s_scroll_y precedent
 * (#bug4) — one LV_EVENT_SCROLL callback writes back a file-static
 * offset, restored on the next build — but keyed on WHICH thread and
 * HOW MANY messages it held, not just "the list exists": unlike
 * Settings, a Signals rebuild can also mean a different thread opened,
 * or a new message arrived in this one, and both of those must land on
 * newest, never a stale offset. `node_id` alone is enough of a key
 * because 0 is CREW's own sentinel (never a real member id) throughout
 * this file (`crew_thread = (v->thread_node == 0u)` above), so there is
 * no separate CREW/member ambiguity to carry a second field for.
 * ------------------------------------------------------------------- */
static uint32_t s_thread_scroll_node_id;
static uint8_t  s_thread_scroll_msg_count;
static int32_t  s_thread_scroll_y;
static bool     s_thread_scroll_valid;

/* LV_EVENT_SCROLL — captured live WHILE the user scrolls: the render loop
 * that triggers a rebuild does lv_obj_clean() first, destroying `list`
 * and any chance to read its scroll offset AFTER the fact, so this is
 * the only point the offset can ever be observed. */
static void inbox_thread_scroll_cb(lv_event_t *e)
{
    lv_obj_t *list = lv_event_get_target(e);
    s_thread_scroll_y = lv_obj_get_scroll_y(list);
}

static void inbox_build_thread(lv_obj_t *parent, ff_app_inbox_t const *v, bool colorblind)
{
    bool const crew_thread = (v->thread_node == 0u);
    ff_inbox_conv_t const *cv = inbox_thread_conv(v);

    inbox_build_back(parent);
    inbox_build_thread_header(parent, v, cv, colorblind);

    int32_t const list_h = crew_thread ? FF_INBOX_THREAD_LIST_H_CREW : FF_INBOX_THREAD_LIST_H_1TO1;
    int32_t const list_margin = inbox_safe_margin_x(FF_INBOX_THREAD_LIST_TOP_Y, list_h);

    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, FF_INBOX_THREAD_LIST_TOP_Y);
    lv_obj_set_size(list, FF_THEME_PUCK_PX, list_h);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    /* Bottom padding: the scroll-to-newest below parks the LAST message
     * this far above the hard clip, so the newest signal reads at full
     * strength instead of dying under the fade. CREW only (PR #149 review
     * round 3) — 1:1's list never approaches the FAB (it stops well above
     * the chip strip) and round 2 already found 0 pad was the right call
     * there (needed for its own rows-visible AC); only CREW's newest-row-
     * under-the-fade defect needs this back. */
    lv_obj_set_style_pad_bottom(list, crew_thread ? 0 : 0, 0);
    /* Observe the live scroll offset so the NEXT rebuild (see below) can
     * restore it instead of always snapping to newest. */
    lv_obj_add_event_cb(list, inbox_thread_scroll_cb, LV_EVENT_SCROLL, NULL);

    uint8_t const n = ff_inbox_thread_count(&v->thread);
    int32_t y = 0;
    for (uint8_t i = 0; i < n; i++) {
        ff_inbox_msg_t const *m = ff_inbox_thread_at(&v->thread, i);
        if (m == NULL) continue;
        y += inbox_build_msg(list, m, y, list_margin, crew_thread, colorblind);
    }

    if (n == 0) {
        /* Honest empty thread (a quiet member / a traffic-less crew).
         * Renamed 2026-09-01, was "NO SIGNALS YET" (reviewer PR #144
         * round 2). */
        lv_obj_t *empty = inbox_mk_label(list, "NO MESSAGES YET", FF_THEME_FONT_CHIP,
                                           FF_THEME_COLOR_DIM);
        lv_obj_set_style_text_letter_space(empty, 2, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, -20);
    }

    /* Compute overflow BEFORE the scroll-to-newest below needs it, so the
     * scroll touch target (next) can also gate on it: whether content
     * overflows is knowable only once the loop above has actually laid
     * out every row. */
    lv_obj_update_layout(list);
    int32_t const overflow = lv_obj_get_scroll_bottom(list);

    if (overflow > 0) {
        /* The scroll touch target (root-cause fix for "CREW thread
         * messages don't scroll", confirmed also true of 1:1 before this
         * fix): message bubbles are pure deco (no CLICKABLE flag), and
         * `list` itself deliberately isn't CLICKABLE either (matching the
         * inbox/picker lists' own convention) — LVGL's `lv_indev_search_obj`
         * only ever resolves a touch to an object carrying LV_OBJ_FLAG_
         * CLICKABLE (lv_obj_hit_test requires it), so with NOTHING
         * clickable anywhere under `list`, a press over the message area
         * resolved to no object inside it at all: it fell through to a
         * non-scrollable ancestor, and a drag never reached `list`'s own
         * LV_DIR_VER scroll. Gated on genuine overflow (not just "any
         * messages") for two reasons: a short thread has nothing to
         * scroll to in the first place, and — measured, not theorized —
         * `banner_on_thread.json` renders a short (non-overflowing) 1:1
         * thread with the S26(d) message banner up top; an unconditional
         * hit target would still claim the full list viewport and the
         * hit-target sweep (correctly) flags it against the banner's own
         * tap target, two independently-clickable siblings (not an
         * ancestor/descendant pair the sweep already exempts) that
         * visually overlap. No overflow, no hit target, no collision —
         * and nothing is lost, since a non-overflowing thread was never
         * reachable-by-scroll to begin with.
         *
         * Two shapes were tried and measured to fail before this one:
         * (1) a hit-rect PER ROW — a CREW inbound pulse/flare event line
         * is only 22px tall, under the 44px hit floor, and a row scrolled
         * to straddle the viewport's top edge still carries its FULL
         * un-clipped rect, which the hit-target sweep's adjacency check
         * then measures against the pinned back button above the list
         * (on this screen's own newest-at-bottom auto-scroll that
         * straddle is the common case, not an edge case); (2) ONE
         * hit-rect spanning the full scrolled CONTENT height — tall
         * enough to always cover the viewport at any scroll offset, but
         * for the SAME reason it necessarily passes straight through
         * y=0 on its way from a (scrolled-to-newest) deeply negative top
         * to a positive bottom, so its raw rect always overlaps the
         * header sitting at y<TOP_Y, regardless of scroll position.
         *
         * The fix: ONE hit-rect sized to `list`'s own VIEWPORT (not the
         * scrolled content) and marked LV_OBJ_FLAG_FLOATING — LVGL's
         * "do not scroll the object when the parent scrolls" flag — so
         * its absolute on-screen rect stays fixed at exactly
         * (list_margin, TOP_Y)-(PUCK_PX - list_margin, TOP_Y + list_h)
         * no matter how far `list` is scrolled. That rect already
         * respects `list_margin` — the same safe margin every message
         * row in this band uses — so it passes circle-containment
         * directly, with no need for the sweep's S21 viewport
         * adjustment; and being fixed, its distance from the header/FAB
         * is the same constant clearance the list's own static geometry
         * already guarantees, never a scroll-dependent straddle. No
         * CLICKED callback is ever bound to it, so it adds no tap
         * behavior of its own; LVGL's scroll processing walks UP from
         * whatever it resolves a press to, finds `list` as the nearest
         * LV_DIR_VER-scrollable ancestor (FLOATING does not remove it
         * from that ancestor walk, only from scroll-following and
         * layout), and scrolls that. */
        lv_obj_t *hit = lv_obj_create(list);
        lv_obj_remove_style_all(hit);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_FLOATING);
        lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
        lv_obj_set_pos(hit, list_margin, 0);
        lv_obj_set_size(hit, FF_THEME_PUCK_PX - 2 * list_margin, list_h);

        /* Scroll-position preservation (PR #143 review): a render-key
         * rebuild of the SAME thread, with the SAME message count, is
         * the user still reading — restore exactly where they were
         * (lv_obj_scroll_to_y clamps to the freshly-laid-out range, so a
         * stale offset from a since-shrunk thread can never overshoot).
         * Anything else — no prior state, a different thread_node (a
         * fresh open, or "switch to a different thread" per the S24
         * spec), or a changed message count (a new message arrived, or
         * the thread got marked/joined differently) — lands on newest,
         * exactly as before this fix. `list_margin`/list_h/the FLOATING
         * hit target above are unaffected either way: the touch target
         * stays pinned to the viewport regardless of which scroll
         * offset ends up restored. */
        bool const same_thread_state = s_thread_scroll_valid && (s_thread_scroll_node_id == v->thread_node) &&
                                       (s_thread_scroll_msg_count == n);
        if (same_thread_state) {
            lv_obj_scroll_to_y(list, s_thread_scroll_y, LV_ANIM_OFF);
        } else {
            lv_obj_scroll_to_y(list, overflow, LV_ANIM_OFF); /* newest at the bottom */
        }
    }

    /* Remember this build's identity for the NEXT rebuild's same-thread-
     * state check above (recorded unconditionally, including a non-
     * overflowing thread, so a later message that pushes it over the
     * overflow threshold is correctly seen as "count changed" rather
     * than "no prior state"). */
    s_thread_scroll_node_id = v->thread_node;
    s_thread_scroll_msg_count = n;
    s_thread_scroll_valid = true;

    /* PR #149 review round 3: a 40px fade (the inbox/Rally default) would
     * dim most or all of the newest message, which must read at FULL
     * strength ("the newest message is the one that must never be
     * hidden") — FF_INBOX_THREAD_FADE_H is a much shorter cue, just
     * enough to signal "content continues below" without touching the
     * newest row's own text (its natural height clears the fade given the
     * list's own bottom_pad above). */
    inbox_build_bottom_fade_h(parent, FF_INBOX_THREAD_LIST_TOP_Y + list_h, FF_INBOX_THREAD_FADE_H);
    if (!crew_thread) {
        inbox_build_chips(parent);
    }
    inbox_build_fab(parent);
}

/* ---------------------------------------------------------------------
 * Sub-view: ACTION POPUP (S24 slice d) — over the (opaque-keyed) thread
 * scope. Three big color-coded action rows + a >=44px close. The thread
 * behind is NOT re-rendered here (the render key masks it, and the design
 * shows the popup on the puck bg with only a dimmed scope hint), so the
 * popup owns the screen with the scope stated at the top.
 * ------------------------------------------------------------------- */

/* One popup action row: a color-washed rounded button (title + subtitle)
 * with a colored left accent. `accent` tints the wash/border/accent; the
 * whole 280x66 row is the tap target (>=44px). */
static void inbox_build_popup_row(lv_obj_t *parent, int32_t y, uint32_t accent, char const *title,
                                    char const *sub, lv_event_cb_t cb, char const *symbol)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, FF_INBOX_POPUP_ROW_W, FF_INBOX_POPUP_ROW_H);
    lv_obj_set_pos(row, FF_INBOX_POPUP_ROW_X, y);
    lv_obj_set_style_radius(row, 18, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_opa(row, 34, 0); /* ~13% wash, the canvas's rgba(accent,0.13) */
    lv_obj_set_style_border_color(row, lv_color_hex(accent), 0);
    lv_obj_set_style_border_opa(row, LV_OPA_40, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    /* Press feedback: deepen the accent wash on touch-down. */
    lv_obj_set_style_bg_opa(row, LV_OPA_60, LV_STATE_PRESSED);

    /* Colored accent glyph (the canvas's per-action icon), left. */
    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(accent), 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 20, 0);

    lv_obj_t *t = inbox_mk_label(row, title, FF_THEME_FONT_HEADLINE, FF_THEME_COLOR_INK);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 58, -9);
    lv_obj_t *s = inbox_mk_label(row, sub, FF_THEME_FONT_CHIP, FF_THEME_COLOR_MUTED);
    lv_obj_align(s, LV_ALIGN_LEFT_MID, 58, 12);
}

/* The centered >=44px circular close (pops the popup back to the thread —
 * FF_INTENT_BACK). */
static void inbox_build_popup_close(lv_obj_t *parent)
{
    lv_obj_t *close = lv_button_create(parent);
    lv_obj_remove_style_all(close);
    lv_obj_set_size(close, FF_INBOX_POPUP_CLOSE_PX, FF_INBOX_POPUP_CLOSE_PX);
    lv_obj_set_pos(close, (FF_THEME_PUCK_PX - FF_INBOX_POPUP_CLOSE_PX) / 2, FF_INBOX_POPUP_CLOSE_Y);
    lv_obj_set_style_radius(close, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(close, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(close, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(close, inbox_back_cb, LV_EVENT_CLICKED, NULL);
    inbox_press_feedback(close);

    lv_obj_t *x = lv_label_create(close);
    lv_label_set_text(x, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(x, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(x, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_center(x);
}

static void inbox_build_popup(lv_obj_t *parent, ff_app_inbox_t const *v, bool colorblind)
{
    (void)colorblind;
    /* Scope hint + "SEND TO <scope>" (the popup states the scope; the
     * Rally screen then does NOT repeat it — the maintainer's note). */
    char const *scope = (v->thread_node == 0u) ? "CREW" : (v->thread_name[0] != '\0' ? v->thread_name : "MEMBER");

    lv_obj_t *hint = inbox_mk_label(parent, scope, FF_THEME_FONT_CHIP, FF_THEME_COLOR_MUTED);
    lv_obj_set_style_text_letter_space(hint, 1, 0);
    lv_obj_set_style_text_opa(hint, LV_OPA_40, 0); /* the dimmed thread-context cue */
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 34);

    char sendto[FF_APP_NAME_LEN + 12];
    snprintf(sendto, sizeof(sendto), "SEND TO %s", scope);
    lv_obj_t *title = inbox_mk_label(parent, sendto, FF_THEME_FONT_CHIP, FF_THEME_COLOR_MUTED);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    /* Compose (green) / Rally (violet) / Flare (amber), color-coded. The
     * third row is a FLARE ("come find me"), not a pulse — the maintainer's
     * "in send to crew we should have flare not pulse". No dedicated flare
     * glyph exists (the 8-ray flare mark is custom-drawn in scr_flare.c, not
     * a font glyph), so the come-find-me row takes LV_SYMBOL_EYE_OPEN ("spot
     * me") — distinct from Compose's EDIT and Rally's GPS. */
    inbox_build_popup_row(parent, FF_INBOX_POPUP_ROW1_Y, FF_THEME_CREW_GREEN, "Compose",
                            "write a message", inbox_popup_compose_cb, LV_SYMBOL_EDIT);
    inbox_build_popup_row(parent, FF_INBOX_POPUP_ROW2_Y, FF_THEME_CREW_VIOLET, "Rally",
                            "meet somewhere", inbox_popup_rally_cb, LV_SYMBOL_GPS);
    inbox_build_popup_row(parent, FF_INBOX_POPUP_ROW3_Y, FF_THEME_COLOR_AMBER, "Flare",
                            "come find me", inbox_popup_flare_cb, LV_SYMBOL_EYE_OPEN);

    inbox_build_popup_close(parent);
}

/* ---------------------------------------------------------------------
 * Sub-view: RALLY (S24 slice d) — WHERE radio list + WHEN chip + Send.
 * ------------------------------------------------------------------- */

/* One WHERE radio row inside the scroll list. `sel` draws the violet
 * selected treatment + a check; `enabled` false renders a disabled On Me
 * (honest reason, no tap target). `idx` rides to RALLY_SELECT_PLACE. */
static void inbox_build_rally_row(lv_obj_t *list, int32_t y, uint8_t idx, char const *name,
                                    char const *sub, bool selected, bool enabled)
{
    int32_t const margin = inbox_safe_margin_x(FF_INBOX_RALLY_LIST_TOP_Y, FF_INBOX_RALLY_LIST_H);
    int32_t const w = FF_THEME_PUCK_PX - 2 * margin;

    lv_obj_t *row = lv_obj_create(list);
    inbox_child_deco(row);
    lv_obj_set_size(row, w, FF_INBOX_RALLY_ROW_H);
    lv_obj_set_pos(row, margin, y);
    lv_obj_set_style_radius(row, 13, 0);
    if (selected) {
        lv_obj_set_style_bg_color(row, lv_color_hex(FF_THEME_CREW_VIOLET), 0);
        lv_obj_set_style_bg_opa(row, 40, 0); /* ~16% violet wash */
        lv_obj_set_style_border_color(row, lv_color_hex(FF_THEME_CREW_VIOLET), 0);
        lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 2, 0);
    } else {
        lv_obj_set_style_bg_color(row, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
        lv_obj_set_style_bg_opa(row, enabled ? LV_OPA_COVER : LV_OPA_50, 0);
    }

    /* The inset transparent tap target — created as the row's FIRST child
     * (index 0), the stable contract the touch test locates it by
     * (find_row_hit_by_name). Inset 4px top/bottom so adjacent tap targets
     * keep the 8px adjacency floor (the inbox row-inset trade). Deco (name/
     * sub/check) is added AFTER, so it paints on top. A disabled row gets
     * NO tap target (deco only) — On Me without a fix is not tappable. */
    if (enabled) {
        lv_obj_t *hit = lv_button_create(row);
        lv_obj_remove_style_all(hit);
        lv_obj_set_size(hit, w, FF_INBOX_RALLY_ROW_H - 2 * FF_INBOX_RALLY_ROW_HIT_INSET_Y);
        lv_obj_set_pos(hit, 0, FF_INBOX_RALLY_ROW_HIT_INSET_Y);
        lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(hit, 13, 0);
        lv_obj_set_style_bg_color(hit, lv_color_hex(FF_THEME_CREW_VIOLET), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(hit, LV_OPA_30, LV_STATE_PRESSED);
        lv_obj_add_event_cb(hit, inbox_rally_place_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);
    }

    uint32_t const name_color = !enabled ? FF_THEME_COLOR_DIM : FF_THEME_COLOR_INK;
    lv_obj_t *nm = inbox_mk_label(row, name, FF_THEME_FONT_LABEL, name_color);
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 16, sub != NULL ? -8 : 0);
    if (sub != NULL) {
        lv_obj_t *s = inbox_mk_label(row, sub, FF_THEME_FONT_CHIP, FF_THEME_COLOR_DIM);
        lv_obj_align(s, LV_ALIGN_LEFT_MID, 16, 11);
    }
    if (selected) {
        lv_obj_t *ck = inbox_mk_label(row, LV_SYMBOL_OK, FF_THEME_FONT_CHIP, FF_THEME_CREW_VIOLET);
        lv_obj_align(ck, LV_ALIGN_RIGHT_MID, -14, 0);
    }
}

/* A small centered "PLACES" divider between On Me and the landmark rows. */
static void inbox_build_rally_divider(lv_obj_t *list, int32_t y)
{
    int32_t const margin = inbox_safe_margin_x(FF_INBOX_RALLY_LIST_TOP_Y, FF_INBOX_RALLY_LIST_H);
    lv_obj_t *lab = inbox_mk_label(list, "PLACES", FF_THEME_FONT_CHIP, FF_THEME_COLOR_DIM);
    lv_obj_set_style_text_letter_space(lab, 2, 0);
    lv_obj_set_pos(lab, margin + 8, y + 6);
}

/* The pinned WHEN chip + Send footer. */
static void inbox_build_rally_footer(lv_obj_t *parent, ff_app_rally_t const *r)
{
    int32_t const margin = inbox_safe_margin_x(FF_INBOX_RALLY_FOOTER_Y, FF_INBOX_RALLY_FOOTER_H);
    int32_t const total_w = FF_THEME_PUCK_PX - 2 * margin;
    int32_t const send_w = total_w - FF_INBOX_RALLY_WHEN_W - FF_INBOX_RALLY_FOOTER_GAP;

    /* WHEN chip — cycles Now / +15m / +30m. */
    lv_obj_t *when = lv_button_create(parent);
    lv_obj_remove_style_all(when);
    lv_obj_set_size(when, FF_INBOX_RALLY_WHEN_W, FF_INBOX_RALLY_FOOTER_H);
    lv_obj_set_pos(when, margin, FF_INBOX_RALLY_FOOTER_Y);
    lv_obj_set_style_radius(when, 16, 0);
    lv_obj_set_style_bg_color(when, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(when, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(when, inbox_rally_when_cb, LV_EVENT_CLICKED, NULL);
    inbox_press_feedback(when);
    lv_obj_t *wl = inbox_mk_label(when, "WHEN", FF_THEME_FONT_CHIP, FF_THEME_COLOR_MUTED);
    lv_obj_set_style_text_letter_space(wl, 1, 0);
    lv_obj_align(wl, LV_ALIGN_CENTER, 0, -9);
    lv_obj_t *wv = inbox_mk_label(when, r->echo_when, FF_THEME_FONT_LABEL, FF_THEME_COLOR_INK);
    lv_obj_align(wv, LV_ALIGN_CENTER, 0, 9);

    /* Send Rally — echoes "<place> · <when>". Disabled (deco, no tap
     * target) when nothing is sendable (On Me off + no places). The armed
     * crew-confirm restyles it to an explicit second-tap ask. */
    int32_t const send_x = margin + FF_INBOX_RALLY_WHEN_W + FF_INBOX_RALLY_FOOTER_GAP;
    if (!r->can_send) {
        lv_obj_t *dead = lv_obj_create(parent);
        inbox_child_deco(dead);
        lv_obj_set_size(dead, send_w, FF_INBOX_RALLY_FOOTER_H);
        lv_obj_set_pos(dead, send_x, FF_INBOX_RALLY_FOOTER_Y);
        lv_obj_set_style_radius(dead, 16, 0);
        lv_obj_set_style_bg_color(dead, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
        lv_obj_set_style_bg_opa(dead, LV_OPA_50, 0);
        lv_obj_t *dl = inbox_mk_label(dead, "PICK A PLACE", FF_THEME_FONT_CHIP, FF_THEME_COLOR_DIM);
        lv_obj_center(dl);
        return;
    }

    lv_obj_t *send = lv_button_create(parent);
    lv_obj_remove_style_all(send);
    lv_obj_set_size(send, send_w, FF_INBOX_RALLY_FOOTER_H);
    lv_obj_set_pos(send, send_x, FF_INBOX_RALLY_FOOTER_Y);
    lv_obj_set_style_radius(send, 16, 0);
    lv_obj_set_style_bg_color(send, lv_color_hex(FF_THEME_CREW_VIOLET), 0);
    lv_obj_set_style_bg_opa(send, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(send, inbox_rally_send_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(send, lv_color_hex(FF_THEME_COLOR_INK), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(send, LV_OPA_20, LV_STATE_PRESSED);

    char echo[FF_APP_NAME_LEN + 12];
    if (r->confirm_armed) {
        /* Crew-wide: first tap armed the confirm — the button now asks for
         * the second, explicit tap (S22 AC4 armed state). */
        lv_obj_t *tl = inbox_mk_label(send, "Tap to confirm", FF_THEME_FONT_LABEL, FF_THEME_COLOR_BG);
        lv_obj_align(tl, LV_ALIGN_CENTER, 0, -9);
        snprintf(echo, sizeof(echo), "%s . %s", r->echo_place, r->echo_when);
        lv_obj_t *el = inbox_mk_label(send, echo, FF_THEME_FONT_CHIP, FF_THEME_COLOR_BG);
        lv_obj_set_style_text_opa(el, LV_OPA_70, 0);
        lv_obj_align(el, LV_ALIGN_CENTER, 0, 10);
    } else {
        lv_obj_t *sl = inbox_mk_label(send, "Send Rally", FF_THEME_FONT_LABEL, FF_THEME_COLOR_BG);
        lv_obj_align(sl, LV_ALIGN_CENTER, 0, -9);
        /* ASCII separator, not the canvas middot (the vendored font has no
         * middot — the same trap the inbox preview divider hit). */
        snprintf(echo, sizeof(echo), "%s . %s", r->echo_place, r->echo_when);
        lv_obj_t *el = inbox_mk_label(send, echo, FF_THEME_FONT_CHIP, FF_THEME_COLOR_BG);
        lv_obj_set_style_text_opa(el, LV_OPA_70, 0);
        lv_obj_align(el, LV_ALIGN_CENTER, 0, 10);
    }
}

static void inbox_build_rally(lv_obj_t *parent, ff_app_inbox_t const *v, bool colorblind)
{
    (void)colorblind;
    ff_app_rally_t const *r = &v->rally;

    /* Header: the >=44px close (BACK -> popup) + a violet RALLY caption.
     * No scope label here (the popup already stated it; the Send echo
     * restates the payload — the maintainer's header note). */
    inbox_build_back(parent);
    lv_obj_t *cap = inbox_mk_label(parent, LV_SYMBOL_GPS " RALLY", FF_THEME_FONT_CHIP, FF_THEME_CREW_VIOLET);
    lv_obj_set_style_text_letter_space(cap, 2, 0);
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 40);

    /* WHERE — a scrollable radio list. */
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, FF_INBOX_RALLY_LIST_TOP_Y);
    lv_obj_set_size(list, FF_THEME_PUCK_PX, FF_INBOX_RALLY_LIST_H);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    /* Genuine-overflow gate, computed analytically (every WHERE row has a
     * FIXED height — FF_INBOX_RALLY_ROW_H, no content-driven variance
     * like the thread's message rows — so this exactly predicts what
     * lv_obj_get_scroll_bottom(list) will read once the rows below are
     * built, with no need for a build-then-measure-then-rebuild pass). */
    uint8_t const n_places = (r->place_count > FF_APP_RALLY_MAX_PLACES) ? FF_APP_RALLY_MAX_PLACES : r->place_count;
    int32_t rally_content_h = FF_INBOX_RALLY_ROW_H + FF_HIT_MIN_GAP_PX; /* On Me */
    if (n_places > 0u) {
        rally_content_h += FF_INBOX_RALLY_DIVIDER_H +
                           (int32_t)n_places * (FF_INBOX_RALLY_ROW_H + FF_HIT_MIN_GAP_PX);
    }
    if (rally_content_h > FF_INBOX_RALLY_LIST_H) {
        /* Added FIRST (child index 0) — see this file's header comment
         * above (Rally screen geometry) for why z-order here is load-
         * bearing, not incidental: LVGL checks a node's children in
         * REVERSE index order, so every row (added after, below) is
         * tried before this catcher, and a row's own hit target still
         * wins whenever a touch actually lands on it. */
        int32_t const margin = inbox_safe_margin_x(FF_INBOX_RALLY_LIST_TOP_Y, FF_INBOX_RALLY_LIST_H);
        lv_obj_t *hit = lv_obj_create(list);
        lv_obj_remove_style_all(hit);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_FLOATING);
        lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
        lv_obj_set_pos(hit, margin, 0);
        lv_obj_set_size(hit, FF_THEME_PUCK_PX - 2 * margin, FF_INBOX_RALLY_LIST_H);
    }

    int32_t y = 0;
    /* On Me (index 0), pinned first. Disabled with an honest reason when
     * my position is unknown — never a fabricated location. */
    inbox_build_rally_row(list, y, 0u, "On Me",
                            r->on_me_ok ? "rally to my live location" : "no position yet",
                            r->sel == 0u && r->on_me_ok, r->on_me_ok);
    y += FF_INBOX_RALLY_ROW_H + FF_HIT_MIN_GAP_PX;

    if (r->place_count > 0u) {
        inbox_build_rally_divider(list, y);
        y += FF_INBOX_RALLY_DIVIDER_H;
        for (uint8_t i = 0; i < r->place_count && i < FF_APP_RALLY_MAX_PLACES; i++) {
            uint8_t const idx = (uint8_t)(i + 1u);
            inbox_build_rally_row(list, y, idx, r->place_names[i], NULL, r->sel == idx, true);
            y += FF_INBOX_RALLY_ROW_H + FF_HIT_MIN_GAP_PX;
        }
    }

    inbox_build_bottom_fade(parent, FF_INBOX_RALLY_LIST_TOP_Y + FF_INBOX_RALLY_LIST_H);
    inbox_build_rally_footer(parent, r);
}

/* ---------------------------------------------------------------------
 * Public helpers + entry point.
 * ------------------------------------------------------------------- */

uint16_t ff_scr_inbox_unread_count(ff_app_inbox_t const *v)
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

void ff_scr_inbox_build(lv_obj_t *parent, ff_app_inbox_t const *v, bool colorblind)
{
    if (parent == NULL || v == NULL) {
        return;
    }

    switch (v->subview) {
    case FF_INBOX_SUB_PICKER:
        inbox_build_picker(parent, v, colorblind);
        return;
    case FF_INBOX_SUB_THREAD:
        inbox_build_thread(parent, v, colorblind);
        return;
    case FF_INBOX_SUB_POPUP:
        inbox_build_popup(parent, v, colorblind);
        return;
    case FF_INBOX_SUB_RALLY:
        inbox_build_rally(parent, v, colorblind);
        return;
    case FF_INBOX_SUB_INBOX:
    default:
        inbox_build_inbox(parent, v, colorblind);
        return;
    }
}
