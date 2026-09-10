/**
 * test_ctl_music_idle_drain.c — fix/s31-music-idle-drain (2026-09-09):
 * reproduces, in the sim, the overnight bench bug this PR fixes.
 *
 * ## Bench evidence (Jake's puck, main 51c5d16, overnight on USB)
 * The puck was left on the Music (Swarm) face. Console log: `ff_mic: mic
 * started` at uptime 44.75s, then NO backlight change for 6.6 HOURS (no
 * DIM at 15s, no OFF at 30s), then `ff_mic: mic stopped` at uptime
 * 23,878s. The room was ordinary night ambient. Root cause:
 * `ff_shell_keep_awake`'s old Music branch ("the face keeps the puck
 * awake only while the level is above the floor") never released —
 * `ff_beat_t`'s loudness is computed against an AUTO-RANGING floor that
 * chases whatever the room's ambient level actually is (`ff_beat.h`'s
 * own "Loudness" section), so ordinary quiet-room noise sits jittering
 * just above that self-tracking floor forever. The screen stayed at 90%
 * and the mic ran all night.
 *
 * ## This test
 * Drives a REAL `ff_ctl_loop_pump` session (the same harness `test_idle_
 * render_skip.c`'s own AC3 test uses) into the Music face, then for 5
 * SIMULATED minutes with NO touch anywhere in the test: feeds a
 * synthetic mic stream — steady ambient noise at -60 dBFS with
 * occasional -40 dBFS "events" (an ordinary room, not a silent one,
 * exactly the case the OLD keep-awake logic was built for and that this
 * bug proved it could never actually release) — through `ff_shell_set_
 * beat_input`, gated the SAME way `app_main.c`'s device loop gates the
 * real mic (`ff_shell_music_wants_mic`, the one shared predicate this PR
 * also introduces so the two can never drift apart).
 *
 * Asserts: DIM at `FF_IDLE_T_DIM_MS` (15s) and OFF at `FF_IDLE_T_OFF_MS`
 * (30s) — the S26 timers, completely unmoved by how loud the room is —
 * idle STAYS OFF for the rest of the 5-minute window (the actual
 * regression: the old code never left ACTIVE at all), the mic is never
 * fed while OFF, the swarm's own per-frame timer stops stepping/
 * redrawing at DIM (`ff_scr_music_debug_render_ticks`, this PR's own
 * test-only instrumentation — "rendering pauses" is a MEASURED property
 * here, not an assumed one, per AGENTS.md item 6), and total mic-on time
 * across the whole 5-minute window stays capped at the pre-DIM ACTIVE
 * window (<=15s) — the overnight bug, in one number, pinned shut.
 */
#include <string.h>

#include "unity.h"

#include "ctl_loop.h"
#include "ff_app_state.h"
#include "ff_idle.h"
#include "ff_intent.h"
#include "ff_shell.h"
#include "scr_music.h"

#include "fp_pack.h"

void setUp(void) {}

/* P0 harness-hang fix (debt/test-harness PR) — same tearDown-safety-net
 * shape every other ctl-loop test file in this directory carries (see
 * test_idle_render_skip.c's own tearDown comment for the full repro/
 * verification writeup): each test owns its own lv_init()/lv_deinit()
 * pairing, so a failed TEST_ASSERT before that lv_deinit() longjmps past
 * it and would leak LVGL initialized into the next test without this. */
void tearDown(void)
{
    if (lv_is_initialized()) {
        lv_deinit();
    }
}

/* Feeds ONE synthetic mic sample — steady -60 dBFS ambient, an "event"
 * at -40 dBFS every FF_TEST_EVENT_EVERY_N-th sample (this file's top
 * comment: "an ordinary room, not a silent one") — but ONLY while
 * `ff_shell_music_wants_mic` says the mic should be running right now,
 * the exact same gate app_main.c's device render loop applies before
 * ever calling the real `ff_mic_start()`/feeding a real frame. Returns
 * true iff a sample was actually fed, so the caller can accumulate
 * mic-on time for real feeds only. */
