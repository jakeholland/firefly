/**
 * test_gesture_glue.c — S28 slice b: sim-side integration tests for the
 * on-glass gesture glue (app/ff_gesture_glue.c), driven through a REAL
 * `ff_ctl_loop_*` session (the same object main.c's `--headless --ctl
 * PORT` uses, and test_wakeonly_touch.c/test_ctl_quick_flare.c already
 * drive) with a genuine `lv_indev_t` press/move/release sequence — the
 * S16 c3 precedent this repo's own AGENTS.md/spec references, applied
 * here to a full multi-step DRAG rather than a stationary tap, since
 * `ff_gesture_glue_attach` is wired into `ff_ctl_loop_open` itself (S28
 * slice b) — the ONE shared place every headless ctl session gets it
 * for free, no test-local attach call needed.
 *
 * Test names mirror the spec's own AC numbering (AGENTS.md convention),
 * continuing from PR1's core-level S28_AC1..AC10: S28_AC11..AC18.
 *
 * THE PROXY, stated up front (AGENTS.md item 6): "the active face
 * changed" is satisfied even by a gesture handler that fires on ANY
 * touch, rim or not — AC12 is the negative control that drives the
 * SAME motion 60px inboard of the rim and asserts it does NOT close the
 * thread, AND that a vertical drag at that same inboard start point
 * still scrolls (the #130 regression this spec's own history section
 * names). AC14 is the analogous negative control for the launcher
 * (BACK/HOME-shaped touches there must not land on a tile). AC16 pairs
 * with AC15 the same way, for the long-press/interactive-widget gate.
 */
#include <string.h>

#include "unity.h"

#include "ctl_loop.h"
#include "ctl_server.h"
#include "ff_app_state.h"
#include "ff_idle.h"
#include "ff_intent.h"
#include "ff_shell.h"
#include "ff_theme.h"
#include "scr_nav.h" /* ff_scr_button_create — the real production button factory, for the wait_release regression test */

#include "fp_pack.h"

void setUp(void) {}

/* P0 harness-hang fix (debt/test-harness PR) — same convention every
 * ctl-driven test in this suite uses; see test_wakeonly_touch.c's own
 * tearDown comment for the full repro/verification writeup. */
void tearDown(void)
{
    if (lv_is_initialized()) {
        lv_deinit();
    }
}

/* Same recursive lookup every other ctl-driven test file in this suite
 * carries its own copy of (test_wakeonly_touch.c's own header comment
 * has the "duplicated rather than shared" reasoning). */
