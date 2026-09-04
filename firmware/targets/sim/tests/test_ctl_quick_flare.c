/**
 * test_ctl_quick_flare.c — S10 quick flare (docs/specs/S10-flare.md's
 * Amendments, 2026-09-03): "press the HOME (BOOT, GPIO0) button 5 times
 * quickly to flare to the crew, no screen needed."
 *
 * Drives a REAL `ff_ctl_loop_*` session (the same object main.c's
 * `--headless --ctl PORT` uses) through FIVE synthetic BOOT/HOME
 * presses via `ff_ctl_loop_boot_press` (ctl_loop.h) — the REAL button
 * path: `ff_button_tick`'s debounce, `ff_idle_touch_gate`'s wake-only
 * verdict, `ff_idle_input`'s re-pin, and `ff_shell_home_press`'s
 * multitap/HOME dispatch — starting from a genuinely idle-FSM-computed
 * OFF screen (not a bare `ff_shell_home_press` unit call), so this is
 * the one test in the suite that proves the whole S26+S10 stack
 * composes end to end: BOOT wakes a dark screen, keeps counting, and
 * the 5th press starts a real flare send with the sender overlay
 * actually rendered in the live LVGL tree.
 *
 * THE PROXY, stated up front (AGENTS.md item 6): "ctx.state.flare.sending
 * reads true" is satisfied even by a shell that flips the flag with no
 * screen behind it at all (exactly the class of bug `test_wakeonly_
 * touch.c`'s own top comment warns about — a state change with no
 * pixel to back it). This test additionally finds the actual "CANCEL"
 * button LVGL built, so a regression that sets the flag but leaves
 * `face_dispatch.c`/`scr_nav.c` never rendering the overlay is still
 * caught.
 */
#include <string.h>

#include "unity.h"

#include "ctl_loop.h"
#include "ctl_server.h"
#include "ff_app_state.h"
#include "ff_flare.h"
#include "ff_idle.h"
#include "ff_intent.h"
#include "ff_proto.h"
#include "ff_shell.h"

#include "fp_pack.h"

void setUp(void) {}

/* P0 harness-hang fix (debt/test-harness PR) — same convention as every
 * other ctl-driven test in this suite; see test_wakeonly_touch.c's
 * tearDown comment for the full repro/verification writeup. */
void tearDown(void)
{
    if (lv_is_initialized()) {
        lv_deinit();
    }
}

/* Same recursive lookup as test_ctl_flare_sequence.c's/test_wakeonly_
 * touch.c's find_button_with_label — duplicated rather than shared,
 * same reasoning those files' own comments give. */
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

/* The gap between successive presses, from `ff_ctl_loop_boot_press`'s
 * own documented internal choreography (two debounce-settle pairs
 * totalling 67ms — see that function's implementation comment in
 * ctl_loop.c) plus this much EXTRA mock-clock advance between calls, so
 * consecutive DEBOUNCED presses land ~300ms apart: comfortably under
 * `FF_MULTITAP_MAX_GAP_MS` (400ms) and, across 4 gaps, comfortably under
 * `FF_MULTITAP_WINDOW_MS` (2500ms) too. */
#define QF_EXTRA_GAP_MS 233u

