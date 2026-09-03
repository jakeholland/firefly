/**
 * test_idle_render_skip.c — S26 slice (c), AC3
 * (docs/specs/S26-device-lifecycle.md "(c) Inactivity -> dim -> screen
 * off": "OFF skips face rebuilds (assert the render loop's rebuild
 * count is 0 across an OFF window in the sim ctl harness)").
 *
 * Drives a REAL `ff_ctl_loop_pump` session (the same object main.c's
 * `--headless --ctl PORT` uses, and `ctl_loop.c`'s own S26c integration
 * — see that file's `ff_ctl_loop_pump`) through repeated dirty ticks,
 * exactly the technique `test_ctl_flare_sequence.c`'s
 * `S16_d_idle_ticks_never_rebuild_the_screen` already established for
 * the dirty-GATE half of this render loop; this file adds the OFF-GATE
 * half S26c introduces.
 *
 * THE PROXY, stated up front (AGENTS.md standing rule item 6): "0
 * rebuilds during an OFF window" is trivially satisfied by a shell that
 * simply has nothing to rebuild (no traffic, no intents) — that would
 * pass even with the OFF gate deleted entirely. So every tick in the
 * OFF window here is made GENUINELY dirty first (an `FF_INTENT_HOME`
 * alternating RADAR<->LAUNCHER, guaranteed to change `active_face` —
 * S26 slice e retired the swipe carousel this used to drive, so HOME is
 * this file's replacement dirty producer), and the test opens with a
 * POSITIVE CONTROL proving that same dirty-producing sequence DOES
 * rebuild while ACTIVE — so the OFF window's zero count is a property
 * of the OFF gate, not of nothing happening. The dirty producer goes
 * through `ff_shell_intent` directly (not a real pointer gesture / the
 * ctl `swipe` command): a real gesture is itself a touch and would wake
 * the idle FSM via `ff_idle_input` (ctl_loop.c wires every tap/swipe/hold
 * to it, mirroring app_main.c's touch feed) — exactly the thing under
 * test must NOT do while proving the OFF window holds. The final wake
 * step below uses a REAL ctl `tap`, deliberately, to prove that path.
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

/* P0 harness-hang fix (debt/test-harness PR). Same bug/fix as
 * tests/test_ctl_flare_sequence.c and tests/test_wakeonly_touch.c (see
 * the latter's tearDown comment for the full repro/verification
 * writeup): each test owns its own lv_init()/lv_deinit() pairing
 * (lv_init() inside ff_ctl_loop_open; lv_deinit() as the test's last
 * line), so a failed TEST_ASSERT anywhere before that lv_deinit()
 * longjmps past it and leaks LVGL initialized into the next test — LVGL
 * v9's lv_init() silently no-ops when already initialized, so the leak
 * isn't caught, it's built on top of. This tearDown is the safety net
 * Unity always runs after every test regardless of outcome: guarded by
 * lv_is_initialized() so it's a no-op on the normal path (the test body
 * already deinited) and does the actual cleanup on the failure path. */
void tearDown(void)
{
    if (lv_is_initialized()) {
        lv_deinit();
    }
}

/* Alternates LAUNCHER<->RADAR directly through ff_shell_intent,
 * bypassing the pointer-gesture path (see this file's top comment for
 * why). AMENDED 2026-09-01: ff_route_home no longer toggles — the
 * launcher IS home now, so a HOME press is a no-op once already there
 * (ff_route.h's header note) — so this alternates HOME (RADAR ->
 * LAUNCHER) with a LAUNCHER_SELECT of Radar (LAUNCHER -> RADAR, index 0
 * in the amended five-circle order), tracked via `*on_launcher` so each
 * call flips deterministically regardless of how many times it has
 * been called before. Unlike the pre-amendment version, direction state
 * IS needed now (the same shape the retired swipe producer needed,
 * ironically un-retired by this same amendment). */
static void drive_one_dirty_nav(ff_shell_t *shell, bool *on_launcher)
{
    if (*on_launcher) {
        ff_intent_t sel = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {0}};
        sel.u.launcher_idx = 0u; /* Radar — index 0 in the amended circle order */
        ff_shell_intent(shell, &sel);
    } else {
        ff_intent_t home = {.kind = FF_INTENT_HOME, .u = {0}};
        ff_shell_intent(shell, &home);
    }
    *on_launcher = !*on_launcher;
}

