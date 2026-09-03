/**
 * test_scr_banner.c — S26 slice d, maintainer decision B (2026-09-02,
 * docs/specs/S26-device-lifecycle.md "Notifications (slice d)"): the
 * banner strip's move to cover the status bar row instead of the row
 * below it. Round 2 (orchestrator review on PR #157) widened the strip
 * (90 -> 160px) and moved its centre down slightly (dy -160 -> -146) so
 * it reads as a real banner (full demo names, a readable preview) while
 * still covering the status row, and wired it into the launcher (home)
 * face, which never rendered one at all before this round.
 *
 * Same "build the real screens, measure the real rects" discipline
 * test_radar_layout.c / test_scr_flare.c / test_scr_intent.c's S99
 * compose-SEND corner-distance test already established for this exact
 * bug class — a pixel-stable golden proves nothing moved, never that
 * what's there is RIGHT (AGENTS.md's proxy-check lesson: "measure, don't
 * reason harder").
 *
 * ## Real measured geometry (this file's own tests are the proof; these
 * numbers are recorded here so a future reader doesn't have to re-derive
 * them by hand)
 *   - strip:            (128,36)-(287,83)   — BANNER_W=160, BANNER_H=48,
 *     centered at (208,60) = (FF_THEME_GLASS_CX, PUCK_RADIUS+BANNER_CY)
 *   - clock label:      (99,38)-(157,53)    — PARTIALLY under the strip
 *     (its left 29px stay exposed, its right 30px are covered)
 *   - MESH label:        (185,38)-(226,53)   — entirely INSIDE the strip
 *   - battery label:     (270,38)-(297,53)   — PARTIALLY under the strip
 *     (its left 17px are covered, its right 10px stay exposed)
 * At this width the strip DOES now overlap the outer two labels, unlike
 * round 1's 90px strip — accepted deliberately (see scr_banner.c's own
 * geometry comment): the achievable, tested property is that the
 * strip's own y-range (one constant band across its whole width, being
 * a rectangle) fully contains the status text's y-band [38,53] — so
 * wherever it does reach, coverage is total top-to-bottom, never a
 * half-height sliver of text peeking out vertically. There is no
 * per-label "never touch it" guarantee any more; there IS a per-band
 * "never touch it by half" guarantee, which is what's actually testable
 * and what "no half-visible clock" can honestly mean once the strip is
 * wide enough to reach the clock at all.
 *
 * ## Mutation check (AGENTS.md standing brief item 2 / docs/review/
 * code-review.md item 6), hand-verified before pushing:
 * Temporarily reverting BANNER_CY to its pre-move value (-90.0f) and
 * rebuilding fails S26d_AC2_banner_covers_mesh_status_label AND
 * S26d_AC2_banner_covers_status_text_row_band — at the old position the
 * strip never reaches the status row at all, so it covers nothing
 * there. See the PR body for the exact `ctest` output.
 */
#include <math.h>
#include <string.h>

#include "unity.h"

#include "ff_app_state.h"
#include "ff_intent.h"
#include "ff_theme.h"
#include "radar_layout.h"
#include "scr_banner.h"
#include "scr_launcher.h" /* the launcher-collision finding, see the bottom section */
#include "scr_nav.h" /* ff_scr_nav_build / the remainder-rule masking pass, round 3 */
#include "scr_radar.h"
#include "scr_inbox.h"

/* setUp/tearDown, the spy sink, the frozen-but-advanceable tick,
 * find_label_exact, click(), and drag_v()/tap_at() (including the one
 * real hazard in drag_v() — the LVGL indev scroll-throw-animation
 * use-after-free) now live in ONE shared header (debt/test-naming-
 * harness), used by this file and test_scr_intent.c — see
 * support/lv_test_harness.h's own top comment for the extraction
 * rationale and the workaround's full derivation. This file previously
 * hand-rolled its own copy of every one of these. */
#include "support/lv_test_harness.h"

void setUp(void)
{
    ff_test_lv_setup(FF_THEME_WINDOW_PX);
}

void tearDown(void)
{
    ff_test_lv_teardown();
}

/* ---------------------------------------------------------------------
 * Small geometry helpers — lv_area_t's x2/y2 are INCLUSIVE (ff_layout.h's
 * own doc comment on the convention mismatch with this codebase's usual
 * "size = far - near" rects), so both helpers below work directly in
 * that inclusive convention rather than converting. Banner-specific (not
 * part of the shared header — test_scr_intent.c has no equivalent).
 * ------------------------------------------------------------------- */

