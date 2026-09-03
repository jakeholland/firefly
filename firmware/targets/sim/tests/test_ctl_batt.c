/**
 * test_ctl_batt.c — S25 slice c (device follow-up PR): the sim's `batt_mv`
 * ctl command actually reaches a live shell and renders on Radar's status
 * bar, through the REAL ctl socket + core filter path (ctl_server.h's
 * `batt_mv` handler -> ctl_loop.c's `ctl_loop_batt_mv` ->
 * `ff_shell_set_batt_mv` -> `ff_batt_filter_push` -> the projected
 * `radar.batt_pct` a real `ff_shell_tick` produces) — not a direct unit
 * call to `ff_batt_pct_from_mv`/`ff_batt_filter_push` (core/tests/
 * test_batt.c already pins that half in isolation).
 *
 * What this proves that the core-only tests cannot:
 *  - the ctl `batt_mv` command exists and is wired all the way to the
 *    live shell (a parse-layer regression here would pass every core
 *    test and still leave the device's own periodic push — the actual
 *    thing this PR ships in app_main.c — silently unverified);
 *  - the FIRST reading is shown immediately (no warm-up delay,
 *    `ff_batt_filter_t`'s own documented "first ever real sample"
 *    exemption) and renders as the exact "%d%%" string
 *    `scr_radar.c`'s `radar_build_status_bar` builds;
 *  - `mv == 0` renders the honest "--%" un-set state, never a fabricated
 *    percent;
 *  - a low reading renders BOTH the correct percent AND the amber
 *    low-battery tint (`ff_radar_batt_is_low`, shared by every renderer
 *    — see that function's own doc comment), through the real render
 *    path rather than asserting on the core boolean alone.
 *
 * Percentages below are the OCV table's own arithmetic
 * (`core/src/ff_batt.c`'s `k_ocv_table` + its round-to-nearest
 * interpolation), computed independently here rather than copied from
 * that file, so a table edit that silently changes a breakpoint would
 * be caught by both suites disagreeing, not hidden by one copying the
 * other's expected value:
 *   - 3700 mV is an exact breakpoint: 30%.
 *   - 3400 mV interpolates between 3300->0% and 3500->5%:
 *     pct = 0 + round(100 * 5 / 200) = round(2.5) = 3% (the "+span_mv/2"
 *     rounding in ff_batt_pct_from_mv rounds .5 up) — comfortably at or
 *     under FF_BATT_LOW_PCT (15), so amber.
 */
#include <string.h>

#include "unity.h"

#include "ctl_loop.h"
#include "ctl_server.h"
#include "ff_app_state.h"
#include "ff_intent.h"
#include "ff_radar.h"
#include "ff_shell.h"
#include "ff_theme.h"

#include "fp_pack.h"

void setUp(void) {}

/* P0 harness-hang fix (debt/test-harness PR) — same convention every
 * other ctl-loop test file in this directory uses (see
 * test_ctl_flare_sequence.c's tearDown for the full repro/rationale):
 * each test owns its own lv_init()/lv_deinit() pairing (lv_init()
 * happens inside ff_ctl_loop_open; each test calls lv_deinit() itself as
 * its last line); this tearDown is only the safety net for the
 * TEST_ASSERT-longjmps-past-the-final-lv_deinit failure path. */
void tearDown(void)
{
    if (lv_is_initialized()) {
        lv_deinit();
    }
}

/* Same recursive lookup as test_scr_inbox_hint.c's find_label_with_text
 * (duplicated rather than shared — that file's own header comment on
 * find_button_with_label explains why: no shared-header dependency for
 * one small test helper). Radar's battery label (scr_radar.c's
 * `radar_build_status_bar`) is a plain lv_label_create child, never
 * inside a button, so this — not find_button_with_label — is the right
 * lookup. */
static lv_obj_t *find_label_with_text(lv_obj_t *root, char const *text)
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
        lv_obj_t *found = find_label_with_text(child, text);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static void ctl_send(ff_ctl_handlers_t const *h, char const *cmd, char *resp, size_t resp_sz)
{
    (void)ff_ctl_process_line(cmd, h, resp, resp_sz);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(resp, "\"ok\":true"), resp);
}

