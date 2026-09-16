/**
 * test_scr_banner.c — S26 slice d (docs/specs/S26-device-lifecycle.md
 * "Notifications (slice d)"), REWORKED by puck-ux-usability-2026-09-15
 * slice 4 (finding 4: "the banner occludes the status row"). Slice d's
 * original maintainer decision moved the strip to COVER the status bar
 * (clock/mesh/battery), on the theory that row was the least valuable
 * thing a transient banner could hide. The 2026-09-15 review measured the
 * result and found it wrong — `banner_on_radar.png` truncated the clock,
 * dropped `LINKED` entirely, and reduced the battery to a bare `%`, the
 * two facts a user checks before trusting the device at all. Slice 4
 * moves the strip BELOW the row instead, so every test in section (b)
 * below now asserts DISJOINT-from, not COVERS — the exact opposite of
 * what this file used to check.
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
 *   - strip:            (108,55)-(307,102)  — BANNER_W=200, BANNER_H=48,
 *     centered at (208,79) = (FF_THEME_GLASS_CX, PUCK_RADIUS+BANNER_CY)
 *   - clock label:      (95,37)-(161,54)    — FF_THEME_FONT_MSG_BODY/INK
 *     as of slice 4 item 3 ("status-row time in INK"); ABOVE the strip,
 *     1px genuine gap (strip.y1=55 > clock.y2=54)
 *   - battery label:    (270,38)-(297,53)   — unchanged font/color;
 *     also entirely above the strip
 *   - thread's first bubble BACKGROUND: overlaps the strip by 5px at its
 *     own top edge (98 vs strip bottom 102) — the one deliberate,
 *     documented trade this slice makes (see scr_banner.c's own
 *     `BANNER_CY` comment) — but its TEXT label sits 8px further down
 *     (106) and never touches the strip at all.
 * There is no "never touch it" guarantee for the bubble's own decorative
 * background any more (see scr_banner.c's `BANNER_CY` comment for the
 * measured 44px-window conflict that makes this unavoidable at a valid
 * BANNER_H); there IS a "never touch the actual TEXT" guarantee, which is
 * what `S26d_AC2_banner_disjoint_from_thread_first_bubble_TEXT` checks.
 *
 * ## Mutation check (AGENTS.md standing brief item 2 / docs/review/
 * code-review.md item 6), hand-verified before pushing:
 * Temporarily reverting `BANNER_CY` to its pre-slice-4 value
 * (`RADAR_LAYOUT_STATUS_BAR_DY + 14.0f`, i.e. covering the status row
 * again — strip y-range [36,83]) and rebuilding fails FOUR of this
 * file's own tests, not just the two the mutation targets:
 * `S26d_AC2_banner_disjoint_from_thread_first_bubble_TEXT` (now vacuous —
 * the old, higher strip no longer even reaches the bubble's background),
 * `S26d_AC2_banner_disjoint_from_mesh_status_label`,
 * `S26d_AC2_banner_disjoint_from_status_text_row` (strip [36,83] does not
 * sit below the status band [37,54]), and even
 * `S26d_AC2_banner_corners_clear_glass_by_10px` (the old centre's own
 * corner measures 197.23px against a 190px bar — it was already this
 * close to the glass at the old position). See the PR body for the exact
 * `ctest` output.
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
 * Small geometry helper — lv_area_t's x2/y2 are INCLUSIVE (ff_layout.h's
 * own doc comment on the convention mismatch with this codebase's usual
 * "size = far - near" rects), so it works directly in that inclusive
 * convention rather than converting. Banner-specific (not part of the
 * shared header — test_scr_intent.c has no equivalent).
 *
 * `area_contains` (the old "does the strip fully cover this label"
 * check) is gone as of slice 4 — finding 4 flips the whole banner-vs-
 * status-row relationship from "covers it" to "disjoint from it", so
 * nothing in this file needs a containment check anymore, only overlap.
 * ------------------------------------------------------------------- */