static void S10_quick_flare_five_boot_presses_from_off_starts_sending_and_renders_overlay(void)
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
    /* Installs the pointer indev read callback — not used by this test
     * directly (BOOT goes through no indev), but every other ctl-driven
     * test calls this before its first pump, and skipping it is
     * untested territory this file has no reason to be the first to
     * explore. */
    bool quit_flag = false;
    (void)ff_ctl_loop_handlers(&ctx, &quit_flag);

    ff_ctl_loop_pump(&ctx); /* settle the always-dirty first tick */
    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_LAUNCHER, ctx.state.active_face,
                               "S26e boot default — is the launcher still home?");
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&ctx.idle));
    TEST_ASSERT_FALSE(ctx.state.flare.sending);

    /* Cross into OFF with no input anywhere — same technique test_
     * wakeonly_touch.c's run_wakeonly_touch_case uses: jump the mock
     * clock straight to the OFF threshold and pump once so ff_idle_tick
     * actually computes the transition. */
    ctx.mock_clock_ms = FF_IDLE_T_OFF_MS;
    ff_ctl_loop_pump(&ctx);
    TEST_ASSERT_EQUAL_MESSAGE(FF_IDLE_STATE_OFF, ff_idle_state(&ctx.idle),
                               "did not reach OFF — is FF_IDLE_T_OFF_MS still what this test assumes?");

    /* Five BOOT presses through the REAL button path, ~300ms apart. */
    for (int tap = 0; tap < 5; tap++) {
        ff_ctl_loop_boot_press(&ctx);
        if (tap < 4) {
            ctx.mock_clock_ms += QF_EXTRA_GAP_MS;
        }
        ff_ctl_loop_pump(&ctx);
    }

    TEST_ASSERT_TRUE_MESSAGE(ctx.state.flare.sending,
                              "quick flare did not start sending after 5 BOOT presses from an OFF screen");
    /* The first press must have woken the screen (S10's own "the first
     * tap wakes and counts" requirement) and the sequence must not have
     * let it fall back asleep in between (the quick_flare_pending
     * keep-awake input). */
    TEST_ASSERT_EQUAL_MESSAGE(FF_IDLE_STATE_ACTIVE, ff_idle_state(&ctx.idle),
                               "screen did not wake/stay awake across the 5-tap sequence");

    /* Not a state-only proxy: the sender overlay must actually be in the
     * live LVGL tree. */
    lv_refr_now(ctx.disp);
    lv_obj_t *cancel_btn = find_button_with_label(lv_screen_active(), "CANCEL");
    TEST_ASSERT_NOT_NULL_MESSAGE(cancel_btn,
                                  "FLARING sender overlay's CANCEL button not found in the live LVGL tree");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* Negative control: 4 presses (same real button path, same cadence)
 * must NOT start sending — proves the fire above is genuinely gated on
 * the 5th press, not on "any BOOT activity at all". */
static void S10_quick_flare_four_boot_presses_do_not_start_sending(void)
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
    bool quit_flag = false;
    (void)ff_ctl_loop_handlers(&ctx, &quit_flag);
    ff_ctl_loop_pump(&ctx);

    for (int tap = 0; tap < 4; tap++) {
        ff_ctl_loop_boot_press(&ctx);
        if (tap < 3) {
            ctx.mock_clock_ms += QF_EXTRA_GAP_MS;
        }
        ff_ctl_loop_pump(&ctx);
    }

    TEST_ASSERT_FALSE(ctx.state.flare.sending);

    /* Positive control, same session: the 5th press (same cadence) DOES
     * fire — proves the negative result above is the count, not a
     * broken harness. */
    ff_ctl_loop_boot_press(&ctx);
    ff_ctl_loop_pump(&ctx);
    TEST_ASSERT_TRUE(ctx.state.flare.sending);

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}


/* Review round 2 — the launcher's own CANCEL button (the sender
 * overlay's undo, `ff_scr_flare_build_sender_overlay`'s `flare_cancel_
 * send_cb`) must actually work on the launcher, not just render there:
 * a real tap on it emits FF_INTENT_FLARE_END and the overlay
 * disappears from the live LVGL tree, driven through a genuine
 * pointer gesture (ctl_loop.c's synthetic indev), not a bare
 * `ff_shell_intent(FF_INTENT_FLARE_END)` call. */