static void ctl_clock_advance(ff_ctl_handlers_t const *h, uint32_t ms)
{
    char cmd[64], resp[128];
    int n = snprintf(cmd, sizeof(cmd), "{\"cmd\":\"clock\",\"advance_ms\":%u}", (unsigned)ms);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(cmd));
    ctl_send(h, cmd, resp, sizeof(resp));
}

/* Same settle choreography as test_ctl_flare_sequence.c's ctl_settle:
 * advance the mock clock, pump the shell (tick + rebuild-if-dirty), and
 * force a layout pass so a freshly-built label's text/coords are
 * current before this file's find_label_with_text ever looks at it. */
static void ctl_settle(ff_ctl_loop_ctx_t *ctx, ff_ctl_handlers_t const *h)
{
    ctl_clock_advance(h, 50);
    lv_timer_handler();
    ff_ctl_loop_pump(ctx);
    lv_timer_handler();
    lv_refr_now(ctx->disp);
}

/* Opens a live ctl session and navigates it from the boot-default
 * launcher to Radar (FF_INTENT_LAUNCHER_SELECT, index 0 — the launcher's
 * own fixed circle order, ff_shell.c's FF_INTENT_LAUNCHER_SELECT case) —
 * scaffolding, not what this file is testing (that navigation path has
 * its own coverage: app/tests/test_intent.c, test_ctl_flare_sequence.c's
 * launcher-tap tests), so it's reached directly through the intent seam
 * rather than a real tap on the launcher's hub circle. */
static void open_batt_session_on_radar(ff_ctl_loop_ctx_t *ctx, ff_shell_t *shell, fp_pack_t *pack,
                                        ff_ctl_handlers_t *h, bool *quit_flag)
{
    ff_shell_cfg_t shell_cfg;
    memset(&shell_cfg, 0, sizeof(shell_cfg));

    ff_ctl_loop_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mock_clock = true;

    TEST_ASSERT_EQUAL_INT(0, ff_ctl_loop_open(ctx, shell, pack, &shell_cfg, &cfg));

    *h = ff_ctl_loop_handlers(ctx, quit_flag);
    ctl_settle(ctx, h);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ctx->state.active_face); /* the boot default */

    ff_intent_t const sel = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {.launcher_idx = 0u}};
    ff_shell_intent(shell, &sel);
    ctl_settle(ctx, h);
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, ctx->state.active_face);
}

/* S25c: the first-ever push is shown immediately (ff_batt_filter_t's
 * documented "first ever real sample" exemption, ff_batt.h) — no
 * warm-up delay, no hysteresis. 3700 mV is an exact OCV breakpoint
 * (this file's top comment): 30%, muted (not low). */
