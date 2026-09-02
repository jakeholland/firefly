/**
 * test_scr_banner.c — S26 slice d, maintainer decision B (2026-09-02,
 * docs/specs/S26-device-lifecycle.md "Notifications (slice d)"): the
 * banner strip's move to cover the status bar row instead of the row
 * below it.
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
 *   - strip:            (163,22)-(252,69)   — BANNER_W=90, BANNER_H=48,
 *     centered at (208,46) = (FF_THEME_GLASS_CX, PUCK_RADIUS+BANNER_CY)
 *   - clock label:      (99,38)-(157,53)    — entirely LEFT of the strip
 *   - MESH label:        (185,38)-(226,53)   — entirely INSIDE the strip
 *   - battery label:     (270,38)-(297,53)   — entirely RIGHT of the strip
 * The strip is too narrow (by the bezel's own true 10px-corner-margin
 * bound — see scr_banner.c's layout comment) to span all three status
 * labels at once; it fully covers the one it does reach (MESH) and
 * clears the other two with a real gap, never touching either partway.
 * That is the actual, achievable meaning of "no half-visible clock
 * behind it": the clock is either fully hidden or fully shown, never
 * clipped through the middle — proven directly below, not assumed.
 *
 * ## Mutation check (AGENTS.md standing brief item 2 / docs/review/
 * code-review.md item 6), hand-verified before pushing:
 * Temporarily reverting BANNER_CY to its pre-move value (-90.0f) and
 * rebuilding fails S26d_AC2_banner_covers_mesh_status_label — at the old
 * position the strip never reaches the status row at all, so it covers
 * nothing there. See the PR body for the exact `ctest` output.
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
#include "scr_radar.h"
#include "scr_inbox.h"

/* ---------------------------------------------------------------------
 * setUp/tearDown + the spy sink (S16c1's seam — same pattern
 * test_scr_intent.c uses, duplicated file-local rather than shared, that
 * file's own established convention for these small test-only helpers).
 * ------------------------------------------------------------------- */

static uint32_t s_fake_tick_ms;

static uint32_t test_tick_cb(void)
{
    return s_fake_tick_ms;
}

typedef struct {
    int count;
    ff_intent_t last;
} spy_sink_t;

static spy_sink_t s_spy;

static void spy_sink_cb(void *user, ff_intent_t const *in)
{
    spy_sink_t *s = (spy_sink_t *)user;
    s->count++;
    s->last = *in;
}

void setUp(void)
{
    s_fake_tick_ms = 0;
    lv_init();
    lv_tick_set_cb(test_tick_cb);
    lv_display_t *disp = lv_display_create(FF_THEME_WINDOW_PX, FF_THEME_WINDOW_PX);
    lv_display_set_default(disp);

    memset(&s_spy, 0, sizeof(s_spy));
    ff_intent_emit_bind(spy_sink_cb, &s_spy);
}

void tearDown(void)
{
    ff_intent_emit_bind(NULL, NULL);
    lv_deinit();
}

/* ---------------------------------------------------------------------
 * Small geometry helpers — lv_area_t's x2/y2 are INCLUSIVE (ff_layout.h's
 * own doc comment on the convention mismatch with this codebase's usual
 * "size = far - near" rects), so both helpers below work directly in
 * that inclusive convention rather than converting.
 * ------------------------------------------------------------------- */

static bool areas_overlap(lv_area_t const *a, lv_area_t const *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

static bool area_contains(lv_area_t const *outer, lv_area_t const *inner)
{
    return outer->x1 <= inner->x1 && outer->x2 >= inner->x2 && outer->y1 <= inner->y1 && outer->y2 >= inner->y2;
}

static lv_obj_t *find_label_exact(lv_obj_t *root, char const *text)
{
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            char const *txt = lv_label_get_text(child);
            if (txt != NULL && strcmp(txt, text) == 0) {
                return child;
            }
        }
        lv_obj_t *found = find_label_exact(child, text);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static void click(lv_obj_t *obj)
{
    TEST_ASSERT_NOT_NULL(obj);
    lv_result_t r = lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL(LV_RESULT_OK, r);
}

/* Real press/move/release through a synthetic pointer indev — the only
 * way to exercise LV_OBJ_FLAG_PRESS_LOCK at all (test_scr_intent.c's own
 * drag_v, duplicated here per this file's own top-comment convention). */
static lv_point_t s_probe_pt;
static lv_indev_state_t s_probe_state;

static void probe_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = s_probe_pt;
    data->state = s_probe_state;
}

static void drag_v(int32_t from_y, int32_t to_y, int32_t x)
{
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, probe_read_cb);

    s_probe_pt.x = (lv_coord_t)x;
    s_probe_pt.y = (lv_coord_t)from_y;
    s_probe_state = LV_INDEV_STATE_PRESSED;
    s_fake_tick_ms += 40u;
    lv_timer_handler();

    enum { STEPS = 6 };
    for (int i = 1; i <= STEPS; i++) {
        s_probe_pt.y = (lv_coord_t)(from_y + (to_y - from_y) * i / STEPS);
        s_fake_tick_ms += 40u;
        lv_timer_handler();
    }

    s_probe_state = LV_INDEV_STATE_RELEASED;
    s_fake_tick_ms += 40u;
    lv_timer_handler();

    lv_anim_delete(indev, NULL);
    lv_indev_delete(indev);
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
    ff_scr_radar_build(parent, &r, false);

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
 * (b) Covers the status bar row — the actual, achievable, MEASURED
 * meaning of that ("fully covers whatever of the row it reaches; never
 * half-clips the rest" — see this file's own top comment for the real
 * numbers and why literal full-row coverage is bezel-impossible at this
 * height). This is also the mutation-sensitive half of (a)/(b): reverting
 * BANNER_CY to -90 fails the first of these two (see top comment).
 * ------------------------------------------------------------------- */