static bool areas_overlap(lv_area_t const *a, lv_area_t const *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

static bool area_contains(lv_area_t const *outer, lv_area_t const *inner)
{
    return outer->x1 <= inner->x1 && outer->x2 >= inner->x2 && outer->y1 <= inner->y1 && outer->y2 >= inner->y2;
}

/* ---------------------------------------------------------------------
 * Fixture builders.
 * ------------------------------------------------------------------- */

static void make_banner(ff_app_banner_t *b)
{
    memset(b, 0, sizeof(*b));
    b->active = true;
    b->kind = FF_NOTIFY_MESSAGE;
    b->node_id = 111u;
    strncpy(b->name, "DANA", sizeof(b->name) - 1);
    b->color_idx = 0;
    strncpy(b->text, "you close? we're at the tower", sizeof(b->text) - 1);
    b->age_ms = 4000u;
}

/* LIVE mode: the name/distance stack (RADAR_LAYOUT_STACK_NAME_DY/
 * _STACK_DIST_DY) AND the status bar (clock/MESH/battery) in one build,
 * so a single fixture serves every Radar-side test below. */
static void make_radar_live(ff_radar_view_t *r)
{
    memset(r, 0, sizeof(*r));
    r->mode = RADAR_LIVE;
    r->arrow_valid = true;
    strncpy(r->name, "DANA", sizeof(r->name) - 1);
    strncpy(r->dist_str, "320 m", sizeof(r->dist_str) - 1);
    strncpy(r->clock_str, "9:46 pm", sizeof(r->clock_str) - 1);
    r->mesh_ok = true;
    r->batt_pct = 74;
}

/* Signals thread, CREW scope, one OUT text bubble — same minimal shape
 * test_scr_intent.c's S24_thread_message_bubble_not_compressed uses. */
static void make_thread(ff_app_inbox_t *v)
{
    memset(v, 0, sizeof(*v));
    v->subview = FF_INBOX_SUB_THREAD;
    v->thread_node = 0u;
    strncpy(v->thread_name, "CREW", sizeof(v->thread_name) - 1);
    ff_inbox_conv_t *cv = &v->inbox.convs[v->inbox.conv_count++];
    memset(cv, 0, sizeof(*cv));
    cv->kind = FF_CONV_CREW;

    ff_inbox_msg_t *m = &v->thread.msgs[v->thread.msg_count++];
    memset(m, 0, sizeof(*m));
    m->kind = FEED_TEXT;
    m->dir = FEED_DIR_OUT;
    strncpy(m->text, "copy, see you there", sizeof(m->text) - 1);
    m->age_ms = 60000u;
}

/* ---------------------------------------------------------------------
 * (a) Disjoint from Radar's name/distance stack, and from a thread's
 * first message bubble — the ORIGINAL bug this whole move exists to
 * avoid recreating one row down (this file's own top comment / the PR
 * this lands in).
 * ------------------------------------------------------------------- */

static void S26d_AC2_banner_disjoint_from_radar_name_distance_stack(void)
{
    ff_radar_view_t r;
    make_radar_live(&r);
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, FF_THEME_WINDOW_PX, FF_THEME_WINDOW_PX);
    ff_scr_radar_build(parent, &r, false, false /* screen_flip (#158): banner tests use the unflipped glass */);

    ff_app_banner_t b;
    make_banner(&b);
    ff_scr_banner_build(parent, &b, false);
    lv_obj_update_layout(parent);

    lv_obj_t *name_lbl = find_label_exact(parent, "DANA");
    lv_obj_t *dist_lbl = find_label_exact(parent, "320 m");
    TEST_ASSERT_NOT_NULL(name_lbl);
    TEST_ASSERT_NOT_NULL(dist_lbl);

    uint32_t n = lv_obj_get_child_count(parent);
    lv_obj_t *strip = lv_obj_get_child(parent, n - 1); /* banner built last, per scr_nav.c's own call order */
    lv_area_t strip_a, name_a, dist_a;
    lv_obj_get_coords(strip, &strip_a);
    lv_obj_get_coords(name_lbl, &name_a);
    lv_obj_get_coords(dist_lbl, &dist_a);

    TEST_ASSERT_FALSE_MESSAGE(areas_overlap(&strip_a, &name_a), "banner must not overlap Radar's name label");
    TEST_ASSERT_FALSE_MESSAGE(areas_overlap(&strip_a, &dist_a), "banner must not overlap Radar's distance label");
}

