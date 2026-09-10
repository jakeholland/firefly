/**
 * test_ctl_music_frame_stats.c — 2026-09-09 amendment (fix/s31-beat-
 * real-audio, docs/specs/S31-music-swarm.md's dated amendment): the
 * `music` bench console line's `frame_ms`/`canvas_us` fragment
 * (`ff_dbgconsole_music_frame_fn` -> `app_main.c`'s `dbgconsole_music_
 * frame` -> `ff_scr_music_debug_frame_stats()`, `scr_music.c`) never
 * populated on Jake's puck for an entire 15s real-music session —
 * `frame_ms=n/a canvas_us=n/a` throughout. This file is the sim
 * regression the deliverable's own "add a sim test that the hook
 * returns non-n/a values after a build+timer run" asks for: it builds
 * Music for real (a real `ff_ctl_loop_pump` session, the same harness
 * `test_ctl_music_idle_drain.c` uses) and drives its per-frame timer
 * with real `lv_timer_handler()` calls under a mock clock, then reads
 * `ff_scr_music_debug_frame_stats()` directly (the exact function the
 * console hook calls) rather than going through the console layer —
 * this file has no `ff_debug_console.h` dependency to exercise; that
 * layer's own NULL-hook/negative-return honesty is `test_debug_
 * console.c`'s job, not this file's.
 *
 * Also pins the SAME amendment's "keep the last values for 30s after
 * leaving the face" fix (`FF_SCR_MUSIC_FRAME_STATS_KEEP_MS`,
 * scr_music.c): a console read shortly after leaving Music still shows
 * the real numbers from the session that just ended; a read long after
 * (>30s) honestly reports n/a again, never an arbitrarily stale number.
 */
#include <string.h>

#include "unity.h"

#include "ctl_loop.h"
#include "ff_app_state.h"
#include "ff_intent.h"
#include "ff_shell.h"
#include "scr_music.h"

#include "fp_pack.h"

void setUp(void) {}

/* Same tearDown-safety-net shape every ctl-loop test file in this
 * directory carries — see test_ctl_music_idle_drain.c's own comment. */
void tearDown(void)
{
    if (lv_is_initialized()) {
        lv_deinit();
    }
}

/* Drives `n_ms` of simulated time through the ctl loop in
 * `FF_TEST_STEP_MS` steps, pumping the shell AND (unlike a bare
 * `ff_shell_tick`) LVGL's own timer queue every step — `scr_music.c`'s
 * per-frame ticker is an `lv_timer_t`, not shell-tick-driven, so
 * `lv_timer_handler()` is what actually calls `music_timer_cb` and, in
 * turn, `music_frame_stats_add` (same technique `test_ctl_music_idle_
 * drain.c`'s own main loop already uses). A steady MIC input is fed
 * every step (constant level — this file is not testing the DETECTOR,
 * only that the screen's own timer runs and reports itself) so the
 * swarm has something nonzero to animate/step, mirroring `test_shell.
 * c`'s own `churn_setup_add_music_input` precedent for "give the face
 * something to render, not a claim about what value matters". */
static void run_music_ms(ff_ctl_loop_ctx_t *ctx, uint32_t n_ms)
{
    enum { FF_TEST_STEP_MS = 20u }; /* matches the mic's own nominal 50Hz frame rate */
    uint32_t elapsed = 0u;
    while (elapsed < n_ms) {
        elapsed += FF_TEST_STEP_MS;
        ctx->mock_clock_ms += FF_TEST_STEP_MS;
        ff_ctl_loop_pump(ctx);
        lv_timer_handler();
        ff_shell_set_beat_input(ctx->shell, /* mic_present */ true, -20.0f, -20.0f, /* mic_low_band_dbfs */ -20.0f,
                                 /* mic_mid_band_dbfs */ -20.0f, /* imu_present */ false, /* accel_z_g */ 0.0f,
                                 ctx->mock_clock_ms);
    }
}

