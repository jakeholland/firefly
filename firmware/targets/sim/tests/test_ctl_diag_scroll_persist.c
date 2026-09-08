/**
 * test_ctl_diag_scroll_persist.c — fix/diag-scroll-persist.
 *
 * Settings -> "DIAGNOSTICS" (PR #229, scr_settings.c's
 * settings_build_diag_page) could not be scrolled: a drag worked, then
 * the page snapped back to the top. Root cause, verified against
 * `shell_render_key` (ff_shell.c) and the page's own build function
 * before this fix: `settings_build_diag_page` unconditionally called
 * `lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF)` at the end of EVERY build —
 * so any render-key-dirtying rebuild snapped the list back to the top,
 * not just the page's own excess churn. Two REAL sources dirty that key
 * routinely while the page is open: the device-stats push app_main.c
 * makes every `FF_DEVICE_STATS_SAMPLE_PERIOD_MS` (2s) — a legitimate,
 * honestly-rendered change (byte-precision free heap) that SHOULD
 * rebuild the page — and (fixed alongside this test, see
 * app/tests/test_shell.c's `S_diag_heading_keys_rendered_whole_degree_
 * only`) sub-degree compass noise re-sampled every 100ms, which should
 * NOT have dirtied the key at all (it rendered no different pixel) but
 * did before the render-key coarsening fix. Either one, unguarded by
 * the screen's own hardcoded reset, could land mid-drag.
 *
 * This file proves the SCROLL half of the fix end to end: a REAL
 * ctl-loop session (the same object main.c's `--headless --ctl PORT`
 * uses), a genuine multi-step drag on the live DIAGNOSTICS list, then a
 * genuine `ff_shell_set_device_stats` push — the exact call app_main.c
 * makes on its own timer — and asserts (a) that push really does dirty
 * the key and rebuild the page (the proxy every rebuild test in this
 * suite guards against: "no scroll loss" is trivially true if nothing
 * was ever dirty) and (b) the scroll offset the drag left survives that
 * rebuild instead of snapping back to the top.
 */
#include <string.h>

#include "unity.h"

#include "ctl_loop.h"
#include "ctl_server.h"
#include "ff_app_state.h"
#include "ff_intent.h"
#include "ff_shell.h"

#include "fp_pack.h"

void setUp(void) {}

/* P0 harness-hang fix (debt/test-harness PR) — same convention every
 * ctl-driven test file in this suite uses (test_wakeonly_touch.c's own
 * tearDown comment has the full repro/rationale): each test owns its
 * own lv_init()/lv_deinit() pairing via ff_ctl_loop_open/lv_deinit(),
 * this is only the safety net for an early TEST_ASSERT failure. */
void tearDown(void)
{
    if (lv_is_initialized()) {
        lv_deinit();
    }
}

/* Same recursive scrollable-finder every ctl-driven test file in this
 * suite carries its own copy of (test_gesture_glue.c's own header
 * comment: these files link entirely different libraries, so a shared
 * header is not worth it for one small helper). */
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

/* A real press -> N move steps -> release, driven straight through
 * ctx->pointer_point/pointer_state (the same fields ctl_loop.c's own
 * tap/swipe command handlers drive) — same shape test_gesture_glue.c's
 * own gg_step/gg_drag use, duplicated here per this suite's own
 * "no shared drag helper across files" convention (see find_scrollable's
 * comment just above). */
static void diag_drag_step(ff_ctl_loop_ctx_t *ctx, int32_t x, int32_t y, bool pressed)
{
    ctx->pointer_point.x = (lv_coord_t)x;
    ctx->pointer_point.y = (lv_coord_t)y;
    ctx->pointer_state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    ctx->mock_clock_ms += 40u; /* > LVGL's 33ms default indev read period */
    lv_timer_handler();
}

static void diag_drag(ff_ctl_loop_ctx_t *ctx, int32_t x, int32_t y0, int32_t y1, int steps)
{
    diag_drag_step(ctx, x, y0, true);
    for (int i = 1; i <= steps; i++) {
        int32_t const y = y0 + (y1 - y0) * i / steps;
        diag_drag_step(ctx, x, y, true);
    }
    diag_drag_step(ctx, x, y1, false);
    ff_ctl_loop_pump(ctx);
    lv_timer_handler();
    lv_refr_now(ctx->disp);
}

/* Opens a real ctl session and lands on the launcher (S26e boot
 * default) — same bring-up every ctl-driven test file in this suite
 * uses. */
static void open_session(ff_shell_t *shell, fp_pack_t *pack, ff_ctl_loop_ctx_t *ctx)
{
    ff_shell_cfg_t shell_cfg;
    memset(&shell_cfg, 0, sizeof(shell_cfg));

    ff_ctl_loop_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mock_clock = true;

    TEST_ASSERT_EQUAL_INT(0, ff_ctl_loop_open(ctx, shell, pack, &shell_cfg, &cfg));
    bool quit_flag = false;
    (void)ff_ctl_loop_handlers(ctx, &quit_flag);

    ff_ctl_loop_pump(ctx);
    lv_refr_now(ctx->disp);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ctx->state.active_face);
}

/* Navigates launcher -> Settings -> DIAGNOSTICS via the intent seam (an
 * ARRANGE step, not what this file tests — same convention
 * test_gesture_glue.c's own goto_face uses). */