static void S10_quick_flare_launcher_cancel_button_ends_the_send(void)
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
    bool quit_flag = false;
    (void)ff_ctl_loop_handlers(&ctx, &quit_flag);
    ff_ctl_loop_pump(&ctx); /* settle the always-dirty first tick */
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ctx.state.active_face);

    /* Five BOOT presses (screen already ACTIVE throughout — the
     * wake-only-touch interaction is the sibling OFF-screen test's job,
     * not this one's). */
    for (int tap = 0; tap < 5; tap++) {
        ff_ctl_loop_boot_press(&ctx);
        if (tap < 4) {
            ctx.mock_clock_ms += QF_EXTRA_GAP_MS;
        }
        ff_ctl_loop_pump(&ctx);
    }
    TEST_ASSERT_TRUE(ctx.state.flare.sending);

    lv_refr_now(ctx.disp);
    lv_obj_t *cancel_btn = find_button_with_label(lv_screen_active(), "CANCEL");
    TEST_ASSERT_NOT_NULL_MESSAGE(cancel_btn, "CANCEL button not found before the tap — setup failed");

    lv_area_t area;
    lv_obj_get_click_area(cancel_btn, &area);
    int32_t const cx = (int32_t)(((int32_t)area.x1 + (int32_t)area.x2) / 2);
    int32_t const cy = (int32_t)(((int32_t)area.y1 + (int32_t)area.y2) / 2);

    ff_ctl_loop_pointer_press(&ctx, cx, cy);
    ff_ctl_loop_pointer_release(&ctx);
    ff_ctl_loop_pointer_step(&ctx); /* one more settle step so the fired intent's rebuild lands */
    ff_ctl_loop_pump(&ctx);
    lv_timer_handler();

    TEST_ASSERT_FALSE_MESSAGE(ctx.state.flare.sending, "CANCEL tap did not end the send (FF_INTENT_FLARE_END never fired)");

    lv_refr_now(ctx.disp);
    lv_obj_t *cancel_after = find_button_with_label(lv_screen_active(), "CANCEL");
    TEST_ASSERT_NULL_MESSAGE(cancel_after, "the sender overlay's CANCEL button is still in the tree after CANCEL was tapped");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* ---------------------------------------------------------------------
 * S10 Amendment (2026-09-03, "Wire honesty", fix/flare-wire-send) — the
 * P0 fix at the ctl-observable level: quick flare's real button path
 * (ff_button debounce -> ff_shell_home_press -> the multitap FSM) must
 * put an actual FLARE frame on the wire, not just flip `sending`. A
 * small spy installed via `ff_shell_set_sender` stands in for the
 * outbound transport (the same seam test_shell.c's flare_wire_spy_t
 * uses) — this is the "ctl-observable outbound log" the task brief asks
 * for: a real ff_ctl_loop_ctx_t session, driven through
 * ff_ctl_loop_boot_press exactly like the sibling test above, with the
 * frame captured at the one seam ctl_loop.h exposes for it
 * (ff_shell_set_sender on the caller-owned `shell`).
 * ------------------------------------------------------------------- */

typedef struct {
    uint8_t buf[FF_PROTO_MAX_PAYLOAD];
    size_t  len;
    int     n_sends;
} qf_wire_spy_t;

static qf_wire_spy_t QS;

static int qf_wire_spy_send_text(void *ctx, uint32_t dest, char const *utf8)
{
    (void)ctx;
    (void)dest;
    (void)utf8;
    return 0;
}

static int qf_wire_spy_send_private(void *ctx, uint32_t dest, uint8_t const *payload, size_t len, uint32_t flags)
{
    (void)dest;
    (void)flags;
    qf_wire_spy_t *s = (qf_wire_spy_t *)ctx;
    s->n_sends++;
    s->len = (len > sizeof(s->buf)) ? sizeof(s->buf) : len;
    memcpy(s->buf, payload, s->len);
    return 0; /* accepted */
}