static bool areas_overlap(lv_area_t const *a, lv_area_t const *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
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
 * first message bubble's actual TEXT — the ORIGINAL bug this whole move
 * exists to avoid recreating one row down (this file's own top comment /
 * the PR this lands in).
 *
 * puck-ux-usability-2026-09-15 slice 4 (finding 4): moving the strip
 * BELOW the status row (see scr_banner.c's own `BANNER_CY` comment for
 * the full derivation) leaves only a 44px window between the status
 * text's real bottom edge (54, montserrat_16 now that the clock is
 * FF_THEME_FONT_MSG_BODY) and the thread's first bubble's real top edge
 * (98) — ONE px short of BANNER_H (48) with zero margin to spare on
 * either side. No placement can keep BOTH disjoint at once (measured,
 * not assumed — see scr_banner.c's own doc comment on this exact
 * conflict). `BANNER_CY` is pinned to keep the EXPLICIT, reviewed
 * obligation (never overlap the status TEXT — finding 4's whole point)
 * at a genuine, non-zero margin, and accepts the smallest achievable
 * overlap with the thread bubble's own decorative background instead —
 * verified below to land ABOVE the bubble's own text (its `inbox_msg_
 * bubble` label sits 8px down from the bubble's own top edge, scr_inbox.c),
 * so the readable message itself is never touched. An AGENTS.md-flagged
 * interpretation call: the review never analyzed the banner-vs-
 * first-bubble case, only status-row/glass.
 * ------------------------------------------------------------------- */

static void S26d_AC2_banner_disjoint_from_radar_name_distance_stack(void)
{
    ff_radar_view_t r;
    make_radar_live(&r);
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, FF_THEME_WINDOW_PX, FF_THEME_WINDOW_PX);
    ff_scr_radar_build(parent, &r, false, false /* screen_flip (#158): banner tests use the unflipped glass */,
                        /*locked=*/false, NULL);

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

static void S26d_AC2_banner_disjoint_from_thread_first_bubble_TEXT(void)
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

    /* The READABLE content is the label itself, not the bubble's own
     * decorative background box — see this section's top comment for why
     * the 44px window forces a choice, and why the bubble's own rounded
     * background (not its text) is the one that can afford to give up a
     * few px. */
    lv_obj_t *label = find_label_exact(parent, "copy, see you there");
    TEST_ASSERT_NOT_NULL_MESSAGE(label, "message bubble text not found");
    lv_obj_t *bubble = lv_obj_get_parent(label);
    TEST_ASSERT_NOT_NULL(bubble);

    uint32_t n = lv_obj_get_child_count(parent);
    lv_obj_t *strip = lv_obj_get_child(parent, n - 1);
    lv_area_t strip_a, bubble_a, label_a;
    lv_obj_get_coords(strip, &strip_a);
    lv_obj_get_coords(bubble, &bubble_a);
    lv_obj_get_coords(label, &label_a);

    TEST_ASSERT_FALSE_MESSAGE(areas_overlap(&strip_a, &label_a),
                              "banner must never overlap the thread's first bubble's actual TEXT");
    /* Not vacuous: the bubble's OWN background genuinely does sit under
     * the banner at this width/centre (mutation-sensitive — widening the
     * disjoint-text margin above without also checking this would let a
     * regression that pushes the banner low enough to reach the text
     * itself slip through, since a banner clear of the whole bubble box
     * trivially clears its text too). */
    TEST_ASSERT_TRUE_MESSAGE(areas_overlap(&strip_a, &bubble_a),
                             "test is vacuous unless the banner reaches the bubble's own background");
}

/* ---------------------------------------------------------------------
 * (b) Disjoint from the status bar row — puck-ux-usability-2026-09-15
 * finding 4, the reason this whole slice exists.
 *
 * Before slice 4: the strip was DELIBERATELY positioned to COVER the
 * status row (the maintainer decision `scr_banner.c` used to document at
 * the top of its layout-constants comment), on the theory that the
 * clock/mesh/battery row was the least valuable thing a transient banner
 * could hide. The review measured the result and found it wrong:
 * `banner_on_radar.png` showed `9:46` truncated, `LINKED` entirely gone,
 * and the battery reduced to a bare `%` — "the two facts a user checks
 * before trusting the device... are hidden by the notification that made
 * them look." Finding 4's fix moves the strip BELOW the row instead, so
 * these tests now assert the OPPOSITE of what they used to: never
 * overlapping the status text at all, not covering it fully.
 * ------------------------------------------------------------------- */

static void S26d_AC2_banner_disjoint_from_mesh_status_label(void)
{
    ff_radar_view_t r;
    make_radar_live(&r);
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, FF_THEME_WINDOW_PX, FF_THEME_WINDOW_PX);
    ff_scr_radar_build(parent, &r, false, false /* screen_flip (#158): banner tests use the unflipped glass */,
                        /*locked=*/false, NULL);

    ff_app_banner_t b;
    make_banner(&b);
    ff_scr_banner_build(parent, &b, false);
    lv_obj_update_layout(parent);

    lv_obj_t *mesh_lbl = find_label_exact(parent, "LINKED");
    TEST_ASSERT_NOT_NULL(mesh_lbl);

    uint32_t n = lv_obj_get_child_count(parent);
    lv_obj_t *strip = lv_obj_get_child(parent, n - 1);
    lv_area_t strip_a, mesh_a;
    lv_obj_get_coords(strip, &strip_a);
    lv_obj_get_coords(mesh_lbl, &mesh_a);

    TEST_ASSERT_FALSE_MESSAGE(areas_overlap(&strip_a, &mesh_a),
                              "banner must no longer overlap the LINKED status label (finding 4)");
}

/* The status TEXT's own measured y-band. The clock (finding 4 / item 3:
 * "status-row time in INK") is now FF_THEME_FONT_MSG_BODY (montserrat_16)
 * while MESH/battery stay FF_THEME_FONT_LABEL (montserrat_14), so the two
 * no longer share one exact y-range the way they did before this slice —
 * the band below is the UNION (min top, max bottom) of both, measured
 * directly off the rendered labels, not assumed. A strip whose own
 * y-range is entirely BELOW this band (banner.y1 > band.y2) cannot
 * overlap it anywhere along its width — no per-x variation to check. */
#define STATUS_TEXT_ROW_TOP_Y 37    /* measured: clock's real top at montserrat_16 */
#define STATUS_TEXT_ROW_BOTTOM_Y 54 /* measured: clock's real bottom at montserrat_16 (battery's is 53) */

static void S26d_AC2_banner_disjoint_from_status_text_row(void)
{
    ff_radar_view_t r;
    make_radar_live(&r);
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, FF_THEME_WINDOW_PX, FF_THEME_WINDOW_PX);
    ff_scr_radar_build(parent, &r, false, false /* screen_flip (#158): banner tests use the unflipped glass */,
                        /*locked=*/false, NULL);
    lv_obj_update_layout(parent);

    /* Cross-check the hardcoded band against the real rendered labels —
     * if scr_radar.c's status-row layout ever moves, this test fails
     * LOUDLY (NOT_NULL/message) rather than silently checking a stale
     * band against a strip that quietly stopped being disjoint from it. */
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
    /* The clock's own bottom edge is the binding one now that it's a
     * bigger font than battery/MESH (54 vs 53) — assert that, not
     * equality, since the two rows are no longer pinned to one baseline. */
    TEST_ASSERT_TRUE_MESSAGE(batt_a.y2 <= clock_a.y2, "battery's bottom must not exceed the clock's own (the binding edge)");

    ff_app_banner_t b;
    make_banner(&b);
    ff_scr_banner_build(parent, &b, false);
    lv_obj_update_layout(parent);

    uint32_t n = lv_obj_get_child_count(parent);
    lv_obj_t *strip = lv_obj_get_child(parent, n - 1);
    lv_area_t strip_a;
    lv_obj_get_coords(strip, &strip_a);

    char msg[128];
    snprintf(msg, sizeof(msg), "banner y-range [%d,%d] must sit entirely below the status-text band [%d,%d]",
             strip_a.y1, strip_a.y2, STATUS_TEXT_ROW_TOP_Y, STATUS_TEXT_ROW_BOTTOM_Y);
    TEST_ASSERT_TRUE_MESSAGE(strip_a.y1 > STATUS_TEXT_ROW_BOTTOM_Y, msg);
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
    /* Sized so its LEFT remainder beside the banner clears the masking
     * rule's floor in BOTH dimensions — 108x96 here. The floor moved from
     * FF_THEME_MIN_HIT_PX (44) to FF_THEME_HIT_PRIMARY_PX (80) with the
     * tap-target sizing pass (see ff_scr_nav_remainder_clears_floor), and
     * the old 300x48 at (60,36) left a 68x48 remainder — which stopped
     * being a "wide remainder" under the new rule and made this test
     * vacuous rather than wrong. Grown, not re-floored: the point of the
     * test is a control that genuinely SURVIVES masking.
     *
     * 200 wide, not 340: ff_scr_nav_rect_best_remainder picks the
     * largest-AREA slice, and at 340 the full-width slice BELOW the
     * banner (340x32) beat the left slice (108x96) on area while failing
     * the floor on height. Narrowing makes the left slice the genuine
     * best remainder, which is the shape this test is about. */
    lv_obj_set_size(wide, 200, 96);
    lv_obj_set_pos(wide, 20, 20); /* same y-band the banner sits in, MUCH wider in x */
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
                             "this control's remainder must clear the masking floor both ways to be meaningful");

    ff_scr_nav_mask_clickables_under_banner(parent, strip, &strip_a);

    TEST_ASSERT_TRUE_MESSAGE(lv_obj_has_flag(wide, LV_OBJ_FLAG_CLICKABLE),
                             "a control whose remainder clears the masking floor must stay clickable");

    /* Tap inside the LEFT slice (x=[20,strip_a.x1), y=[20,116)) — visible,
     * uncovered, and per the assertion above >= the floor in both
     * dimensions. */
    tap_at(60, 60);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_wide_clicks, "a real tap on the control's visible remainder must reach it");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S26d_AC2_banner_disjoint_from_radar_name_distance_stack);
    RUN_TEST(S26d_AC2_banner_disjoint_from_thread_first_bubble_TEXT);
    RUN_TEST(S26d_AC2_banner_disjoint_from_mesh_status_label);
    RUN_TEST(S26d_AC2_banner_disjoint_from_status_text_row);
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