static void S26d_AC2_banner_disjoint_from_thread_first_bubble(void)
{
    ff_app_inbox_t v;
    make_thread(&v);
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, FF_THEME_WINDOW_PX, FF_THEME_WINDOW_PX);
    ff_scr_inbox_build(parent, &v, false);

    ff_app_banner_t b;
    make_banner(&b);
    ff_scr_banner_build(parent, &b, false);
    lv_obj_update_layout(parent);

    lv_obj_t *label = find_label_exact(parent, "copy, see you there");
    TEST_ASSERT_NOT_NULL_MESSAGE(label, "message bubble text not found");
    lv_obj_t *bubble = lv_obj_get_parent(label);
    TEST_ASSERT_NOT_NULL(bubble);

    uint32_t n = lv_obj_get_child_count(parent);
    lv_obj_t *strip = lv_obj_get_child(parent, n - 1);
    lv_area_t strip_a, bubble_a;
    lv_obj_get_coords(strip, &strip_a);
    lv_obj_get_coords(bubble, &bubble_a);

    TEST_ASSERT_FALSE_MESSAGE(areas_overlap(&strip_a, &bubble_a), "banner must not overlap the thread's first bubble");
}

/* ---------------------------------------------------------------------
 * (b) Covers the status bar row.
 *
 * Orchestrator review, round 2: round 1's strip (90px) only ever
 * touched the MESH label and never reached clock/battery at all, so
 * "never half-clips" was trivially true by staying away. This round's
 * wider strip (160px, see the top-of-file / scr_banner.c geometry
 * comments for why) is a deliberate trade the other way — it now
 * reaches clock and battery too, PARTIALLY (measured: clock's left 29px
 * of 58 stays exposed, battery's right 10px of 27 stays exposed) — and
 * that is accepted, not a regression: the real, achievable, testable
 * property "covers the status bar row" can mean is the strip's own
 * rect (which has one constant y-range across its whole width, being a
 * rectangle) spans the status TEXT's y-band (measured 38..53) — i.e.
 * covers it FULLY wherever it reaches, never a half-height sliver of
 * text peeking out from under the pill vertically. That holds by
 * construction for any correctly-sized rectangle and is what's checked
 * below, plus the concrete MESH-label containment as the "this isn't
 * vacuous" proof that the strip really does sit over real content.
 * This is also the mutation-sensitive half of (a)/(b): reverting
 * BANNER_CY to -90 fails the first of these two (see top comment).
 * ------------------------------------------------------------------- */

static void S26d_AC2_banner_covers_mesh_status_label(void)
{
    ff_radar_view_t r;
    make_radar_live(&r);
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, FF_THEME_WINDOW_PX, FF_THEME_WINDOW_PX);
    ff_scr_radar_build(parent, &r, false, false /* screen_flip (#158): banner tests use the unflipped glass */);

    ff_app_banner_t b;
    make_banner(&b);
    ff_scr_banner_build(parent, &b, false);
    lv_obj_update_layout(parent);

    lv_obj_t *mesh_lbl = find_label_exact(parent, "MESH");
    TEST_ASSERT_NOT_NULL(mesh_lbl);

    uint32_t n = lv_obj_get_child_count(parent);
    lv_obj_t *strip = lv_obj_get_child(parent, n - 1);
    lv_area_t strip_a, mesh_a;
    lv_obj_get_coords(strip, &strip_a);
    lv_obj_get_coords(mesh_lbl, &mesh_a);

    TEST_ASSERT_TRUE_MESSAGE(area_contains(&strip_a, &mesh_a),
                             "banner must fully cover the MESH status label — no half-visible text behind it");
}

/* The status TEXT's own measured y-band (clock/MESH/battery all share
 * one font/baseline, so one band covers all three — measured directly
 * off the rendered labels, not assumed). A strip whose own y-range
 * fully contains this band covers it FULLY across the strip's entire
 * width by construction (a rectangle has one y-range for every x in
 * it) — no per-x variation to separately check. */
#define STATUS_TEXT_ROW_TOP_Y 38 /* measured; coordinator review said "39..53", real render is 38..53 */
#define STATUS_TEXT_ROW_BOTTOM_Y 53