static void goto_diagnostics(ff_ctl_loop_ctx_t *ctx)
{
    ff_intent_t const settings = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {.launcher_idx = 4u}}; /* Settings */
    ff_shell_intent(ctx->shell, &settings);
    ff_ctl_loop_pump(ctx);
    lv_timer_handler();
    lv_refr_now(ctx->disp);
    TEST_ASSERT_EQUAL(FF_APP_FACE_SETTINGS, ctx->state.active_face);

    ff_intent_t const diag = {.kind = FF_INTENT_SETTINGS_OPEN_DIAGNOSTICS, .u = {0}};
    ff_shell_intent(ctx->shell, &diag);
    ff_ctl_loop_pump(ctx);
    lv_timer_handler();
    lv_refr_now(ctx->disp);
    TEST_ASSERT_EQUAL(FF_SETTINGS_SUB_DIAGNOSTICS, ctx->state.settings.subview);
}

/* fix/diag-scroll-persist — the bug, reproduced end to end (fail-first
 * against pre-fix scr_settings.c: the unconditional scroll-to-0 fails
 * this test's final scroll-offset assertion every time). */
static void diag_scroll_survives_device_stats_refresh(void)
{
    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_ctl_loop_ctx_t ctx;

    open_session(&shell, &pack, &ctx);
    goto_diagnostics(&ctx);

    lv_obj_t *list = find_scrollable(lv_screen_active());
    TEST_ASSERT_NOT_NULL_MESSAGE(list, "no scrollable DIAGNOSTICS list found");
    lv_obj_update_layout(list);

    lv_area_t list_area;
    lv_obj_get_coords(list, &list_area);
    int32_t const max_scroll = lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list);
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(
        0, max_scroll, "the DIAGNOSTICS page must genuinely overflow its list to be a real scroll test");
    TEST_ASSERT_EQUAL_INT32(0, lv_obj_get_scroll_y(list)); /* fresh entry starts at the top */

    int32_t const list_x = (list_area.x1 + list_area.x2) / 2;
    /* A real upward drag well inside the list's own viewport (finger
     * moves up -> content scrolls down). Both endpoints stay clear of
     * the S28 HOME rim zone (ff_gesture.h/ff_gesture.c:
     * `home_rim_edge = cy + r - home_rim_px` = 206 + 200 - 64 = 342 in
     * screen-absolute y) — the list's own bottom ~14px
     * (FF_DIAG_LIST_Y=114, FF_DIAG_LIST_H=242 -> list bottom at 356)
     * sits inside that rim, so a drag reaching down there is recognized
     * as a HOME swipe (ff_gesture_glue.c) and leaves Settings entirely
     * before ever reaching this page's own scroll handling — a
     * mechanism this test must route around, not one it is testing. */
    diag_drag(&ctx, list_x, list_area.y1 + 180, list_area.y1 + 20, 6);

    lv_obj_t *list_after_drag = find_scrollable(lv_screen_active());
    TEST_ASSERT_NOT_NULL(list_after_drag);
    lv_obj_update_layout(list_after_drag);
    int32_t const scrolled_y = lv_obj_get_scroll_y(list_after_drag);
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(0, scrolled_y, "the drag did not actually scroll the DIAGNOSTICS list");

    uint32_t const rebuilds_before = ctx.rebuild_count;

    /* The genuine dirty producer: a device-stats push, the SAME call
     * app_main.c makes on its own periodic timer. free_heap_bytes
     * renders at full byte precision (settings_build_diag_page's own
     * "%u B"), so a changed reading is honestly a real, rendered change
     * and legitimately dirties the key — unlike the sub-degree heading
     * noise fixed alongside this in ff_shell.c's shell_render_key
     * (covered instead by app/tests/test_shell.c's own
     * S_diag_heading_keys_rendered_whole_degree_only, which pins that
     * a NON-rendered change must NOT dirty; this test's job is the
     * opposite side — that a rendered change legitimately dirties, and
     * still does not cost the scroll position). */
    ff_shell_set_device_stats(&shell, true, 123456u, FF_APP_MAG_NONE, FF_APP_IMU_ABSENT);
    ctx.mock_clock_ms += 50u;
    ff_ctl_loop_pump(&ctx);
    lv_timer_handler();
    lv_refr_now(ctx.disp);

    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(
        rebuilds_before, ctx.rebuild_count,
        "a device-stats push must genuinely rebuild the DIAGNOSTICS page for this to be a real test of the fix, "
        "not a no-op");

    lv_obj_t *list_after_rebuild = find_scrollable(lv_screen_active());
    TEST_ASSERT_NOT_NULL_MESSAGE(list_after_rebuild, "the DIAGNOSTICS list is gone after the stats-refresh rebuild");
    lv_obj_update_layout(list_after_rebuild);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(
        scrolled_y, lv_obj_get_scroll_y(list_after_rebuild),
        "a routine device-stats refresh reset the DIAGNOSTICS scroll position — the drag was lost");

    /* The touch target is still alive post-restore: drag again and
     * confirm the list keeps scrolling (not stuck, not swallowed) —
     * same "still tappable/draggable after the restore" property
     * S24_thread_scroll_preserved_across_same_thread_rebuild
     * (app/screens/tests/test_scr_intent.c) checks for the thread's own
     * scroll-restore fix. */
    lv_obj_get_coords(list_after_rebuild, &list_area);
    diag_drag(&ctx, list_x, list_area.y1 + 20, list_area.y1 + 180, 6);
    lv_obj_t *list_final = find_scrollable(lv_screen_active());
    TEST_ASSERT_NOT_NULL(list_final);
    lv_obj_update_layout(list_final);
    TEST_ASSERT_LESS_THAN_INT32_MESSAGE(
        scrolled_y, lv_obj_get_scroll_y(list_final),
        "a second drag after the restore did not move the list — the touch target survived but stopped scrolling");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(diag_scroll_survives_device_stats_refresh);
    return UNITY_END();
}