static lv_obj_t *find_button_with_label(lv_obj_t *root, char const *label_text)
{
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        if (lv_obj_check_type(child, &lv_button_class)) {
            uint32_t nc = lv_obj_get_child_count(child);
            for (uint32_t j = 0; j < nc; j++) {
                lv_obj_t *maybe_label = lv_obj_get_child(child, j);
                if (lv_obj_check_type(maybe_label, &lv_label_class)) {
                    char const *txt = lv_label_get_text(maybe_label);
                    if (txt != NULL && strcmp(txt, label_text) == 0) {
                        return child;
                    }
                }
            }
        }
        lv_obj_t *found = find_button_with_label(child, label_text);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

/* find_scrollable — the one PLAIN lv_obj carrying LV_OBJ_FLAG_SCROLLABLE
 * (the thread's own message list) — same identification trick
 * app/screens/tests/test_scr_intent.c's own find_scrollable uses (that
 * file's own doc comment has the full "why not just SCROLLABLE" case),
 * duplicated here rather than shared since the two files link entirely
 * different libraries. */
static lv_obj_t *find_scrollable(lv_obj_t *root)
{
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        if (lv_obj_check_type(child, &lv_obj_class) && lv_obj_has_flag(child, LV_OBJ_FLAG_SCROLLABLE)) {
            return child;
        }
        lv_obj_t *found = find_scrollable(child);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------
 * Gesture-drive helpers — a REAL press -> N move steps -> release
 * through ctx->pointer_point/pointer_state (the same fields ctl_loop.c's
 * own tap/swipe command handlers drive), each step 40ms of MOCK clock
 * apart (> LVGL's own 33ms default indev read period AND > the gesture
 * glue's own 20ms poll timer — see ff_gesture_glue.c's top comment on
 * why that timer stands in for LV_EVENT_PRESSING, which this LVGL
 * version never delivers to an indev-level listener). `ff_ctl_loop_open`
 * already attaches the gesture glue onto `ctx->pointer_indev` (S28
 * slice b) — nothing test-local to wire.
 * ------------------------------------------------------------------- */

static void gg_step(ff_ctl_loop_ctx_t *ctx, int32_t x, int32_t y, bool pressed)
{
    ctx->pointer_point.x = (lv_coord_t)x;
    ctx->pointer_point.y = (lv_coord_t)y;
    ctx->pointer_state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    ctx->mock_clock_ms += 40u;
    lv_timer_handler();
}

/* A straight-line drag from (x0,y0) to (x1,y1) over `steps` intermediate
 * points, then a release, then one settle pass so a dispatched intent's
 * effect is mirrored into ctx->state before the caller inspects it. */
static void gg_drag(ff_ctl_loop_ctx_t *ctx, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int steps)
{
    gg_step(ctx, x0, y0, true);
    for (int i = 1; i <= steps; i++) {
        int32_t const x = x0 + (x1 - x0) * i / steps;
        int32_t const y = y0 + (y1 - y0) * i / steps;
        gg_step(ctx, x, y, true);
    }
    gg_step(ctx, x1, y1, false);
    ff_ctl_loop_pump(ctx);
    lv_timer_handler();
    lv_refr_now(ctx->disp);
}

/* Holds a stationary press at (x,y) for `hold_ms` (mock clock), pumping
 * after every 40ms step so the glue's own poll timer and G3's
 * `ff_gesture_tick` both run repeatedly across the hold — does NOT
 * release; the caller inspects state (and/or extends the hold) with the
 * finger still down, then is responsible for its own release. */
static void gg_hold(ff_ctl_loop_ctx_t *ctx, int32_t x, int32_t y, uint32_t hold_ms)
{
    ctx->pointer_point.x = (lv_coord_t)x;
    ctx->pointer_point.y = (lv_coord_t)y;
    ctx->pointer_state = LV_INDEV_STATE_PRESSED;
    for (uint32_t elapsed = 0; elapsed < hold_ms; elapsed += 40u) {
        ctx->mock_clock_ms += 40u;
        lv_timer_handler();
        ff_ctl_loop_pump(ctx);
    }
    lv_refr_now(ctx->disp);
}

static void gg_release(ff_ctl_loop_ctx_t *ctx, int32_t x, int32_t y)
{
    gg_step(ctx, x, y, false);
    ff_ctl_loop_pump(ctx);
    lv_timer_handler();
}

/* Left-rim / bottom-rim points, per docs/specs/S28-gestures.md's own
 * geometry (FF_THEME_GLASS_CX/CY/R = 208/206/200, rim_px = 28): left
 * rim zone is x <= 36, bottom rim zone is y >= 378. */
#define GG_LEFT_RIM_X 20
#define GG_BOTTOM_RIM_Y 390
#define GG_CENTER_X FF_THEME_GLASS_CX
#define GG_CENTER_Y FF_THEME_GLASS_CY

/* Shared session bring-up: opens a real ctl session, installs the
 * pointer read_cb + gesture glue (both inside ff_ctl_loop_open/
 * ff_ctl_loop_handlers), settles the always-dirty first tick, and lands
 * on the launcher (S26e boot default). */
static void open_session(ff_shell_t *shell, fp_pack_t *pack, ff_ctl_loop_ctx_t *ctx, ff_ctl_handlers_t *out_h)
{
    ff_shell_cfg_t shell_cfg;
    memset(&shell_cfg, 0, sizeof(shell_cfg));

    ff_ctl_loop_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mock_clock = true;

    TEST_ASSERT_EQUAL_INT(0, ff_ctl_loop_open(ctx, shell, pack, &shell_cfg, &cfg));
    bool quit_flag = false;
    *out_h = ff_ctl_loop_handlers(ctx, &quit_flag);

    ff_ctl_loop_pump(ctx);
    lv_refr_now(ctx->disp);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ctx->state.active_face);
}

/* Navigates straight to `face` via the launcher-select intent (an
 * ARRANGE step, not what any AC below tests) and settles. */
static void goto_face(ff_ctl_loop_ctx_t *ctx, uint8_t launcher_idx)
{
    ff_intent_t const sel = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {.launcher_idx = launcher_idx}};
    ff_shell_intent(ctx->shell, &sel);
    ff_ctl_loop_pump(ctx);
    lv_timer_handler();
    lv_refr_now(ctx->disp);
}

