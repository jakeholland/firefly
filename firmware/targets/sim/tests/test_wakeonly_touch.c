/**
 * test_wakeonly_touch.c — S26 wake-only-touch amendment
 * (docs/specs/S26-device-lifecycle.md "(c) Inactivity -> dim -> screen
 * off", dated 2026-09-02, maintainer decision): "a touch or button press
 * that begins while the screen is not ACTIVE is a wake-only input and is
 * never delivered to the UI."
 *
 * On-glass bug this closes: a tap on a DIM/OFF screen both woke the
 * device AND landed on whatever was under the finger — an unintended
 * launcher navigation, in this test's case. `ff_idle_touch_gate`
 * (core/include/ff_idle.h) is the pure decision; `ctl_loop.c`'s
 * `ctl_loop_pointer_read_cb` is the sim's enact of it — this file drives
 * a REAL `ff_ctl_loop_*` session (the same object main.c's `--headless
 * --ctl PORT` uses) through a hand-choreographed press/release so it can
 * inspect LVGL's PRESSED style mid-gesture, something the `ctl_tap`
 * black-box helper other tests use cannot do (it completes press+release
 * in one call).
 *
 * THE PROXY, stated up front (AGENTS.md standing rule item 6): the easy
 * proxy for "a wake-only tap does nothing" is checking the face/intent
 * AFTER the fact — which a version that swallows the CLICK but still
 * lets LVGL apply the PRESSED style (a visible flash with no navigation)
 * would still pass. So every test below ALSO inspects
 * `lv_obj_has_state(hub_btn, LV_STATE_PRESSED)` immediately after the
 * press half of the gesture, before release — catching a gate that only
 * suppresses the eventual CLICKED event but still leaks the live PRESSED
 * indev state through to LVGL's hit-testing.
 *
 * Uses the launcher's RADAR hub circle (boot default face, S26e amended
 * 2026-09-01) as the real button: an ordinary `lv_button` with
 * `LV_STATE_PRESSED` styling and a `LV_EVENT_CLICKED` ->
 * `FF_INTENT_LAUNCHER_SELECT` handler (scr_launcher.c), so both
 * observables (press style, intent-fired face change) are available on
 * the very first screen a fresh session boots to — no navigation
 * scaffolding needed.
 *
 * Each test below (DIM, OFF) opens its own `ff_ctl_loop_*` session with
 * its OWN static `shell`/`pack`/`ctx` storage (same convention
 * test_idle_render_skip.c's two independent tests use) rather than
 * sharing one helper's function-static locals across both — reusing the
 * SAME `ff_shell_t`/`ff_ctl_loop_ctx_t` memory across two open/close
 * cycles inside one function is untested territory this suite does not
 * otherwise rely on, so the shared logic below takes shell/pack/ctx BY
 * POINTER instead, and each RUN_TEST supplies its own.
 */
#include <string.h>

#include "unity.h"

#include "ctl_loop.h"
#include "ctl_server.h"
#include "ff_app_state.h"
#include "ff_idle.h"
#include "ff_intent.h"
#include "ff_shell.h"

#include "fp_pack.h"

void setUp(void) {}
void tearDown(void) {}

/* Same recursive lookup as test_ctl_flare_sequence.c's
 * find_button_with_label — duplicated rather than shared, same
 * reasoning that file's own comment gives (no shared-header dependency
 * for one small helper). */
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

/* ---------------------------------------------------------------------
 * A hand-choreographed press/release, mirroring ctl_loop.c's own
 * ctl_loop_pointer_gesture EXACTLY (same 40ms-per-step margin over
 * LVGL's 33ms indev read period, same point-set/step-delay/state-set/
 * lv_timer_handler ordering on both ends) but split into two calls so
 * the test can inspect LVGL state between them — the one thing the
 * ctl socket's black-box `tap` command cannot offer.
 * ------------------------------------------------------------------- */

static void wg_step_delay(ff_ctl_loop_ctx_t *ctx)
{
    ctx->mock_clock_ms += 40u; /* > the 33ms LVGL indev read period */
}

static void wg_press(ff_ctl_loop_ctx_t *ctx, lv_obj_t *btn)
{
    lv_area_t area;
    lv_obj_get_click_area(btn, &area);
    ctx->pointer_point.x = (lv_coord_t)(((int32_t)area.x1 + (int32_t)area.x2) / 2);
    ctx->pointer_point.y = (lv_coord_t)(((int32_t)area.y1 + (int32_t)area.y2) / 2);
    wg_step_delay(ctx);
    ctx->pointer_state = LV_INDEV_STATE_PRESSED;
    lv_timer_handler();
}

/* Releases, then settles exactly like test_ctl_flare_sequence.c's own
 * ctl_tap helper does after its internal gesture completes: one more
 * advance+pump+handler so a fired intent's shell-state change is
 * mirrored into ctx->state before the caller inspects it. */
static void wg_release_and_settle(ff_ctl_loop_ctx_t *ctx)
{
    ctx->pointer_state = LV_INDEV_STATE_RELEASED;
    wg_step_delay(ctx);
    lv_timer_handler();

    wg_step_delay(ctx);
    lv_timer_handler();
    ff_ctl_loop_pump(ctx);
    lv_timer_handler();
}