static void enter_music(ff_ctl_loop_ctx_t *ctx)
{
    ff_intent_t const sel = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {.launcher_idx = 5u}}; /* Music, per S31's own launcher slot */
    ff_shell_intent(ctx->shell, &sel);
    ff_ctl_loop_pump(ctx);
    lv_timer_handler();
    TEST_ASSERT_EQUAL(FF_APP_FACE_MUSIC, ctx->state.active_face);
}

static void leave_to_launcher(ff_ctl_loop_ctx_t *ctx)
{
    ff_intent_t const back = {.kind = FF_INTENT_BACK, .u = {0}};
    ff_shell_intent(ctx->shell, &back);
    ff_ctl_loop_pump(ctx);
    lv_timer_handler();
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ctx->state.active_face);
}

static void S31_frame_stats_hook_reports_real_numbers_after_a_build_and_timer_run(void)
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
    ff_ctl_loop_pump(&ctx); /* settle the always-dirty first tick */

    enter_music(&ctx);

    /* Immediately after the build (the settle draw only, no real timer
     * tick yet) — honestly n/a, the on-device bug's OWN failure mode,
     * proving this assertion below is not vacuously true. */
    ff_scr_music_frame_stats_t const before = ff_scr_music_debug_frame_stats();
    TEST_ASSERT_FALSE_MESSAGE(before.valid, "frame stats were valid before a single real timer tick ever ran");

    /* 2 real seconds of a real per-frame timer running — comfortably
     * more than one FF_SCR_MUSIC_FRAME_STATS_WINDOW_MS (1000ms) window
     * closing at this face's own ~30fps period. */
    run_music_ms(&ctx, 2000u);

    ff_scr_music_frame_stats_t const after = ff_scr_music_debug_frame_stats();
    TEST_ASSERT_TRUE_MESSAGE(after.valid, "frame stats never became valid after 2s of a real build+timer run");
    TEST_ASSERT_GREATER_THAN_FLOAT_MESSAGE(0.0f, after.frame_period_avg_ms, "frame_period_avg_ms was not a real, positive number");
    /* The 30fps-normal-battery period is 33ms; a generous upper bound
     * (500ms) catches "reporting garbage", not a specific fps target
     * this test has no business pinning. */
    TEST_ASSERT_LESS_THAN_FLOAT_MESSAGE(500.0f, after.frame_period_avg_ms, "frame_period_avg_ms looked like garbage, not a real frame period");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

static void S31_frame_stats_survive_leaving_the_face_for_30s_then_expire(void)
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

    enter_music(&ctx);
    run_music_ms(&ctx, 2000u);

    ff_scr_music_frame_stats_t const while_on_music = ff_scr_music_debug_frame_stats();
    TEST_ASSERT_TRUE(while_on_music.valid);

    leave_to_launcher(&ctx);

    /* A console read moments after leaving still shows the real numbers
     * from the session that just ended — the deliverable's own stated
     * acceptance ("a console read after the session still shows them"). */
    ctx.mock_clock_ms += 5000u;
    ff_ctl_loop_pump(&ctx);
    ff_scr_music_frame_stats_t const soon_after_leaving = ff_scr_music_debug_frame_stats();
    TEST_ASSERT_TRUE_MESSAGE(soon_after_leaving.valid, "frame stats were dropped immediately on leaving the face");
    TEST_ASSERT_EQUAL_FLOAT(while_on_music.frame_period_avg_ms, soon_after_leaving.frame_period_avg_ms);

    /* Long after (comfortably past FF_SCR_MUSIC_FRAME_STATS_KEEP_MS =
     * 30s from the LAST real tick, not from leaving) — honestly n/a
     * again, never an arbitrarily stale number from a session long
     * over. */
    ctx.mock_clock_ms += 40000u;
    ff_ctl_loop_pump(&ctx);
    ff_scr_music_frame_stats_t const long_after_leaving = ff_scr_music_debug_frame_stats();
    TEST_ASSERT_FALSE_MESSAGE(long_after_leaving.valid, "frame stats stayed valid forever — never expired after leaving");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S31_frame_stats_hook_reports_real_numbers_after_a_build_and_timer_run);
    RUN_TEST(S31_frame_stats_survive_leaving_the_face_for_30s_then_expire);
    return UNITY_END();
}