static void S26c_AC3_off_skips_rebuild_and_defers_to_wake(void)
{
    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_ctl_loop_ctx_t ctx;

    ff_shell_cfg_t shell_cfg;
    memset(&shell_cfg, 0, sizeof(shell_cfg));

    ff_ctl_loop_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mock_clock = true;

    TEST_ASSERT_EQUAL_INT(0, ff_ctl_loop_open(&ctx, &shell, &pack, &shell_cfg, &cfg));

    /* Settle the always-dirty first tick (ctl_loop.h's own documented
     * rule) before measuring anything. */
    ff_ctl_loop_pump(&ctx);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ctx.state.active_face); /* the boot default, S26e amended 2026-09-01 */
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&ctx.idle));
    bool on_launcher = true; /* matches the boot default just asserted above */

    /* ---- Positive control: the dirty-producing sequence rebuilds
     * while ACTIVE. Small clock steps, well under FF_IDLE_T_DIM_MS
     * cumulative, so idle stays ACTIVE throughout. ---- */
    for (int i = 0; i < 4; i++) {
        drive_one_dirty_nav(&shell, &on_launcher);
        ctx.mock_clock_ms += 500u;
        ff_ctl_loop_pump(&ctx);
    }
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&ctx.idle));
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(
        0u, ctx.rebuild_count, "positive control failed: alternating launcher/Radar nav never rebuilt while ACTIVE");

    /* ---- Cross into OFF. ff_idle_input is never called anywhere in
     * this test (the dirty producer above deliberately bypasses the
     * touch path), so idle time has been accruing since t=0 the whole
     * time — jumping the mock clock past FF_IDLE_T_OFF_MS lands
     * directly in OFF (ff_idle's own AC1 boundary tests cover the exact
     * threshold arithmetic; this harness only needs ONE confirmed
     * crossing). ---- */
    ctx.mock_clock_ms = FF_IDLE_T_OFF_MS + 5000u;
    drive_one_dirty_nav(&shell, &on_launcher);
    ff_ctl_loop_pump(&ctx);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_state(&ctx.idle));

    uint32_t const rebuilds_at_off_entry = ctx.rebuild_count;

    /* ---- The OFF window: keep driving REAL dirty ticks (not "nothing
     * happened anyway" — see this file's top comment) across many
     * pumps; the rebuild count must not move. ---- */
    for (int i = 0; i < 6; i++) {
        drive_one_dirty_nav(&shell, &on_launcher);
        ctx.mock_clock_ms += 1000u;
        ff_ctl_loop_pump(&ctx);
        TEST_ASSERT_EQUAL_MESSAGE(FF_IDLE_STATE_OFF, ff_idle_state(&ctx.idle),
                                   "idle left OFF mid-window with no input — a keep_awake source leaked in");
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rebuilds_at_off_entry, ctx.rebuild_count,
                                      "a rebuild happened during the OFF window — AC3 requires 0");

    /* ---- Wake via a REAL ctl `tap` (the actual touch path — see
     * ctl_loop.c's ctl_loop_pointer_gesture, which calls ff_idle_input
     * exactly as app_main.c's touch indev does) at a blank corner (no
     * button there, so this exercises only the wake path, not some
     * button's side effect). The dirty view accumulated during OFF
     * (rebuild_pending, latched but never drained) must now rebuild —
     * "a dirty view is rebuilt on wake". ---- */
    bool quit_flag = false;
    ff_ctl_handlers_t h = ff_ctl_loop_handlers(&ctx, &quit_flag);
    char resp[FF_CTL_MAX_RESP];
    (void)ff_ctl_process_line("{\"cmd\":\"tap\",\"x\":10,\"y\":10}", &h, resp, sizeof(resp));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(resp, "\"ok\":true"), resp);

    ff_ctl_loop_pump(&ctx);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&ctx.idle));
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(
        rebuilds_at_off_entry, ctx.rebuild_count, "the dirty view accumulated during OFF was not rebuilt on wake");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* S26 slice (f) — "Sim: nothing to enact (no sleep on host) — ctl_loop
 * should treat SLEEP like OFF for rebuild-skip; extend
 * test_idle_render_skip to cover a SLEEP window." Same technique as the
 * OFF test above (real dirty ticks via ff_shell_intent, not "nothing
 * happened anyway" — see this file's top-of-file proxy note), continued
 * past FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS into SLEEP: the rebuild
 * count must still not move, and a real wake (ctl `tap`) must still
 * drain the accumulated dirty view straight to ACTIVE — proving
 * ctl_loop.c's rebuild gate folds SLEEP into the same `screen_blank`
 * check as OFF, not a second independent one that could regress on its
 * own. */
static void S26f_sleep_window_also_skips_rebuild_and_defers_to_wake(void)
{
    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_ctl_loop_ctx_t ctx;

    ff_shell_cfg_t shell_cfg;
    memset(&shell_cfg, 0, sizeof(shell_cfg));

    ff_ctl_loop_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mock_clock = true;

    TEST_ASSERT_EQUAL_INT(0, ff_ctl_loop_open(&ctx, &shell, &pack, &shell_cfg, &cfg));

    ff_ctl_loop_pump(&ctx);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ctx.state.active_face); /* the boot default, S26e amended 2026-09-01 */
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&ctx.idle));
    bool on_launcher = true; /* matches the boot default just asserted above */

    /* ---- Positive control, same as the OFF test: dirty ticks rebuild
     * while ACTIVE. ---- */
    for (int i = 0; i < 4; i++) {
        drive_one_dirty_nav(&shell, &on_launcher);
        ctx.mock_clock_ms += 500u;
        ff_ctl_loop_pump(&ctx);
    }
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&ctx.idle));
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(
        0u, ctx.rebuild_count, "positive control failed: alternating launcher/Radar nav never rebuilt while ACTIVE");

    /* ---- Jump straight into SLEEP (past FF_IDLE_T_OFF_MS +
     * FF_IDLE_T_SLEEP_MS) — no ff_idle_input anywhere in this test, so
     * idle time has been accruing since t=0 the whole time. One
     * confirmed crossing is enough; ff_idle's own tests cover the exact
     * threshold arithmetic. ---- */
    ctx.mock_clock_ms = FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS + 5000u;
    drive_one_dirty_nav(&shell, &on_launcher);
    ff_ctl_loop_pump(&ctx);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_SLEEP, ff_idle_state(&ctx.idle));

    uint32_t const rebuilds_at_sleep_entry = ctx.rebuild_count;

    /* ---- The SLEEP window: keep driving real dirty ticks; the rebuild
     * count must not move, same AC3 property OFF already proved, now
     * extended one state further. ---- */
    for (int i = 0; i < 6; i++) {
        drive_one_dirty_nav(&shell, &on_launcher);
        ctx.mock_clock_ms += 1000u;
        ff_ctl_loop_pump(&ctx);
        TEST_ASSERT_EQUAL_MESSAGE(FF_IDLE_STATE_SLEEP, ff_idle_state(&ctx.idle),
                                   "idle left SLEEP mid-window with no input — a keep_awake source leaked in");
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rebuilds_at_sleep_entry, ctx.rebuild_count,
                                      "a rebuild happened during the SLEEP window — S26f requires 0, same as OFF");

    /* ---- Wake via a REAL ctl `tap`; the dirty view accumulated during
     * SLEEP must rebuild, landing in ACTIVE. ---- */
    bool quit_flag = false;
    ff_ctl_handlers_t h = ff_ctl_loop_handlers(&ctx, &quit_flag);
    char resp[FF_CTL_MAX_RESP];
    (void)ff_ctl_process_line("{\"cmd\":\"tap\",\"x\":10,\"y\":10}", &h, resp, sizeof(resp));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(resp, "\"ok\":true"), resp);

    ff_ctl_loop_pump(&ctx);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&ctx.idle));
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(
        rebuilds_at_sleep_entry, ctx.rebuild_count, "the dirty view accumulated during SLEEP was not rebuilt on wake");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S26c_AC3_off_skips_rebuild_and_defers_to_wake);
    RUN_TEST(S26f_sleep_window_also_skips_rebuild_and_defers_to_wake);
    return UNITY_END();
}