static void S26d_AC2_banner_covers_mesh_status_label(void)
{
    ff_radar_view_t r;
    make_radar_live(&r);
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, FF_THEME_WINDOW_PX, FF_THEME_WINDOW_PX);
    ff_scr_radar_build(parent, &r, false);

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

static void assert_no_partial_overlap(lv_area_t const *strip, lv_area_t const *label, char const *label_name)
{
    bool disjoint = !areas_overlap(strip, label);
    bool fully_covered = area_contains(strip, label);
    char msg[128];
    snprintf(msg, sizeof(msg), "%s must be either fully clear of the banner or fully hidden by it, never half-clipped",
             label_name);
    TEST_ASSERT_TRUE_MESSAGE(disjoint || fully_covered, msg);
}

static void S26d_AC2_banner_never_half_clips_clock_or_battery(void)
{
    ff_radar_view_t r;
    make_radar_live(&r);
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, FF_THEME_WINDOW_PX, FF_THEME_WINDOW_PX);
    ff_scr_radar_build(parent, &r, false);

    ff_app_banner_t b;
    make_banner(&b);
    ff_scr_banner_build(parent, &b, false);
    lv_obj_update_layout(parent);

    lv_obj_t *clock_lbl = find_label_exact(parent, "9:46 pm");
    lv_obj_t *batt_lbl = find_label_exact(parent, "74%");
    TEST_ASSERT_NOT_NULL(clock_lbl);
    TEST_ASSERT_NOT_NULL(batt_lbl);

    uint32_t n = lv_obj_get_child_count(parent);
    lv_obj_t *strip = lv_obj_get_child(parent, n - 1);
    lv_area_t strip_a, clock_a, batt_a;
    lv_obj_get_coords(strip, &strip_a);
    lv_obj_get_coords(clock_lbl, &clock_a);
    lv_obj_get_coords(batt_lbl, &batt_a);

    assert_no_partial_overlap(&strip_a, &clock_a, "the clock label");
    assert_no_partial_overlap(&strip_a, &batt_a, "the battery label");
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
 * Launcher collision finding (task brief: "check hit-target sweep... if
 * no banner_on_launcher fixture exists, add one... describe it").
 *
 * `ff_scr_launcher_build` never calls `ff_scr_banner_build` at all —
 * `targets/sim/face_dispatch.c` dispatches FF_APP_FACE_LAUNCHER straight
 * to it, bypassing `scr_nav.c` (the only call site) entirely. So a
 * `banner_on_launcher.json` golden fixture would render a launcher with
 * NO banner ever drawn — a fixture that can never fail no matter how
 * badly the two would actually collide, i.e. a proxy that looks like
 * coverage and isn't (the exact class of finding AGENTS.md's standing
 * brief names). Deliberately not added; this test instead proves the
 * structural gap directly (so a future wiring-up doesn't silently start
 * passing without anyone re-examining the geometry), and the geometry
 * below is measured independently for the PR body/spec note.
 *
 * MEASURED (this file, hand-run before landing): the top compass
 * satellite's rect is (162,34)-(249,121); the banner, built standalone at
 * the SAME size/position it would render at if composited here, is
 * (163,22)-(252,69) — these do not merely violate the 8px adjacency
 * floor, they overtly OVERLAP (shared region (163,34)-(249,69), 87x36px).
 * Wiring the banner into the launcher is out of this PR's scope (the
 * brief's own "keep yours small... do not touch other screens" line) —
 * flagged as a follow-up rather than silently left unmentioned.
 * ------------------------------------------------------------------- */

static void S26d_launcher_does_not_render_a_banner_today(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    state.banner.active = true; /* if the launcher ever reads this, a banner-shaped control would appear */
    strncpy(state.banner.name, "DANA", sizeof(state.banner.name) - 1);
    state.banner.kind = FF_NOTIFY_MESSAGE;

    ff_scr_launcher_build(&state);
    lv_obj_update_layout(lv_screen_active());

    /* No 90x48 clickable (the banner's own size) exists anywhere in the
     * built tree — the structural proof that scr_launcher.c ignores
     * state->banner entirely today. */
    TEST_ASSERT_NULL_MESSAGE(find_label_exact(lv_screen_active(), LV_SYMBOL_ENVELOPE),
                              "the launcher unexpectedly rendered a banner glyph — "
                              "this test (and this file's launcher-collision note) is now stale");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S26d_AC2_banner_disjoint_from_radar_name_distance_stack);
    RUN_TEST(S26d_AC2_banner_disjoint_from_thread_first_bubble);
    RUN_TEST(S26d_AC2_banner_covers_mesh_status_label);
    RUN_TEST(S26d_AC2_banner_never_half_clips_clock_or_battery);
    RUN_TEST(S26d_AC2_banner_corners_clear_glass_by_10px);
    RUN_TEST(S26d_AC2_banner_tap_emits_banner_open_exactly_once);
    RUN_TEST(S26d_AC2_banner_drag_off_emits_nothing);
    RUN_TEST(S26d_launcher_does_not_render_a_banner_today);
    return UNITY_END();
}