static void S10_wire_quick_flare_five_boot_presses_puts_one_flare_frame_on_the_wire(void)
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

    memset(&QS, 0, sizeof(QS));
    ff_wiring_sender_t const spy = {qf_wire_spy_send_text, qf_wire_spy_send_private, &QS};
    ff_shell_set_sender(&shell, spy);

    bool quit_flag = false;
    (void)ff_ctl_loop_handlers(&ctx, &quit_flag);
    ff_ctl_loop_pump(&ctx); /* settle the always-dirty first tick */
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ctx.state.active_face);
    TEST_ASSERT_EQUAL_INT(0, QS.n_sends);

    for (int tap = 0; tap < 5; tap++) {
        ff_ctl_loop_boot_press(&ctx);
        if (tap < 4) {
            ctx.mock_clock_ms += QF_EXTRA_GAP_MS;
        }
        ff_ctl_loop_pump(&ctx);
    }

    TEST_ASSERT_TRUE_MESSAGE(ctx.state.flare.sending, "quick flare did not start sending");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, QS.n_sends,
                                   "the 5th BOOT press did not put a FLARE frame on the wire — "
                                   "the P0 bug this PR fixes (FF_FLARE_INTENT_SEND_FLARE discarded)");

    ff_proto_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_FLARE, ff_proto_decode(QS.buf, QS.len, &msg));
    TEST_ASSERT_EQUAL_UINT16(300u, msg.body.flare.dur_s);

    TEST_ASSERT_EQUAL(FF_FLARE_WIRE_SENT, ctx.state.flare.wire_state);

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* ---------------------------------------------------------------------
 * fix/flare-cancel-taps — the diagnosis, pinned as tests.
 *
 * MAINTAINER REPORT (on glass, after a 5x HOME quick flare): "the CANCEL
 * button doesn't work well — taps seem to pass through and I tapped it
 * multiple times before it actually worked."
 *
 * Root cause (a), read+confirmed in ff_shell.c: `shell_render_key`
 * coarsened `flare.send_expires_in_ms` to whole seconds and left it IN
 * the render key, so the key dirtied once a second for the WHOLE life
 * of a send. A dirty key gets a full `lv_obj_clean()` + rebuild
 * (targets/esp32s3/main/app_main.c / targets/sim/sim_lifecycle.c), which
 * destroys and recreates the sender overlay's own CANCEL button. A
 * CANCEL press/release straddling one of those once-a-second rebuilds
 * has its pressed object deleted out from under it — LVGL never fires
 * the CLICKED. `S10_flare_countdown_alone_does_not_dirty_key_while_
 * sending` (firmware/app/tests/test_shell.c) pins the SHELL-level half
 * of this; the two tests below pin the same property end-to-end,
 * through a REAL LVGL tree and a REAL synthetic pointer gesture.
 * ------------------------------------------------------------------- */

/* Advances `ctx->mock_clock_ms` to `target_ms` and pumps once — the
 * "with a pump between" step the PR brief's repro recipe calls for,
 * landing a real `ff_shell_tick`/rebuild-gate pass at that exact clock
 * reading (mirrors app_main.c's per-frame order: time moves, THEN the
 * frame's tick runs). */
static void advance_and_pump(ff_ctl_loop_ctx_t *ctx, uint32_t target_ms)
{
    ctx->mock_clock_ms = target_ms;
    ff_ctl_loop_pump(ctx);
}

/* One straddle attempt: press CANCEL 20ms BEFORE a countdown coarsening
 * bucket boundary, let time cross it WHILE HELD (a pump lands mid-hold,
 * exactly like a live tick would on the real device/sim), then release
 * ~20ms after. Returns true iff `ctx->state.flare.sending` flipped false
 * as a direct result — i.e. the tap actually registered.
 *
 * The two internal `ff_ctl_loop_pointer_press`/`_release` calls each add
 * their OWN fixed 40ms indev-poll delay (`ctl_loop_pointer_step_delay`,
 * ctl_loop.c) before the state transition they perform actually gets
 * processed — accounted for below (subtracting 40 from the target
 * before each call) so the PROCESSED press/release land as close to
 * `boundary_ms -/+ 20` as this harness's fixed step size allows, not
 * merely the raw field write. */
