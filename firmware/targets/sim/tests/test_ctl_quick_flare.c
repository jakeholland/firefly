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

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S10_quick_flare_five_boot_presses_from_off_starts_sending_and_renders_overlay);
    RUN_TEST(S10_quick_flare_four_boot_presses_do_not_start_sending);

    return UNITY_END();
}