/* ===================================================================
 * AC11 / AC12 — Inbox thread: left-rim BACK closes it; the same drag
 * 60px inboard does not (and a vertical drag there still scrolls — the
 * #130 regression guard).
 * =================================================================== */

enum { GG_KEV = 0x0000BEEFu, GG_MY_ID = 0x00001111u };

/* Pairs KEV and pushes enough CREW-broadcast messages to overflow the
 * thread's own scroll band (S24's own 12-message fixture, here pushed
 * through the real events seam instead of hand-built view fields), then
 * opens the CREW thread. */
static void arrange_open_overflowing_crew_thread(ff_ctl_loop_ctx_t *ctx)
{
    mc_events_t const ev = ff_shell_events(ctx->shell);
    ev.on_my_info(ev.user, GG_MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(ctx->shell, GG_KEV, true));
    mc_nodeinfo_t n;
    memset(&n, 0, sizeof(n));
    n.node_num = GG_KEV;
    n.has_short_name = true;
    strncpy(n.short_name, "KEV", sizeof(n.short_name) - 1);
    ev.on_node(ev.user, &n);

    for (int i = 0; i < 12; i++) {
        char text[32];
        snprintf(text, sizeof(text), "signal number %d checking in", i);
        ev.on_text(ev.user, GG_KEV, MC_ADDR_BROADCAST, text, strlen(text));
    }

    goto_face(ctx, 2 /* Signals/Inbox */);
    ff_intent_t const open = {.kind = FF_INTENT_INBOX_OPEN_THREAD, .u = {.node_id = 0u}};
    ff_shell_intent(ctx->shell, &open);
    ff_ctl_loop_pump(ctx);
    lv_timer_handler();
    lv_refr_now(ctx->disp);

    TEST_ASSERT_EQUAL(FF_APP_FACE_INBOX, ctx->state.active_face);
    TEST_ASSERT_EQUAL(FF_INBOX_SUB_THREAD, ctx->state.inbox.subview);
}

static void S28_AC11_left_rim_drag_in_open_thread_closes_it(void)
{
    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_ctl_loop_ctx_t ctx;
    ff_ctl_handlers_t h;
    open_session(&shell, &pack, &ctx, &h);
    arrange_open_overflowing_crew_thread(&ctx);

    /* Left-rim swipe right: DOWN at x=20 (<= the 36px rim boundary),
     * dx=60 well past the 56px threshold, |dy|=0. */
    gg_drag(&ctx, GG_LEFT_RIM_X, GG_CENTER_Y, GG_LEFT_RIM_X + 70, GG_CENTER_Y, 5);

    /* THE PROXY this AC's own review round caught (AGENTS.md item 6):
     * checking `inbox.subview` alone is satisfied even by a BACK that
     * navigates all the way HOME (rule 4's "any other base face") rather
     * than the spec's actual "back to the list" (rule 2) — leaving
     * INBOX entirely stops `ctx.state.inbox` from being repopulated as
     * INBOX's own subview at all, which this one field can't tell apart
     * from a genuine close. Asserting `active_face` stays INBOX closes
     * that gap. */
    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_INBOX, ctx.state.active_face,
                               "a left-rim BACK swipe on an open thread must stay on the Inbox face, not go home");
    TEST_ASSERT_EQUAL_MESSAGE(FF_INBOX_SUB_INBOX, ctx.state.inbox.subview,
                               "a left-rim BACK swipe did not close the open thread");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