static void S26d_AC2_banner_covers_status_text_row_band(void)
{
    ff_radar_view_t r;
    make_radar_live(&r);
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, FF_THEME_WINDOW_PX, FF_THEME_WINDOW_PX);
    ff_scr_radar_build(parent, &r, false, false /* screen_flip (#158): banner tests use the unflipped glass */);
    lv_obj_update_layout(parent);

    /* Cross-check the hardcoded band against the real rendered labels —
     * if scr_radar.c's status-row layout ever moves, this test fails
     * LOUDLY (NOT_NULL/message) rather than silently checking a stale
     * band against a strip that quietly stopped covering anything. */
    lv_obj_t *clock_lbl = find_label_exact(parent, "9:46 pm");
    lv_obj_t *batt_lbl = find_label_exact(parent, "74%");
    TEST_ASSERT_NOT_NULL(clock_lbl);
    TEST_ASSERT_NOT_NULL(batt_lbl);
    lv_area_t clock_a, batt_a;
    lv_obj_get_coords(clock_lbl, &clock_a);
    lv_obj_get_coords(batt_lbl, &batt_a);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(STATUS_TEXT_ROW_TOP_Y, clock_a.y1,
                                    "measured status-text top drifted — update STATUS_TEXT_ROW_TOP_Y");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(STATUS_TEXT_ROW_BOTTOM_Y, clock_a.y2,
                                    "measured status-text bottom drifted — update STATUS_TEXT_ROW_BOTTOM_Y");
    TEST_ASSERT_EQUAL_INT32(clock_a.y1, batt_a.y1);
    TEST_ASSERT_EQUAL_INT32(clock_a.y2, batt_a.y2);

    ff_app_banner_t b;
    make_banner(&b);
    ff_scr_banner_build(parent, &b, false);
    lv_obj_update_layout(parent);

    uint32_t n = lv_obj_get_child_count(parent);
    lv_obj_t *strip = lv_obj_get_child(parent, n - 1);
    lv_area_t strip_a;
    lv_obj_get_coords(strip, &strip_a);

    char msg[128];
    snprintf(msg, sizeof(msg), "banner y-range [%d,%d] must fully contain the status-text band [%d,%d]",
             strip_a.y1, strip_a.y2, STATUS_TEXT_ROW_TOP_Y, STATUS_TEXT_ROW_BOTTOM_Y);
    TEST_ASSERT_TRUE_MESSAGE(strip_a.y1 <= STATUS_TEXT_ROW_TOP_Y && strip_a.y2 >= STATUS_TEXT_ROW_BOTTOM_Y, msg);
}

/* ---------------------------------------------------------------------
 * (c) Every banner corner clears the glass by >= 10px — the true 2D
 * Euclidean bezel-margin bar (S99_compose_send_corner_clears_bezel_
 * margin_bar's own precedent, test_scr_intent.c), not just the weaker
 * per-axis chord bound (see scr_banner.c's layout comment for why the
 * chord bound alone is insufficient this close to the pole).
 * ------------------------------------------------------------------- */

static void S26d_AC2_banner_corners_clear_glass_by_10px(void)
{
    ff_app_banner_t b;
    make_banner(&b);
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, FF_THEME_WINDOW_PX, FF_THEME_WINDOW_PX);
    ff_scr_banner_build(parent, &b, false);
    lv_obj_update_layout(parent);

    lv_obj_t *strip = lv_obj_get_child(parent, 0);
    lv_area_t a;
    lv_obj_get_coords(strip, &a);

    /* lv_area_t's x2/y2 are inclusive — +1 to get the exclusive far
     * corner (ff_layout.h's own documented conversion). */
    float const cx = (float)FF_THEME_GLASS_CX;
    float const cy = (float)FF_THEME_GLASS_CY;
    float const safe_r = (float)FF_THEME_GLASS_R - 10.0f;
    float const corners_x[4] = {(float)a.x1, (float)(a.x2 + 1), (float)a.x1, (float)(a.x2 + 1)};
    float const corners_y[4] = {(float)a.y1, (float)a.y1, (float)(a.y2 + 1), (float)(a.y2 + 1)};

    for (int i = 0; i < 4; i++) {
        float dx = corners_x[i] - cx;
        float dy = corners_y[i] - cy;
        float dist = sqrtf(dx * dx + dy * dy);
        char msg[96];
        snprintf(msg, sizeof(msg), "banner corner %d measures %.2fpx from glass center (bar: %.2fpx)", i,
                 (double)dist, (double)safe_r);
        TEST_ASSERT_LESS_OR_EQUAL_FLOAT_MESSAGE(safe_r, dist, msg);
    }
}