static void batt_mv_3700_shows_30_pct_on_radar(void)
{
    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_ctl_loop_ctx_t ctx;
    ff_ctl_handlers_t h;
    bool quit_flag = false;

    open_batt_session_on_radar(&ctx, &shell, &pack, &h, &quit_flag);

    char resp[256];
    ctl_send(&h, "{\"cmd\":\"batt_mv\",\"mv\":3700}", resp, sizeof(resp));
    ctl_settle(&ctx, &h);

    TEST_ASSERT_EQUAL_INT8(30, ctx.state.radar.batt_pct);
    lv_obj_t *lbl = find_label_with_text(lv_screen_active(), "30%");
    TEST_ASSERT_NOT_NULL_MESSAGE(lbl, "battery label showing \"30%\" not found on Radar");
    lv_color_t const color = lv_obj_get_style_text_color(lbl, 0);
    /* lv_color_to_u32 sets the top byte to opaque alpha (0xFF......) —
     * mask it off before comparing against the RGB-only theme constant. */
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(FF_THEME_COLOR_MUTED, lv_color_to_u32(color) & 0x00FFFFFFu,
                                     "30% is well above the low-battery threshold — must render muted, not amber");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* S25c: `mv == 0` is ff_batt.h's documented "no reading" sentinel — must
 * render the honest un-set "--%" state, never a fabricated percent
 * (CLAUDE.md: "honest data over pretty data"). */
static void batt_mv_0_shows_unset_pct_on_radar(void)
{
    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_ctl_loop_ctx_t ctx;
    ff_ctl_handlers_t h;
    bool quit_flag = false;

    open_batt_session_on_radar(&ctx, &shell, &pack, &h, &quit_flag);

    char resp[256];
    ctl_send(&h, "{\"cmd\":\"batt_mv\",\"mv\":0}", resp, sizeof(resp));
    ctl_settle(&ctx, &h);

    TEST_ASSERT_EQUAL_INT8(-1, ctx.state.radar.batt_pct);
    lv_obj_t *lbl = find_label_with_text(lv_screen_active(), "--%");
    TEST_ASSERT_NOT_NULL_MESSAGE(lbl, "battery label showing the unset \"--%\" state not found on Radar");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* S25c: 3400 mV interpolates to 3% (this file's top comment) — at or
 * under FF_BATT_LOW_PCT (15, ff_radar.h), so BOTH the percent AND the
 * amber low-battery tint must show, through the shared
 * `ff_radar_batt_is_low` classification every renderer uses. */
static void batt_mv_3400_shows_low_pct_amber_on_radar(void)
{
    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_ctl_loop_ctx_t ctx;
    ff_ctl_handlers_t h;
    bool quit_flag = false;

    open_batt_session_on_radar(&ctx, &shell, &pack, &h, &quit_flag);

    char resp[256];
    ctl_send(&h, "{\"cmd\":\"batt_mv\",\"mv\":3400}", resp, sizeof(resp));
    ctl_settle(&ctx, &h);

    TEST_ASSERT_EQUAL_INT8(3, ctx.state.radar.batt_pct);
    TEST_ASSERT_TRUE_MESSAGE(ff_radar_batt_is_low(ctx.state.radar.batt_pct),
                              "3% must classify as low battery (FF_BATT_LOW_PCT=15)");

    lv_obj_t *lbl = find_label_with_text(lv_screen_active(), "3%");
    TEST_ASSERT_NOT_NULL_MESSAGE(lbl, "battery label showing \"3%\" not found on Radar");
    lv_color_t const color = lv_obj_get_style_text_color(lbl, 0);
    /* See the sibling test's identical mask note above. */
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(FF_THEME_COLOR_STALE_AMBER, lv_color_to_u32(color) & 0x00FFFFFFu,
                                     "3% is at/under the low-battery threshold — must render amber, not muted");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* Parse-layer guard: `mv` out of `uint16_t` range is rejected before it
 * ever reaches a narrowing cast — same discipline `tap`'s x/y bounds
 * check documents (ctl_server.c). Regression coverage for this new
 * command's own bound, not a duplicate of tap's. */
static void batt_mv_out_of_range_is_rejected(void)
{
    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_ctl_loop_ctx_t ctx;
    ff_ctl_handlers_t h;
    bool quit_flag = false;

    open_batt_session_on_radar(&ctx, &shell, &pack, &h, &quit_flag);

    char resp[256];
    (void)ff_ctl_process_line("{\"cmd\":\"batt_mv\",\"mv\":70000}", &h, resp, sizeof(resp));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(resp, "\"ok\":false"), resp);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(resp, "0, 65535"), resp);

    /* Nothing was ever pushed — the shell still reads the un-set state. */
    ctl_settle(&ctx, &h);
    TEST_ASSERT_EQUAL_INT8(-1, ctx.state.radar.batt_pct);

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(batt_mv_3700_shows_30_pct_on_radar);
    RUN_TEST(batt_mv_0_shows_unset_pct_on_radar);
    RUN_TEST(batt_mv_3400_shows_low_pct_amber_on_radar);
    RUN_TEST(batt_mv_out_of_range_is_rejected);

    return UNITY_END();
}