static void S28_AC12_swipe_60px_inboard_does_not_close_thread_but_still_scrolls(void)
{
    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_ctl_loop_ctx_t ctx;
    ff_ctl_handlers_t h;
    open_session(&shell, &pack, &ctx, &h);
    arrange_open_overflowing_crew_thread(&ctx);

    int32_t const inboard_x = GG_LEFT_RIM_X + 60; /* well past the 36px rim zone */

    /* The exact same horizontal motion as AC11, just started 60px
     * further in — must NOT close the thread (mutation guard: the same
     * failure a flipped/missing rim-zone check in the glue would let
     * through). */
    gg_drag(&ctx, inboard_x, GG_CENTER_Y, inboard_x + 70, GG_CENTER_Y, 5);
    TEST_ASSERT_EQUAL_MESSAGE(FF_INBOX_SUB_THREAD, ctx.state.inbox.subview,
                               "a drag starting inboard of the rim wrongly closed the thread");

    /* The #130 regression guard: a VERTICAL drag at that SAME inboard
     * start point over the thread's own message list must still scroll
     * it (not be eaten as a gesture candidate, and not be mistaken for
     * a row-open tap — the thread has none). */
    lv_obj_t *list = find_scrollable(lv_screen_active());
    TEST_ASSERT_NOT_NULL(list);
    lv_obj_update_layout(list);
    int32_t const max_scroll = lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list);
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(0, max_scroll, "thread fixture must actually overflow to be a real scroll test");
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    TEST_ASSERT_EQUAL_INT32(0, lv_obj_get_scroll_y(list));

    lv_area_t list_area;
    lv_obj_get_coords(list, &list_area);
    int32_t const list_top = list_area.y1;
    int32_t const list_x = (list_area.x1 + list_area.x2) / 2;
    gg_drag(&ctx, list_x, list_top + 90, list_x, list_top + 10, 5);

    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(0, lv_obj_get_scroll_y(list), "a vertical drag inside the thread must still scroll it");
    TEST_ASSERT_EQUAL_MESSAGE(FF_INBOX_SUB_THREAD, ctx.state.inbox.subview, "the vertical scroll drag must not also close the thread");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* ===================================================================
 * lv_indev_wait_release regression (review round 2 on PR #200 finding
 * 3) — a BACK recognition mid-drag must swallow the REST of that
 * gesture for whatever real widget the finger started on, per
 * ff_gesture_glue.h's own documented contract ("No click, no scroll
 * continues for the widget under the finger").
 *
 * Real production screens never place a genuinely clickable widget
 * INSIDE the rim zone itself: round-safe framing keeps interactive
 * content well off the very edge by construction (measured directly —
 * the Inbox list's own left margin is 62px in practice, comfortably
 * outside the 36px rim), so there is no existing production widget
 * this test could drive through unmodified. Builds one REAL button
 * instead — `ff_scr_button_create`, the exact same factory every
 * screen uses (app/screens/scr_nav.c), not a bespoke stand-in — sized
 * wide enough that BOTH the DOWN point and the drag's END point land
 * inside its own bounds throughout. That last part is what actually
 * isolates this guarantee: every OTHER drag test in this file ends
 * OUTSIDE whatever it started on, which the pre-existing #145/#148
 * PRESS_LOCK-cleared-click fix already suppresses on its own (reviewer
 * finding 3's own mutation proof: dropping `lv_indev_wait_release`
 * entirely did not fail any of those 70 tests) — a release still
 * within the SAME widget's bounds is the one case where LVGL's default
 * click semantics would otherwise still fire CLICKED.
 * =================================================================== */

static bool s_wait_release_probe_clicked;

static void wait_release_probe_click_cb(lv_event_t *e)
{
    (void)e;
    s_wait_release_probe_clicked = true;
}