static bool try_cancel_straddle(ff_ctl_loop_ctx_t *ctx, int32_t cx, int32_t cy, uint32_t boundary_ms)
{
    ctx->mock_clock_ms = boundary_ms - 20u - 40u;
    ff_ctl_loop_pointer_press(ctx, cx, cy); /* processed at boundary_ms - 20 (old bucket) */

    /* "with a pump between": time crosses the boundary while CANCEL is
     * still held down — a genuine dirty tick (the OLD code's
     * per-second coarsening bucket flip) lands in the middle of the
     * gesture, exactly the straddle the maintainer's report describes.
     * `ctx->mock_clock_ms` is left at `boundary_ms + 20` afterward. */
    advance_and_pump(ctx, boundary_ms + 20u);

    /* `ff_ctl_loop_pointer_release` adds its own +40ms step delay before
     * the RELEASED state is actually processed, so this lands at
     * `boundary_ms + 60` — after the boundary, still well inside the
     * same held gesture. */
    ff_ctl_loop_pointer_release(ctx);
    ff_ctl_loop_pointer_step(ctx); /* one more settle step so the fired intent's rebuild lands */
    ff_ctl_loop_pump(ctx);
    lv_timer_handler();

    return !ctx->state.flare.sending;
}

/* THE straddle test. Tries the gesture at THREE boundaries spread across
 * the 30s window the brief asks for (early/mid/late) — a FRESH quick
 * flare each time (a session only ever has one CANCEL to give), and
 * requires EVERY one of them to end the send on that ONE gesture, no
 * retry needed. Before this PR's fix this test fails intermittently
 * exactly at the coarsening boundary (see the PR body for the pre-fix
 * transcript: 0-3 successes out of these 3 offsets, never a reliable
 * 3, which is the harness-level shape of "I tapped it multiple times
 * before it actually worked"). */
static void S10_quick_flare_cancel_straddling_a_countdown_bucket_boundary_ends_the_send(void)
{
    static uint32_t const kOffsetsMs[] = {1000u, 15000u, 29000u}; /* early / mid / late in the 30s window */

    for (size_t i = 0; i < sizeof(kOffsetsMs) / sizeof(kOffsetsMs[0]); i++) {
        static ff_shell_t shell;
        static fp_pack_t pack;
        static ff_ctl_loop_ctx_t ctx;

        ff_shell_cfg_t shell_cfg;
        memset(&shell_cfg, 0, sizeof(shell_cfg));
        ff_ctl_loop_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.mock_clock = true;

        TEST_ASSERT_EQUAL_INT(0, ff_ctl_loop_open(&ctx, &shell, &pack, &shell_cfg, &cfg));
        bool quit_flag = false;
        (void)ff_ctl_loop_handlers(&ctx, &quit_flag);
        ff_ctl_loop_pump(&ctx); /* settle the always-dirty first tick */
        TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ctx.state.active_face);

        for (int tap = 0; tap < 5; tap++) {
            ff_ctl_loop_boot_press(&ctx);
            if (tap < 4) {
                ctx.mock_clock_ms += QF_EXTRA_GAP_MS;
            }
            ff_ctl_loop_pump(&ctx);
        }
        TEST_ASSERT_TRUE(ctx.state.flare.sending);

        /* Run the clock forward to `kOffsetsMs[i]` into the send (settled,
         * no gesture) so the boundary this attempt straddles is the
         * `now + boundary_ms` one, then find CANCEL fresh at that moment
         * — its exact screen position never moves, but a fresh lookup
         * keeps this test honest about "the real built object", not an
         * assumption it survived from before. */
        advance_and_pump(&ctx, ctx.mock_clock_ms + kOffsetsMs[i]);
        lv_refr_now(ctx.disp);
        lv_obj_t *cancel_btn = find_button_with_label(lv_screen_active(), "CANCEL");
        TEST_ASSERT_NOT_NULL_MESSAGE(cancel_btn, "CANCEL button not found before the straddle attempt");
        lv_area_t area;
        lv_obj_get_click_area(cancel_btn, &area);
        int32_t const cx = (int32_t)(((int32_t)area.x1 + (int32_t)area.x2) / 2);
        int32_t const cy = (int32_t)(((int32_t)area.y1 + (int32_t)area.y2) / 2);

        /* The next whole-second coarsening boundary from HERE — same
         * arithmetic shell_coarsen_ms used to apply (ms / 1000, so the
         * boundary is wherever send_expires_in_ms next crosses a
         * multiple of 1000). */
        int32_t const remaining = ctx.state.flare.send_expires_in_ms;
        uint32_t const rem_mod = (uint32_t)remaining % 1000u;
        uint32_t to_boundary = (rem_mod == 0u) ? 1000u : rem_mod;
        /* try_cancel_straddle positions the PROCESSED press 60ms before
         * `boundary_ms` (20ms of margin plus the harness's own 40ms
         * indev-poll step delay) — if the boundary found above is closer
         * than that, targeting it would walk `ctx->mock_clock_ms`
         * BACKWARD, which is not a real gesture the mock clock (or LVGL's
         * own indev timing) is meant to represent. Roll forward one more
         * whole bucket in that case so there is always real headroom. */
        if (to_boundary < 100u) {
            to_boundary += 1000u;
        }
        uint32_t const boundary_ms = ctx.mock_clock_ms + to_boundary;

        char msg[256]; /* CLAUDE.md: GCC's -Wformat-truncation sizes %u against unsigned int's full
                         * worst-case width (10 digits), not this call's actual short values — size against
                         * that worst case rather than tuning the literal length this message happens to be. */
        snprintf(msg, sizeof(msg),
                 "straddle attempt at offset %us did not end the send on the FIRST tap — "
                 "exactly the maintainer's 'tapped it multiple times' report",
                 (unsigned)(kOffsetsMs[i] / 1000u));
        TEST_ASSERT_TRUE_MESSAGE(try_cancel_straddle(&ctx, cx, cy, boundary_ms), msg);

        ff_ctl_loop_close(&ctx);
        ff_shell_close(&shell);
        lv_deinit();
    }
}