/* ---------------------------------------------------------------------
 * (d) Tap -> FF_INTENT_BANNER_OPEN exactly once; a drag-off emits
 * nothing (LV_OBJ_FLAG_PRESS_LOCK cleared — the #145 lesson).
 * ------------------------------------------------------------------- */

static void S26d_AC2_banner_tap_emits_banner_open_exactly_once(void)
{
    ff_app_banner_t b;
    make_banner(&b);
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_banner_build(parent, &b, false);

    lv_obj_t *strip = lv_obj_get_child(parent, 0);
    click(strip);

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_BANNER_OPEN, s_spy.last.kind);
}

static void S26d_AC2_banner_drag_off_emits_nothing(void)
{
    ff_app_banner_t b;
    make_banner(&b);
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, FF_THEME_WINDOW_PX, FF_THEME_WINDOW_PX);
    ff_scr_banner_build(parent, &b, false);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *strip = lv_obj_get_child(parent, 0);
    lv_area_t a;
    lv_obj_get_coords(strip, &a);
    int32_t cx = (a.x1 + a.x2) / 2;
    int32_t cy = (a.y1 + a.y2) / 2;

    /* Press on the strip's own center, drag 150px straight down (well
     * off the 48px-tall strip), release far away, never back on it. */
    drag_v(cy, cy + 150, cx);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_spy.count, "a slide-off of the banner must never open it");
}

/* ---------------------------------------------------------------------
 * Launcher wiring (orchestrator review round 2, remainder rule round 3):
 * round 1 found `ff_scr_launcher_build` never called `ff_scr_banner_
 * build` at all and left it unwired. Round 2 wired it in for real
 * (`ff_scr_launcher_build` now calls `ff_scr_banner_build` LAST, same
 * "built after, drawn on top" convention `scr_nav.c` uses for every
 * other face) and masked the covered Inbox satellite unconditionally
 * by compass position. Round 3 replaced that blanket mask with the
 * shared remainder rule (`ff_scr_nav_mask_clickables_under_banner`,
 * scr_nav.h) — the SAME pass `scr_nav.c` runs for the five base faces,
 * so the launcher's Inbox satellite is masked for the identical
 * measured reason (its ~88x37px remainder under the strip fails the
 * 44px HEIGHT floor), not a hard-coded "compass_pos 0 always loses"
 * rule. See scr_nav.c's own top-of-block comment for the full
 * rationale and the "the region is the banner" acceptance this rule
 * still honors.
 * ------------------------------------------------------------------- */

static void S26d_AC2_launcher_banner_tap_emits_banner_open_not_launcher_select(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    make_banner(&state.banner);

    ff_scr_launcher_build(&state);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *puck = lv_obj_get_child(scr, 0);
    uint32_t n = lv_obj_get_child_count(puck);
    lv_obj_t *strip = lv_obj_get_child(puck, n - 1); /* banner built last, per ff_scr_launcher_build's own call order */

    click(strip);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_spy.count, "the banner tap must emit exactly one intent");
    TEST_ASSERT_EQUAL_MESSAGE(FF_INTENT_BANNER_OPEN, s_spy.last.kind,
                              "tapping the banner over the launcher must open the sender's thread "
                              "(FF_INTENT_BANNER_OPEN), never launcher-select whatever sits underneath it");
}

/* The Inbox satellite (the one the banner ever reaches — compass_pos 0,
 * the top cardinal point) must stop being independently tappable while
 * the banner covers it — its own remainder there (~88x37px) fails the
 * 44px HEIGHT floor (ff_scr_nav_remainder_clears_floor), same rule as
 * every other masked control in this file. Found by its own "INBOX"
 * caption, walking up to the satellite's button ancestor (caption ->
 * button; same one-step-up shape find_row_hit_by_name uses elsewhere in
 * this codebase for a caption/row relationship). */
static void S26d_AC2_launcher_inbox_satellite_not_clickable_while_banner_active(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    make_banner(&state.banner);

    ff_scr_launcher_build(&state);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *caption = find_label_exact(lv_screen_active(), "INBOX");
    TEST_ASSERT_NOT_NULL(caption);
    lv_obj_t *satellite = lv_obj_get_parent(caption);
    TEST_ASSERT_NOT_NULL(satellite);

    TEST_ASSERT_FALSE_MESSAGE(lv_obj_has_flag(satellite, LV_OBJ_FLAG_CLICKABLE),
                              "the Inbox satellite must not remain independently clickable "
                              "while a banner covers it");
}