static bool feed_synthetic_mic_sample(ff_ctl_loop_ctx_t *ctx, uint32_t now_ms, uint32_t *sample_idx)
{
    ff_idle_state_t const idle_state = ff_idle_state(&ctx->idle);
    if (!ff_shell_music_wants_mic(&ctx->state, idle_state)) {
        return false;
    }
    enum { FF_TEST_EVENT_EVERY_N = 7u };
    bool const is_event = ((*sample_idx % FF_TEST_EVENT_EVERY_N) == 0u);
    float const dbfs = is_event ? -40.0f : -60.0f;
    (*sample_idx)++;
    ff_shell_set_beat_input(ctx->shell, /* mic_present */ true, dbfs, dbfs, /* mic_low_band_dbfs */ dbfs,
                             /* mic_mid_band_dbfs */ dbfs, /* imu_present */ false,
                             /* accel_z_g */ 0.0f, now_ms);
    return true;
}

static void S31_overnight_bench_repro_dims_offs_and_stops_the_mic(void)
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

    /* Settle the always-dirty first tick before measuring anything (same
     * rule test_idle_render_skip.c's own AC3 test documents). */
    ff_ctl_loop_pump(&ctx);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ctx.state.active_face);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&ctx.idle));

    /* ---- Enter Music via a direct intent, never a real touch — same
     * "the dirty producer bypasses the touch path" technique test_idle_
     * render_skip.c's own top comment establishes for its OFF-window
     * test: a real tap would itself feed ff_idle_input, confounding "no
     * input anywhere in this test" from the very first step. Music is
     * launcher_idx 5 (ff_shell.c's FF_INTENT_LAUNCHER_SELECT handling,
     * "0=Radar, 1=Now, 2=Signals, 3=Map, 4=Settings, 5=Music"). ---- */
    ff_intent_t const sel = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {.launcher_idx = 5u}};
    ff_shell_intent(ctx.shell, &sel);
    ff_ctl_loop_pump(&ctx);
    lv_timer_handler(); /* drives scr_music.c's own per-frame timer for real, same as test_gesture_glue.c's goto_face */
    TEST_ASSERT_EQUAL(FF_APP_FACE_MUSIC, ctx.state.active_face);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&ctx.idle));

    uint32_t const render_ticks_at_music_entry = ff_scr_music_debug_render_ticks();

    /* ---- 5 simulated minutes, no touch anywhere below, a synthetic
     * ambient mic stream the whole time. ---- */
    enum { FF_TEST_STEP_MS = 250u };
    enum { FF_TEST_TOTAL_MS = 300000u }; /* 5 minutes */

    uint32_t sample_idx = 0u;
    uint32_t mic_on_ms = 0u;
    uint32_t render_ticks_at_dim_entry = 0u;
    bool saw_dim = false;
    bool saw_off = false;
    bool dim_at_15s = false;
    bool off_at_30s = false;

    for (uint32_t elapsed = FF_TEST_STEP_MS; elapsed <= FF_TEST_TOTAL_MS; elapsed += FF_TEST_STEP_MS) {
        ctx.mock_clock_ms = elapsed;
        ff_ctl_loop_pump(&ctx);
        lv_timer_handler();

        ff_idle_state_t const st = ff_idle_state(&ctx.idle);

        if (feed_synthetic_mic_sample(&ctx, elapsed, &sample_idx)) {
            mic_on_ms += FF_TEST_STEP_MS;
            TEST_ASSERT_FALSE_MESSAGE(st == FF_IDLE_STATE_OFF, "mic fed while OFF — S31 power policy violated");
            TEST_ASSERT_FALSE_MESSAGE(st == FF_IDLE_STATE_SLEEP, "mic fed while SLEEP — S31 power policy violated");
        }

        if (!saw_dim && st == FF_IDLE_STATE_DIM) {
            saw_dim = true;
            dim_at_15s = (elapsed <= FF_IDLE_T_DIM_MS + FF_TEST_STEP_MS);
            /* Captured AFTER this same iteration's lv_timer_handler() —
             * scr_music.c's timer reads state->music.screen_awake one
             * frame behind the idle transition itself (ff_shell_set_
             * screen_awake's own doc comment: same small lag ff_shell_
             * set_mic_status already tolerates elsewhere), so this frame
             * may still show one last tick from the tail end of ACTIVE.
             * Every frame from here on must NOT move this counter. */
            render_ticks_at_dim_entry = ff_scr_music_debug_render_ticks();
        }
        if (!saw_off && st == FF_IDLE_STATE_OFF) {
            saw_off = true;
            off_at_30s = (elapsed <= FF_IDLE_T_OFF_MS + FF_TEST_STEP_MS);
        }
        if (saw_off) {
            /* OFF may advance further into SLEEP (S26 slice f, at
             * FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS = 150s, well inside
             * this test's 5-minute window) — that is ordinary forward
             * progress, not a regression. What must NEVER happen with no
             * input anywhere in this test is a reversal back to DIM or
             * ACTIVE — that IS the overnight regression this PR fixes. */
            TEST_ASSERT_TRUE_MESSAGE(st == FF_IDLE_STATE_OFF || st == FF_IDLE_STATE_SLEEP,
                                      "idle left OFF/SLEEP mid-window with no input — a keep_awake source leaked in "
                                      "(the exact overnight regression this PR fixes)");
        }
        if (saw_dim) {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(render_ticks_at_dim_entry, ff_scr_music_debug_render_ticks(),
                                              "the swarm kept stepping/redrawing after DIM — rendering did not pause");
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(saw_dim, "never reached DIM across 5 simulated minutes with no input");
    TEST_ASSERT_TRUE_MESSAGE(dim_at_15s, "DIM did not arrive at FF_IDLE_T_DIM_MS (15s)");
    TEST_ASSERT_TRUE_MESSAGE(saw_off, "never reached OFF across 5 simulated minutes with no input");
    TEST_ASSERT_TRUE_MESSAGE(off_at_30s, "OFF did not arrive at FF_IDLE_T_OFF_MS (30s)");
    /* By the end of the 5-minute window idle has legitimately progressed
     * to SLEEP (FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS = 150s, well inside
     * this window — S26 slice f, ordinary forward-only progress with no
     * input) — the per-iteration loop above already pinned that it never
     * reversed back to DIM/ACTIVE at any point once OFF was reached. */
    ff_idle_state_t const final_state = ff_idle_state(&ctx.idle);
    TEST_ASSERT_TRUE_MESSAGE(final_state == FF_IDLE_STATE_OFF || final_state == FF_IDLE_STATE_SLEEP,
                              "idle did not stay OFF/SLEEP through the full 5-minute window — the overnight bug is back");

    /* ---- The overnight bug, pinned in one number: total mic-on time
     * across the whole 5-minute window must stay capped at the ACTIVE-
     * before-DIM window, not run anywhere close to the full 5 minutes
     * the old loudness-based keep_awake allowed (the real bug ran 6.6
     * HOURS). ---- */
    TEST_ASSERT_LESS_OR_EQUAL_UINT32_MESSAGE(
        FF_IDLE_T_DIM_MS, mic_on_ms, "mic ran longer than the ACTIVE window — the overnight battery-drain bug is back");
    TEST_ASSERT_FALSE_MESSAGE(ff_shell_music_wants_mic(&ctx.state, ff_idle_state(&ctx.idle)),
                               "the mic still wants to run while OFF");

    /* ---- Positive control: the swarm DID actually render while
     * genuinely ACTIVE (so the frozen count above is a property of the
     * DIM gate, not of nothing having rendered at all — same "prove the
     * gate, not the absence of traffic" discipline test_idle_render_
     * skip.c's own top comment applies to its rebuild-count assertion). */
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(
        render_ticks_at_music_entry, render_ticks_at_dim_entry,
        "positive control failed: the swarm never rendered at all while ACTIVE");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S31_overnight_bench_repro_dims_offs_and_stops_the_mic);
    return UNITY_END();
}