/* Zero rebuilds across the whole 30s window while sending, with NO
 * touch anywhere — isolates "the countdown alone must not rebuild"
 * from the CANCEL-specific tests above (which only prove a tap survives
 * a straddle, not that nothing was rebuilding at all in between). Before
 * this PR's fix, `ctx.rebuild_count` grew by ~30 over this window (one
 * rebuild per coarsening bucket); after it, zero. */
static void S10_quick_flare_zero_rebuilds_across_30s_while_sending(void)
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
    bool quit_flag = false;
    (void)ff_ctl_loop_handlers(&ctx, &quit_flag);
    ff_ctl_loop_pump(&ctx);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ctx.state.active_face);

    for (int tap = 0; tap < 5; tap++) {
        ff_ctl_loop_boot_press(&ctx);
        if (tap < 4) {
            ctx.mock_clock_ms += QF_EXTRA_GAP_MS;
        }
        ff_ctl_loop_pump(&ctx);
    }
    TEST_ASSERT_TRUE(ctx.state.flare.sending);

    uint32_t const rebuilds_before = ctx.rebuild_count;
    uint32_t const start_ms = ctx.mock_clock_ms;
    for (uint32_t s = 1; s <= 30u; s++) {
        advance_and_pump(&ctx, start_ms + s * 1000u);
    }

    TEST_ASSERT_TRUE_MESSAGE(ctx.state.flare.sending, "the send ended on its own within 30s — test setup invalid");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rebuilds_before, ctx.rebuild_count,
                                      "the countdown alone caused a rebuild somewhere in the 30s window while "
                                      "sending — send_expires_in_ms must stay excluded from the render key");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* Root cause (b): a tap on the DIMMED area — deliberately the SETTINGS
 * satellite's own center, which the compass-ring geometry places just
 * ~30px above CANCEL (SETTINGS sits at compass_pos 2 of 4, i.e. straight
 * down from the hub at the orbit radius, 128px — CANCEL sits at 158px on
 * the same line) — must be fully ABSORBED while sending: no satellite
 * navigation, no change to the flare at all. Finds the satellite BEFORE
 * sending starts (its position never moves) so the lookup itself does
 * not depend on the dim/catcher behavior under test. */