/* Regression guard for the OTHER half of the brief ("keep the launcher-
 * without-banner goldens byte-identical"): with no active banner, Inbox
 * stays exactly as clickable as every other satellite — this is the
 * same property test_scr_intent.c's S26e_launcher_signals_circle_emits_
 * index_2 already exercises end-to-end (click -> LAUNCHER_SELECT idx 2),
 * checked here too as a direct flag assertion so a regression shows up
 * in this file's own suite, not only a distant one. */
static void S26d_AC2_launcher_inbox_satellite_stays_clickable_without_banner(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    state.banner.active = false;

    ff_scr_launcher_build(&state);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *caption = find_label_exact(lv_screen_active(), "INBOX");
    TEST_ASSERT_NOT_NULL(caption);
    lv_obj_t *satellite = lv_obj_get_parent(caption);
    TEST_ASSERT_NOT_NULL(satellite);

    TEST_ASSERT_TRUE_MESSAGE(lv_obj_has_flag(satellite, LV_OBJ_FLAG_CLICKABLE),
                             "Inbox must stay clickable when no banner is showing");
}

/* ---------------------------------------------------------------------
 * Remainder-rule masking (orchestrator review round 3): the honest
 * "only mask what the uncovered remainder can't itself serve as a
 * target" rule scr_nav.c's own top-of-block comment documents, proven
 * against the ONE real control it currently affects outside the
 * launcher (Inbox's thread/picker/popup/rally BACK button) plus a
 * synthetic control whose remainder DOES clear the floor, to prove the
 * rule doesn't over-mask.
 * ------------------------------------------------------------------- */

static void S26d_AC2_inbox_thread_back_masked_while_banner_active(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    state.active_face = FF_APP_FACE_INBOX;
    make_thread(&state.inbox);
    make_banner(&state.banner);

    ff_scr_nav_build(&state);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *glyph = find_label_exact(lv_screen_active(), LV_SYMBOL_LEFT);
    TEST_ASSERT_NOT_NULL_MESSAGE(glyph, "thread BACK glyph not found");
    lv_obj_t *back = lv_obj_get_parent(glyph);
    TEST_ASSERT_NOT_NULL(back);

    lv_area_t back_a, strip_a;
    lv_obj_get_coords(back, &back_a);
    /* The banner is the last child scr_banner.c's own contract adds to
     * the puck (scr_nav.c builds puck -> content -> ... -> banner). */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *puck = lv_obj_get_child(scr, 0);
    lv_obj_t *strip = lv_obj_get_child(puck, lv_obj_get_child_count(puck) - 1);
    lv_obj_get_coords(strip, &strip_a);

    /* This IS the exact case round 3 fixes: only 25 of BACK's 44px
     * width sits under the strip, leaving a real 19px-wide sliver — too
     * narrow (< FF_THEME_MIN_HIT_PX) to be its own target, so BACK is
     * still the right control to mask, just for the measured reason. */
    TEST_ASSERT_TRUE_MESSAGE(areas_overlap(&back_a, &strip_a), "test is vacuous unless BACK and the banner overlap");
    TEST_ASSERT_FALSE_MESSAGE(ff_scr_nav_remainder_clears_floor(back_a, strip_a),
                              "BACK's remainder must fail the 44px floor for this test to be meaningful");
    TEST_ASSERT_FALSE_MESSAGE(lv_obj_has_flag(back, LV_OBJ_FLAG_CLICKABLE),
                              "the thread BACK button must not stay independently clickable "
                              "while its remainder under the banner is under 44px");
}

/* "restored after expiry/rebuild": a banner is transient (6s, per spec)
 * — the shell rebuilds the WHOLE screen every tick, so "restored" is
 * simply what a fresh build with an expired (inactive) banner produces.
 * Proven directly rather than assumed. */