static void S28_wait_release_swallows_click_on_a_real_widget_under_the_swipe(void)
{
    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_ctl_loop_ctx_t ctx;
    ff_ctl_handlers_t h;
    open_session(&shell, &pack, &ctx, &h);
    goto_face(&ctx, 0 /* Radar */);
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, ctx.state.active_face);

    s_wait_release_probe_clicked = false;
    lv_obj_t *probe = ff_scr_button_create(lv_screen_active());
    lv_obj_set_pos(probe, 8, 186);
    lv_obj_set_size(probe, 212, 40); /* spans x=[8,220] on screen — the whole drag below stays inside it */
    lv_obj_add_event_cb(probe, wait_release_probe_click_cb, LV_EVENT_CLICKED, NULL);
    /* lv_obj_get_click_area (and hence LVGL's own hit-testing) reads
     * cached coords that are only computed by a layout pass — without
     * forcing one here, the very first touch sample can hit-test
     * against stale (pre-creation) coordinates and miss this brand-new
     * object entirely, which would make this test pass FOR THE WRONG
     * REASON (the probe never gets pressed at all, mutation or not —
     * this is the exact gap that made an early draft of this test pass
     * even with `lv_indev_wait_release` removed). Same "force it before
     * reading/relying on real coords" discipline test_ctl_flare_
     * sequence.c's own `ctl_settle`/`ctl_tap_button` already document. */
    lv_obj_update_layout(probe);

    /* The same left-rim BACK motion AC11/AC18 use — DOWN at x=20
     * (inside both the rim zone and the probe button), dragged to x=90
     * (dx=70, still well inside the probe's own [8,220] span). */
    gg_drag(&ctx, GG_LEFT_RIM_X, GG_CENTER_Y, GG_LEFT_RIM_X + 70, GG_CENTER_Y, 5);

    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_LAUNCHER, ctx.state.active_face,
                               "setup failed: the BACK gesture did not recognise (rule 4 -> HOME on Radar)");
    TEST_ASSERT_FALSE_MESSAGE(s_wait_release_probe_clicked,
                               "a widget under a recognised BACK swipe received CLICKED -- lv_indev_wait_release did not swallow it");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* ===================================================================
 * AC13 / AC14 — bottom-rim swipe HOME on Radar lands on the launcher;
 * the same swipe on the launcher itself is a no-op (no tile selected).
 * =================================================================== */

static void S28_AC13_bottom_rim_swipe_on_radar_lands_on_launcher(void)
{
    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_ctl_loop_ctx_t ctx;
    ff_ctl_handlers_t h;
    open_session(&shell, &pack, &ctx, &h);
    goto_face(&ctx, 0 /* Radar */);
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, ctx.state.active_face);

    /* Bottom-rim swipe up: DOWN at y=390 (>= the 378px rim boundary),
     * up=70 past the 64px threshold, |dx|=0. */
    gg_drag(&ctx, GG_CENTER_X, GG_BOTTOM_RIM_Y, GG_CENTER_X, GG_BOTTOM_RIM_Y - 80, 5);

    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_LAUNCHER, ctx.state.active_face,
                               "a bottom-rim HOME swipe on Radar did not land on the launcher");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

static void S28_AC14_rim_swipe_on_launcher_is_a_no_op(void)
{
    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_ctl_loop_ctx_t ctx;
    ff_ctl_handlers_t h;
    open_session(&shell, &pack, &ctx, &h);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ctx.state.active_face);

    /* The identical bottom-rim swipe-up as AC13, but starting already
     * on the launcher (spec rule 5: LAUNCHER -> no-op) — must not select
     * a tile (active_face stays LAUNCHER) even though the raw motion is
     * a completed HOME-shaped gesture. */
    gg_drag(&ctx, GG_CENTER_X, GG_BOTTOM_RIM_Y, GG_CENTER_X, GG_BOTTOM_RIM_Y - 80, 5);

    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_LAUNCHER, ctx.state.active_face,
                               "a rim swipe on the launcher must stay a no-op, not select a tile");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* ===================================================================
 * AC15 / AC16 — long-press on empty Radar arms the flare countdown; the
 * identical long-press on a REAL Radar button (the FLARE button, once a
 * close crew member makes it render) does not.
 * =================================================================== */

