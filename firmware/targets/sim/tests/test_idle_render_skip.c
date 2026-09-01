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
 * OFF window here is made GENUINELY dirty first (an `FF_INTENT_SWIPE`
 * alternating RADAR<->NOW, guaranteed to change `active_face` — the
 * same guaranteed-dirty technique `ctl_swipe_actually_changes_the_face`
 * proves), and the test opens with a POSITIVE CONTROL proving that same
 * dirty-producing sequence DOES rebuild while ACTIVE — so the OFF
 * window's zero count is a property of the OFF gate, not of nothing
 * happening. The dirty producer goes through `ff_shell_intent` directly
 * (not a real pointer gesture / the ctl `swipe` command): a real
 * gesture is itself a touch and would wake the idle FSM via
 * `ff_idle_input` (ctl_loop.c wires every tap/swipe/hold to it, mirroring
 * app_main.c's touch feed) — exactly the thing under test must NOT do
 * while proving the OFF window holds. The final wake step below uses a
 * REAL ctl `tap`, deliberately, to prove that path.
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

/* Alternates FF_INTENT_SWIPE RADAR<->NOW directly through
 * ff_shell_intent, bypassing the pointer-gesture path (see this file's
 * top comment for why). Starting from RADAR, the first call must swipe
 * toward SIGNALS (+1, ff_intent.h's documented direction convention) or
 * it would be clamped at the RADAR boundary and produce no change. */
static int8_t s_swipe_dir = 1;

static void drive_one_dirty_swipe(ff_shell_t *shell)
{
    ff_intent_t sw = {.kind = FF_INTENT_SWIPE, .u = {.swipe_dir = s_swipe_dir}};
    ff_shell_intent(shell, &sw);
    s_swipe_dir = (int8_t)(-s_swipe_dir);
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
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, ctx.state.active_face);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&ctx.idle));

    /* ---- Positive control: the dirty-producing sequence rebuilds
     * while ACTIVE. Small clock steps, well under FF_IDLE_T_DIM_MS
     * cumulative, so idle stays ACTIVE throughout. ---- */
    for (int i = 0; i < 4; i++) {
        drive_one_dirty_swipe(&shell);
        ctx.mock_clock_ms += 500u;
        ff_ctl_loop_pump(&ctx);
    }
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&ctx.idle));
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(
        0u, ctx.rebuild_count, "positive control failed: alternating FF_INTENT_SWIPE never rebuilt while ACTIVE");

    /* ---- Cross into OFF. ff_idle_input is never called anywhere in
     * this test (the dirty producer above deliberately bypasses the
     * touch path), so idle time has been accruing since t=0 the whole
     * time — jumping the mock clock past FF_IDLE_T_OFF_MS lands
     * directly in OFF (ff_idle's own AC1 boundary tests cover the exact
     * threshold arithmetic; this harness only needs ONE confirmed
     * crossing). ---- */
    ctx.mock_clock_ms = FF_IDLE_T_OFF_MS + 5000u;
    drive_one_dirty_swipe(&shell);
    ff_ctl_loop_pump(&ctx);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_state(&ctx.idle));

    uint32_t const rebuilds_at_off_entry = ctx.rebuild_count;

    /* ---- The OFF window: keep driving REAL dirty ticks (not "nothing
     * happened anyway" — see this file's top comment) across many
     * pumps; the rebuild count must not move. ---- */
    for (int i = 0; i < 6; i++) {
        drive_one_dirty_swipe(&shell);
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S26c_AC3_off_skips_rebuild_and_defers_to_wake);
    return UNITY_END();
}