static void S26d_AC2_inbox_thread_back_clickable_again_once_banner_inactive(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    state.active_face = FF_APP_FACE_INBOX;
    make_thread(&state.inbox);
    state.banner.active = false; /* expired / never queued */

    ff_scr_nav_build(&state);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *glyph = find_label_exact(lv_screen_active(), LV_SYMBOL_LEFT);
    TEST_ASSERT_NOT_NULL_MESSAGE(glyph, "thread BACK glyph not found");
    lv_obj_t *back = lv_obj_get_parent(glyph);
    TEST_ASSERT_NOT_NULL(back);

    TEST_ASSERT_TRUE_MESSAGE(lv_obj_has_flag(back, LV_OBJ_FLAG_CLICKABLE),
                             "BACK must be clickable again once no banner is showing");
}

/* A synthetic control whose remainder DOES clear 44px in both
 * dimensions on either side of the banner (300x48, spanning the
 * banner's own y-range but far wider in x) must KEEP its clickability —
 * proving the rule doesn't over-mask — and a REAL coordinate tap on its
 * visible (uncovered) left slice must still route to it, not vanish
 * into the banner drawn on top of the OTHER half of this control. */
static int s_wide_clicks;

static void wide_click_cb(lv_event_t *e)
{
    (void)e;
    s_wide_clicks++;
}

static void S26d_AC2_object_with_wide_remainder_stays_clickable_and_routes_tap(void)
{
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(parent); /* strip default theme padding — this parent must be a raw (0,0)-origin canvas */
    lv_obj_set_size(parent, FF_THEME_WINDOW_PX, FF_THEME_WINDOW_PX);
    lv_obj_set_pos(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *wide = lv_button_create(parent);
    lv_obj_remove_style_all(wide);
    lv_obj_set_size(wide, 300, 48);
    lv_obj_set_pos(wide, 60, 36); /* same y-band the banner sits in, MUCH wider in x */
    s_wide_clicks = 0;
    lv_obj_add_event_cb(wide, wide_click_cb, LV_EVENT_CLICKED, NULL);

    ff_app_banner_t b;
    make_banner(&b);
    ff_scr_banner_build(parent, &b, false);
    lv_obj_update_layout(parent);

    lv_obj_t *strip = lv_obj_get_child(parent, lv_obj_get_child_count(parent) - 1);
    lv_area_t strip_a, wide_a;
    lv_obj_get_coords(strip, &strip_a);
    lv_obj_get_coords(wide, &wide_a);

    TEST_ASSERT_TRUE_MESSAGE(areas_overlap(&wide_a, &strip_a), "test is vacuous unless the control and banner overlap");
    TEST_ASSERT_TRUE_MESSAGE(ff_scr_nav_remainder_clears_floor(wide_a, strip_a),
                             "this control's remainder must clear 44px both ways for this test to be meaningful");

    ff_scr_nav_mask_clickables_under_banner(parent, strip, &strip_a);

    TEST_ASSERT_TRUE_MESSAGE(lv_obj_has_flag(wide, LV_OBJ_FLAG_CLICKABLE),
                             "a control whose remainder clears the 44px floor must stay clickable");

    /* Tap inside the LEFT slice (x=[60,strip_a.x1), y=[36,84)) — visible,
     * uncovered, and per the assertion above >= 44px in both dimensions. */
    tap_at(80, 60);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_wide_clicks, "a real tap on the control's visible remainder must reach it");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S26d_AC2_banner_disjoint_from_radar_name_distance_stack);
    RUN_TEST(S26d_AC2_banner_disjoint_from_thread_first_bubble);
    RUN_TEST(S26d_AC2_banner_covers_mesh_status_label);
    RUN_TEST(S26d_AC2_banner_covers_status_text_row_band);
    RUN_TEST(S26d_AC2_banner_corners_clear_glass_by_10px);
    RUN_TEST(S26d_AC2_banner_tap_emits_banner_open_exactly_once);
    RUN_TEST(S26d_AC2_banner_drag_off_emits_nothing);
    RUN_TEST(S26d_AC2_launcher_banner_tap_emits_banner_open_not_launcher_select);
    RUN_TEST(S26d_AC2_launcher_inbox_satellite_not_clickable_while_banner_active);
    RUN_TEST(S26d_AC2_launcher_inbox_satellite_stays_clickable_without_banner);
    RUN_TEST(S26d_AC2_inbox_thread_back_masked_while_banner_active);
    RUN_TEST(S26d_AC2_inbox_thread_back_clickable_again_once_banner_inactive);
    RUN_TEST(S26d_AC2_object_with_wide_remainder_stays_clickable_and_routes_tap);
    return UNITY_END();
}