static void S10_quick_flare_dim_area_tap_emits_nothing(void)
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
    bool quit_flag = false;
    (void)ff_ctl_loop_handlers(&ctx, &quit_flag);
    ff_ctl_loop_pump(&ctx);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ctx.state.active_face);
    lv_refr_now(ctx.disp);

    lv_obj_t *settings_sat = find_button_with_label(lv_screen_active(), "SETTINGS");
    TEST_ASSERT_NOT_NULL_MESSAGE(settings_sat, "the launcher's SETTINGS satellite was not found before sending");
    lv_area_t area;
    lv_obj_get_click_area(settings_sat, &area);
    int32_t const cx = (int32_t)(((int32_t)area.x1 + (int32_t)area.x2) / 2);
    int32_t const cy = (int32_t)(((int32_t)area.y1 + (int32_t)area.y2) / 2);

    for (int tap = 0; tap < 5; tap++) {
        ff_ctl_loop_boot_press(&ctx);
        if (tap < 4) {
            ctx.mock_clock_ms += QF_EXTRA_GAP_MS;
        }
        ff_ctl_loop_pump(&ctx);
    }
    TEST_ASSERT_TRUE_MESSAGE(ctx.state.flare.sending, "quick flare did not start — test setup invalid");

    /* The 5th BOOT press's `ff_ctl_loop_pump` just tore down and rebuilt
     * the WHOLE launcher tree (a legitimate dirty transition — `sending`
     * flipping true) but never ran an actual LVGL layout/refresh pass
     * over the fresh tree (`ff_ctl_loop_pump` never calls
     * `lv_timer_handler`/`lv_refr_now`) — so the new `dim_catcher`'s own
     * `coords` are not yet resolved to its full-puck size at this exact
     * instant. `lv_refr_now` forces that layout pass now, same as every
     * other ctl test that taps right after a face rebuild
     * (`S10_quick_flare_launcher_cancel_button_ends_the_send`'s own
     * `lv_refr_now` call above the sibling CANCEL tap). Without this, the
     * very first synthetic press after a rebuild can hit-test against
     * stale (pre-layout) coordinates — a harness artifact, not a real
     * on-glass timing (a human's finger cannot physically move fast
     * enough to land inside that window). */
    lv_refr_now(ctx.disp);

    ff_ctl_loop_pointer_press(&ctx, cx, cy);
    ff_ctl_loop_pointer_release(&ctx);
    ff_ctl_loop_pointer_step(&ctx);
    ff_ctl_loop_pump(&ctx);
    lv_timer_handler();

    TEST_ASSERT_TRUE_MESSAGE(ctx.state.flare.sending,
                              "a tap on the dimmed area (the SETTINGS satellite, ~30px off CANCEL) ended the send "
                              "or otherwise reached something underneath — it must be fully absorbed");
    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_LAUNCHER, ctx.state.active_face,
                               "a dim-area tap navigated to SETTINGS underneath the sender overlay — "
                               "the dim layer is not absorbing taps (fix/flare-cancel-taps root cause (b))");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S10_quick_flare_five_boot_presses_from_off_starts_sending_and_renders_overlay);
    RUN_TEST(S10_quick_flare_four_boot_presses_do_not_start_sending);
    RUN_TEST(S10_quick_flare_launcher_cancel_button_ends_the_send);
    RUN_TEST(S10_wire_quick_flare_five_boot_presses_puts_one_flare_frame_on_the_wire);

    RUN_TEST(S10_quick_flare_cancel_straddling_a_countdown_bucket_boundary_ends_the_send);
    RUN_TEST(S10_quick_flare_zero_rebuilds_across_30s_while_sending);
    RUN_TEST(S10_quick_flare_dim_area_tap_emits_nothing);

    return UNITY_END();
}