static void S28_AC15_long_press_on_empty_radar_arms_flare_countdown(void)
{
    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_ctl_loop_ctx_t ctx;
    ff_ctl_handlers_t h;
    open_session(&shell, &pack, &ctx, &h);
    goto_face(&ctx, 0 /* Radar */);
    /* No crew paired: RADAR_NOSEL renders only two labels — genuinely
     * empty glass, no widget under the finger at all. */
    TEST_ASSERT_FALSE(ctx.state.flare.sending);

    gg_hold(&ctx, GG_CENTER_X, GG_CENTER_Y, 1250 /* > the 1200ms long-press threshold */);

    TEST_ASSERT_TRUE_MESSAGE(ctx.state.flare.sending, "a long press on empty Radar did not arm the flare countdown");

    /* Release before inspecting the tree: a rebuild is deferred while a
     * finger is down (the rebuild-mid-tap latch every target/ctl_loop
     * shares — sim_lifecycle.h's own doc comment), so the sender
     * overlay's CANCEL button does not actually land in the live LVGL
     * tree until the touch lifts. Same proxy-guard discipline
     * test_ctl_quick_flare.c already applies to this exact fact
     * (AGENTS.md item 6): the flag alone is satisfied even by a shell
     * that flips it with no screen behind it. */
    gg_release(&ctx, GG_CENTER_X, GG_CENTER_Y);
    TEST_ASSERT_NOT_NULL_MESSAGE(find_button_with_label(lv_screen_active(), "CANCEL"),
                                  "flare.sending is true but the sender overlay's CANCEL button is not on screen");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

static void S28_AC16_long_press_on_a_radar_button_does_not_flare(void)
{
    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_ctl_loop_ctx_t ctx;
    ff_ctl_handlers_t h;
    open_session(&shell, &pack, &ctx, &h);
    goto_face(&ctx, 0 /* Radar */);

    /* RADAR_CLOSE (and its FLARE button) needs a paired member with a
     * live, strong-RSSI DIRECT packet — ff_crew_close_range's own
     * criteria (app/tests/test_shell.c's own RADAR_CLOSE setup uses the
     * identical shape: pair, then one strong DIRECT rx_meta sample). */
    mc_events_t const ev = ff_shell_events(ctx.shell);
    ev.on_my_info(ev.user, GG_MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(ctx.shell, GG_KEV, true));
    ff_shell_set_heading(ctx.shell, 0.0f);
    ff_shell_set_my_pos(ctx.shell, (ff_latlon_t){39.936, -82.414}); /* my own fix must be known too (app/tests/test_shell.c's own RADAR_CLOSE setup) */
    mc_rx_meta_t m;
    memset(&m, 0, sizeof(m));
    m.rx_path = MC_RX_PATH_DIRECT;
    m.has_rssi = true;
    m.rssi_dbm = -30;
    ev.on_rx_meta(ev.user, GG_KEV, &m);
    ff_ctl_loop_pump(&ctx);
    lv_timer_handler();
    lv_refr_now(ctx.disp);
    TEST_ASSERT_EQUAL_MESSAGE(RADAR_CLOSE, ctx.state.radar.mode, "setup failed to reach RADAR_CLOSE");

    lv_obj_t *flare_btn = find_button_with_label(lv_screen_active(), "FLARE");
    TEST_ASSERT_NOT_NULL_MESSAGE(flare_btn, "RADAR_CLOSE's FLARE button not found — is scr_radar.c still building it?");
    lv_area_t area;
    lv_obj_get_click_area(flare_btn, &area);
    int32_t const cx = (area.x1 + area.x2) / 2;
    int32_t const cy = (area.y1 + area.y2) / 2;

    gg_hold(&ctx, cx, cy, 1250);

    TEST_ASSERT_FALSE_MESSAGE(ctx.state.flare.sending,
                               "a long press on the FLARE button itself must not ALSO arm the quick-flare gesture");

    gg_release(&ctx, cx, cy);
    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* ===================================================================
 * AC17 — during the flare (send) countdown, a bottom-rim swipe does
 * nothing.
 * =================================================================== */

static void S28_AC17_bottom_rim_swipe_during_flare_countdown_does_nothing(void)
{
    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_ctl_loop_ctx_t ctx;
    ff_ctl_handlers_t h;
    open_session(&shell, &pack, &ctx, &h);
    goto_face(&ctx, 0 /* Radar */);

    ff_intent_t const start = {.kind = FF_INTENT_FLARE_START, .u = {0}};
    ff_shell_intent(ctx.shell, &start);
    ff_ctl_loop_pump(&ctx);
    lv_timer_handler();
    lv_refr_now(ctx.disp);
    TEST_ASSERT_TRUE_MESSAGE(ctx.state.flare.sending, "setup failed to start the flare send");

    /* The exact same bottom-rim HOME swipe as AC13 — must do nothing at
     * all while the countdown is up: no navigation, still sending. */
    gg_drag(&ctx, GG_CENTER_X, GG_BOTTOM_RIM_Y, GG_CENTER_X, GG_BOTTOM_RIM_Y - 80, 5);

    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_RADAR, ctx.state.active_face,
                               "a bottom-rim swipe during the flare countdown must not navigate");
    TEST_ASSERT_TRUE_MESSAGE(ctx.state.flare.sending, "the flare countdown must still be running");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* ===================================================================
 * AC18 — BACK on a Settings (sub-)page steps home; BACK on Lineup steps
 * home too.
 *
 * Interpretation note (see the PR body for the full writeup): S21
 * (docs/specs/S21-settings-rework.md) already removed the settings
 * sub-page concept entirely — Settings is one continuously-scrolling
 * list with no page-stepping view state left (ff_shell.c's own comment
 * on this, next to its settings fields, has the full history). So
 * "BACK on Settings sub-page -> root page" has no sub-page state to act
 * on today; the observable behavior this test pins is what the shell's
 * rule ladder actually falls through to in that case — rule (4), "any
 * other base face -> HOME" — the same rule this test's Lineup half
 * exercises directly.
 * =================================================================== */

static void S28_AC18_back_on_settings_and_lineup_goes_home(void)
{
    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_ctl_loop_ctx_t ctx;
    ff_ctl_handlers_t h;
    open_session(&shell, &pack, &ctx, &h);

    goto_face(&ctx, 4 /* Settings */);
    TEST_ASSERT_EQUAL(FF_APP_FACE_SETTINGS, ctx.state.active_face);
    gg_drag(&ctx, GG_LEFT_RIM_X, GG_CENTER_Y, GG_LEFT_RIM_X + 70, GG_CENTER_Y, 5);
    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_LAUNCHER, ctx.state.active_face, "BACK on Settings did not go home");

    goto_face(&ctx, 1 /* Lineup (Now) */);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LINEUP, ctx.state.active_face);
    gg_drag(&ctx, GG_LEFT_RIM_X, GG_CENTER_Y, GG_LEFT_RIM_X + 70, GG_CENTER_Y, 5);
    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_LAUNCHER, ctx.state.active_face, "BACK on Lineup did not go home");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* Permanent regression guard for the tearDown fix above (debt/test-
 * harness PR), same pin test_wakeonly_touch.c carries. */