/* ---------------------------------------------------------------------
 * Shared logic, driven against caller-owned storage: session open
 * through settle-back-to-radar after the second (delivered) tap.
 * `state_advance_ms` is how far past t=0 the mock clock jumps (no touch
 * anywhere in between, so idle time has been accruing since t=0 the
 * whole session) to land in the state under test before the gated tap —
 * DIM's own threshold for the DIM case, OFF's for the OFF case (ff_idle's
 * own boundary tests already pin the exact arithmetic; this harness only
 * needs one confirmed crossing each).
 * ------------------------------------------------------------------- */
static void run_wakeonly_touch_case(ff_shell_t *shell, fp_pack_t *pack, ff_ctl_loop_ctx_t *ctx,
                                     uint32_t state_advance_ms, ff_idle_state_t expected_state_before_tap)
{
    ff_shell_cfg_t shell_cfg;
    memset(&shell_cfg, 0, sizeof(shell_cfg));

    ff_ctl_loop_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mock_clock = true;

    TEST_ASSERT_EQUAL_INT(0, ff_ctl_loop_open(ctx, shell, pack, &shell_cfg, &cfg));
    /* Installs ctl_loop_pointer_read_cb on the pointer indev — without
     * this, lv_timer_handler() below reads nothing and neither the gate
     * nor a real click ever fires (every other ctl-driven test relies on
     * this same call for the identical reason). */
    bool quit_flag = false;
    (void)ff_ctl_loop_handlers(ctx, &quit_flag);

    ff_ctl_loop_pump(ctx); /* settle the always-dirty first tick */
    lv_refr_now(ctx->disp); /* force layout so lv_obj_get_click_area below returns real coords, not (0,0) */
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ctx->state.active_face); /* S26e amended 2026-09-01 boot default */
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&ctx->idle));

    lv_obj_t *hub_btn = find_button_with_label(lv_screen_active(), "RADAR");
    TEST_ASSERT_NOT_NULL_MESSAGE(hub_btn, "launcher RADAR hub button not found — is the launcher actually built?");
    TEST_ASSERT_FALSE(lv_obj_has_state(hub_btn, LV_STATE_PRESSED));

    /* ---- Cross into the state under test, with no touch anywhere. --- */
    ctx->mock_clock_ms = state_advance_ms;
    ff_ctl_loop_pump(ctx);
    TEST_ASSERT_EQUAL(expected_state_before_tap, ff_idle_state(&ctx->idle));

    /* ---- Gated tap: begins while not ACTIVE. -------------------------
     * Must wake, must swallow the ENTIRE gesture — checked at the press
     * half (before release, this file's own top-comment proxy note)
     * AND after settling. */
    wg_press(ctx, hub_btn);
    TEST_ASSERT_EQUAL_MESSAGE(FF_IDLE_STATE_ACTIVE, ff_idle_state(&ctx->idle),
                               "a press-begin while DIM/OFF did not wake idle to ACTIVE");
    TEST_ASSERT_FALSE_MESSAGE(lv_obj_has_state(hub_btn, LV_STATE_PRESSED),
                               "the hub showed PRESSED during a wake-only tap — the gesture leaked to LVGL");
    wg_release_and_settle(ctx);

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&ctx->idle));
    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_LAUNCHER, ctx->state.active_face,
                               "a wake-only tap changed the active face — FF_INTENT_LAUNCHER_SELECT fired");
    TEST_ASSERT_FALSE(lv_obj_has_state(hub_btn, LV_STATE_PRESSED));

    /* ---- Tap again: now ACTIVE, delivered from the first sample. ---- */
    wg_press(ctx, hub_btn);
    TEST_ASSERT_TRUE_MESSAGE(lv_obj_has_state(hub_btn, LV_STATE_PRESSED),
                              "the hub never showed PRESSED on a normal ACTIVE tap — positive control failed");
    wg_release_and_settle(ctx);

    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_RADAR, ctx->state.active_face,
                               "the second tap (begun ACTIVE) did not deliver — FF_INTENT_LAUNCHER_SELECT never fired");

    ff_ctl_loop_close(ctx);
    ff_shell_close(shell);
    lv_deinit();
}

static void S26_wakeonly_dim_tap_wakes_and_swallows_then_delivers(void)
{
    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_ctl_loop_ctx_t ctx;
    run_wakeonly_touch_case(&shell, &pack, &ctx, FF_IDLE_T_DIM_MS, FF_IDLE_STATE_DIM);
}

static void S26_wakeonly_off_tap_wakes_and_swallows_then_delivers(void)
{
    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_ctl_loop_ctx_t ctx;
    run_wakeonly_touch_case(&shell, &pack, &ctx, FF_IDLE_T_OFF_MS, FF_IDLE_STATE_OFF);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S26_wakeonly_dim_tap_wakes_and_swallows_then_delivers);
    RUN_TEST(S26_wakeonly_off_tap_wakes_and_swallows_then_delivers);
    return UNITY_END();
}