static void tearDown_is_idempotent_after_lv_init(void)
{
    lv_init();
    TEST_ASSERT_TRUE(lv_is_initialized());
    tearDown();
    TEST_ASSERT_FALSE(lv_is_initialized());
    tearDown();
    TEST_ASSERT_FALSE(lv_is_initialized());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S28_AC11_left_rim_drag_in_open_thread_closes_it);
    RUN_TEST(S28_AC12_swipe_60px_inboard_does_not_close_thread_but_still_scrolls);
    RUN_TEST(S28_wait_release_swallows_click_on_a_real_widget_under_the_swipe);
    RUN_TEST(S28_AC13_bottom_rim_swipe_on_radar_lands_on_launcher);
    RUN_TEST(S28_AC14_rim_swipe_on_launcher_is_a_no_op);
    RUN_TEST(S28_AC15_long_press_on_empty_radar_arms_flare_countdown);
    RUN_TEST(S28_AC16_long_press_on_a_radar_button_does_not_flare);
    RUN_TEST(S28_AC17_bottom_rim_swipe_during_flare_countdown_does_nothing);
    RUN_TEST(S28_AC18_back_on_settings_and_lineup_goes_home);
    RUN_TEST(tearDown_is_idempotent_after_lv_init);
    return UNITY_END();
}
