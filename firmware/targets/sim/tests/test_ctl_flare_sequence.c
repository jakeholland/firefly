/**
 * test_ctl_flare_sequence.c — S16 slice d, AC10.
 *
 * "Sequence test via the ctl socket (not the single-frame golden
 * harness): draft typed -> flare injected -> takeover renders ->
 * takeover cleared -> composer returns with draft intact. Requires a new
 * ctl `flare` command."
 *
 * Drives a REAL `ff-sim-ctl-loop` session (the same object main.c's
 * `--headless --ctl PORT` uses) through `ff_ctl_process_line` — the ctl
 * socket's pure, socket-free command-processing layer (ctl_server.h's
 * own documented split; no real TCP client, no docker, no `ffsim`
 * subprocess). What this proves that no other test in the suite does:
 *
 *  - the ctl `flare` command exists and actually reaches a live shell
 *    (ctl_loop.c's ctl_loop_flare);
 *  - the render lifecycle (S16 slice d: rebuild only on a dirty
 *    ff_shell_tick, always lv_obj_clean() first) correctly swaps the
 *    LVGL tree between the composer and the full-screen takeover and
 *    back, through real ctl-driven ticks — not just a unit-level
 *    ff_shell_intent call (test_shell.c's S16_AC3b_... already pins the
 *    core-only half of this);
 *  - a real `{"cmd":"tap"}` at real button coordinates, discovered from
 *    the actual built tree, delivers a genuine LVGL click through the
 *    synthetic pointer indev end to end (T9 key -> draft; DISMISS ->
 *    takeover cleared) — not `lv_obj_send_event` shortcuts.
 *
 * WHY EVERY TAP IS WRAPPED IN ctl_settle() ON BOTH SIDES. LVGL's indev
 * read timer only re-polls once its period (LV_DEF_REFR_PERIOD, 33 ms)
 * has genuinely elapsed since it last ran (lv_timer.c's
 * lv_timer_time_remaining) — the exact fact
 * app/screens/tests/test_scr_intent.c's drag() helper documents and
 * works around for its own physical-swipe tests. `ctl_loop_tap`'s press
 * and release both happen inside ONE synchronous call with no elapsed
 * time between them, so neither half is actually delivered to LVGL
 * unless something else advances the clock and pumps the timer both
 * BEFORE the tap (so the press half — the first of the pair to run —
 * finds the timer's period already satisfied and fires) and AFTER it
 * (so the release half, otherwise stranded until some later poll,
 * fires too). `--mock-clock` plus the ctl `clock` command is exactly the
 * documented, supported way to drive that by hand (see CTL.md).
 *
 * WHY THIS TEST NEVER USES ctl `swipe` TO REACH COMPOSE. Getting from
 * Radar to Signals to Compose is scaffolding, not what AC10 is about —
 * and it is already covered, more directly, by
 * app/screens/tests/test_scr_intent.c's physical swipe tests and
 * ff_route's own unit tests. This test instead calls `ff_shell_intent`
 * directly with `FF_INTENT_OPEN_COMPOSE`, the exact same entry point
 * `ff_shell_intent_sink` (bound to the ctl session by ff_ctl_loop_open)
 * would forward a real tap to — legitimate scaffolding, not a shortcut
 * around the seam under test.
 */
#include <string.h>

#include "unity.h"

#include "ctl_loop.h"
#include "ctl_server.h"
#include "ff_app_state.h"
#include "ff_flare.h"
#include "ff_intent.h"
#include "ff_shell.h"

#include "fp_pack.h"

void setUp(void) {}
void tearDown(void) {}

/* Same recursive lookup as app/screens/tests/test_scr_intent.c's
 * find_button_with_label — duplicated rather than shared (that file's
 * own header comment explains why: no shared-header dependency for one
 * small helper). */
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

/* Advances the mock clock and gives every due timer a chance to actually
 * run, then pumps the shell (ff_ctl_loop_pump — tick, and rebuild only
 * if dirty, the same step main.c's real loop runs every iteration).
 * `lv_refr_now` forces the layout pass current production code only ever
 * gets from the display's own refresh timer firing over real elapsed
 * time (ctl_loop_screenshot's own `lv_refr_now` call exists for exactly
 * this reason) — without it, a freshly-built object's `coords` stay at
 * LVGL's zeroed default until SOME refresh happens to run, and
 * `lv_obj_get_click_area` (unlike this test's coordinate-discovery
 * helper below) never triggers layout itself. Used after something that
 * changed shell state WITHOUT going through the synthetic pointer indev
 * (a direct ff_shell_intent call, the `flare` command) — see ctl_tap
 * below for why a real tap needs a different, more careful sequence. */
static void ctl_settle(ff_ctl_loop_ctx_t *ctx, ff_ctl_handlers_t const *h)
{
    ctl_clock_advance(h, 50);
    lv_timer_handler();
    ff_ctl_loop_pump(ctx);
    lv_timer_handler();
    lv_refr_now(ctx->disp);
}

/**
 * One real ctl "tap" at (x, y) that LVGL actually delivers as a
 * press-then-release CLICKED event — not just a state change nothing
 * ever polls. See this file's top comment for the full reasoning; the
 * short version: `ctl_loop_tap` sets PRESSED, calls `lv_timer_handler()`,
 * sets RELEASED, calls `lv_timer_handler()` again, all synchronously with
 * NO elapsed time in between — so BOTH of those calls only do anything
 * if LVGL's indev read timer's period (33 ms) has already elapsed since
 * it last ran:
 *
 *  1. Advance the clock (no firing yet) BEFORE the tap, so the timer is
 *     already stale when the tap's own first `lv_timer_handler()` call
 *     (the press) runs — that call fires, registers the press, and
 *     resets the timer's "last ran" mark to the current (just-advanced)
 *     tick. The tap's SECOND call (the release) then has zero elapsed
 *     time and does nothing — the release is set but not yet delivered.
 *  2. Advance the clock again AFTER the tap and pump once — NOW the
 *     timer is stale again, this call fires, reads the (already-set)
 *     RELEASED state, and — since the indev's own internal state is
 *     still "pressed" from step 1 — delivers a press-then-release
 *     transition: a genuine click.
 */
static void ctl_tap(ff_ctl_loop_ctx_t *ctx, ff_ctl_handlers_t const *h, double x, double y)
{
    /* NOTE: no pre-advance here anymore, deliberately. ctl_loop_tap now
     * advances time internally around press and release (the PR #62 fix —
     * over the real socket loop, a pump after every command consumed any
     * staleness a prior `clock` command left, and the tap was lost while
     * replying ok). Running AC10 WITHOUT external choreography is what
     * pins that self-sufficiency: re-adding a dependency on caller-side
     * clock staging would make this suite pass while socket-driven taps
     * break again. */

    char cmd[128], resp[256];
    int n = snprintf(cmd, sizeof(cmd), "{\"cmd\":\"tap\",\"x\":%.2f,\"y\":%.2f}", x, y);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(cmd));
    ctl_send(h, cmd, resp, sizeof(resp)); /* press registers now; release is stranded until the next poll */

    ctl_clock_advance(h, 50); /* step 2: let the stranded release actually reach LVGL */
    lv_timer_handler();
    ff_ctl_loop_pump(ctx);
    lv_timer_handler();
    lv_refr_now(ctx->disp);
}

/* Taps the center of a button already found in the live tree. */
static void ctl_tap_button(ff_ctl_loop_ctx_t *ctx, ff_ctl_handlers_t const *h, lv_obj_t *btn)
{
    TEST_ASSERT_NOT_NULL(btn);
    lv_area_t area;
    lv_obj_get_click_area(btn, &area);
    double x = ((double)area.x1 + (double)area.x2) / 2.0;
    double y = ((double)area.y1 + (double)area.y2) / 2.0;
    ctl_tap(ctx, h, x, y);
}

static void S16_AC10_draft_typed_flare_injected_takeover_clears_draft_survives(void)
{
    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_ctl_loop_ctx_t ctx;

    ff_shell_cfg_t shell_cfg;
    memset(&shell_cfg, 0, sizeof(shell_cfg));

    ff_ctl_loop_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mock_clock = true; /* the ctl `clock` command — CTL.md — is what makes this test deterministic */

    TEST_ASSERT_EQUAL_INT(0, ff_ctl_loop_open(&ctx, &shell, &pack, &shell_cfg, &cfg));

    bool quit_flag = false;
    ff_ctl_handlers_t h = ff_ctl_loop_handlers(&ctx, &quit_flag);

    /* Reach Compose directly through the intent seam's real entry point
     * (see this file's top comment for why this isn't a shortcut around
     * what AC10 tests) — the same call ff_shell_intent_sink (bound by
     * ff_ctl_loop_open) would make for a real Signals "+" tap. */
    ff_intent_t open_compose = {.kind = FF_INTENT_OPEN_COMPOSE, .u = {.node_id = 0u}};
    ff_shell_intent(&shell, &open_compose);
    ctl_settle(&ctx, &h);

    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, ctx.state.active_face);
    TEST_ASSERT_EQUAL_STRING("", ctx.state.compose.text);

    /* --- draft typed: a real ctl "tap" on the "DEF" key --------------- */
    lv_obj_t *def_key = find_button_with_label(lv_screen_active(), "DEF");
    TEST_ASSERT_NOT_NULL_MESSAGE(def_key, "compose keypad's DEF key not found — is Compose actually built?");
    ctl_tap_button(&ctx, &h, def_key);

    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, ctx.state.active_face);
    TEST_ASSERT_EQUAL_STRING("d", ctx.state.compose.text); /* ff_t9's key-3 table: "def", first press */

    /* --- flare injected: the new ctl `flare` command ------------------ */
    enum { FLARE_FROM = 0xDA1Au, FLARE_DUR_S = 300u };
    char resp[FF_CTL_MAX_RESP]; /* the `state` dump at the bottom needs the full budget, not just a small reply */
    char flare_cmd[128];
    int fn = snprintf(flare_cmd, sizeof(flare_cmd), "{\"cmd\":\"flare\",\"from\":%u,\"dur_s\":%u}", (unsigned)FLARE_FROM,
                       (unsigned)FLARE_DUR_S);
    TEST_ASSERT_TRUE(fn > 0 && (size_t)fn < sizeof(flare_cmd));
    ctl_send(&h, flare_cmd, resp, sizeof(resp));
    ctl_settle(&ctx, &h);

    /* --- takeover renders ---------------------------------------------
     * AC13: active_face is never FLARE — the composer stays "the visible
     * modal" per the route, and the takeover is ff_flare_t's own fact.
     * What actually reaches the screen is checked structurally: the
     * takeover's GO/DISMISS buttons exist and the composer's own SEND
     * button does not (S16 slice d's rebuild REPLACED the tree, it did
     * not merely draw on top of it). */
    TEST_ASSERT_TRUE(ff_shell_flare(&shell)->takeover_active);
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, ctx.state.active_face);
    lv_obj_t *dismiss_btn = find_button_with_label(lv_screen_active(), "DISMISS");
    TEST_ASSERT_NOT_NULL_MESSAGE(dismiss_btn, "takeover DISMISS button not found — did the screen actually rebuild?");
    TEST_ASSERT_NULL_MESSAGE(find_button_with_label(lv_screen_active(), "SEND"),
                              "composer's SEND button is still on screen during a takeover");

    /* The draft survives UNDERNEATH, even though it is not what is
     * currently rendered (AC3b, exercised here through the real ctl/LVGL
     * stack rather than a direct ff_shell_intent call). */
    TEST_ASSERT_EQUAL_STRING("d", ctx.state.compose.text);

    /* --- takeover cleared: a real ctl "tap" on DISMISS ----------------- */
    ctl_tap_button(&ctx, &h, dismiss_btn);

    TEST_ASSERT_FALSE(ff_shell_flare(&shell)->takeover_active);

    /* --- composer returns with draft intact ---------------------------- */
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, ctx.state.active_face);
    TEST_ASSERT_EQUAL_STRING("d", ctx.state.compose.text);
    TEST_ASSERT_NOT_NULL_MESSAGE(find_button_with_label(lv_screen_active(), "SEND"),
                                  "composer's SEND button did not come back after the takeover cleared");

    /* One direct check that the ctl `state` dump — not just the C
     * struct this test has been reading — surfaces the same facts
     * (AC10's "through the ctl socket", read end to end). */
    ctl_send(&h, "{\"cmd\":\"state\"}", resp, sizeof(resp));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(resp, "\"face\":\"compose\""), resp);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(resp, "\"text\":\"d\""), resp);
    /* S16 slice e: the ctl `state` dump exposes link state too, same
     * bench-visibility rationale as "wall" — this session never opened a
     * transport (no --connect), so the honest reading is "none". */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(resp, "\"link\":\"none\""), resp);

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* PR #60 review finding: the slice's headline claim — "rebuild only on a
 * dirty tick, never every frame" — had no regression test. The reviewer
 * proved it by mutation: ignoring `dirty` in ctl_loop.c passed the entire
 * suite, because nothing exercised an idle stretch long enough to notice.
 * Proxy: the suite is green. Property: idle frames don't rebuild.
 *
 * Observable: a rebuild is `lv_obj_clean(screen)` + rebuild, so the
 * screen's first child (the puck) is destroyed and recreated. Pointer
 * identity is NOT a sound observable for that — writing this test
 * proved it: LVGL's allocator reused the freed block and the rebuilt
 * puck landed at the same address, failing the positive control. A
 * user_data sentinel is sound in both directions: it survives on a
 * live object and cannot survive recreation (the constructor zeroes
 * user_data), regardless of where the new object is allocated. The
 * positive control keeps the observable honest either way. */
static void S16_d_idle_ticks_never_rebuild_the_screen(void)
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
    ff_ctl_handlers_t h = ff_ctl_loop_handlers(&ctx, &quit_flag);

    ctl_settle(&ctx, &h); /* initial build */
    lv_obj_t *puck_before = lv_obj_get_child(lv_screen_active(), 0);
    TEST_ASSERT_NOT_NULL(puck_before);
    static int sentinel; /* any stable address */
    lv_obj_set_user_data(puck_before, &sentinel);

    /* 200 idle ticks. No traffic, no intents, no flare: nothing rendered
     * can change (no crew/feed rows to age; wall clock UNKNOWN so no
     * clock string). Any rebuild here is the dirty gate being ignored. */
    for (int i = 0; i < 200; i++) {
        ctl_clock_advance(&h, 50);
        lv_timer_handler();
        ff_ctl_loop_pump(&ctx);
    }
    lv_obj_t *puck_after_idle = lv_obj_get_child(lv_screen_active(), 0);
    TEST_ASSERT_NOT_NULL(puck_after_idle);
    TEST_ASSERT_EQUAL_PTR_MESSAGE(&sentinel, lv_obj_get_user_data(puck_after_idle),
                                   "screen was rebuilt during idle ticks — the dirty gate is being ignored");

    /* Positive control: a real state change must rebuild (pointer moves),
     * proving the observable can detect rebuilds at all. */
    char resp[FF_CTL_MAX_RESP];
    ctl_send(&h, "{\"cmd\":\"flare\",\"from\":55834,\"dur_s\":60}", resp, sizeof(resp));
    ctl_settle(&ctx, &h);
    lv_obj_t *puck_after_flare = lv_obj_get_child(lv_screen_active(), 0);
    TEST_ASSERT_NOT_NULL(puck_after_flare);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(&sentinel, lv_obj_get_user_data(puck_after_flare),
                                   "positive control failed: a takeover did not rebuild the screen, so this "
                                   "test's sentinel observable can no longer detect rebuilds");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* ctl `swipe` regression: the command used to run its press/moves/release
 * with zero elapsed time between steps, so LVGL's indev timer polled at
 * most once, no gesture was recognized, and the face never changed while
 * the command replied ok. This drives a real swipe end to end and asserts
 * the face actually moved. (This file's top comment explains why AC10
 * itself deliberately avoided `swipe`; that avoidance is also why the bug
 * survived until someone drove the command for real.) */
static void ctl_swipe_actually_changes_the_face(void)
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
    ff_ctl_handlers_t h = ff_ctl_loop_handlers(&ctx, &quit_flag);

    ctl_settle(&ctx, &h);
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, ctx.state.active_face);

    char resp[256];
    ctl_send(&h, "{\"cmd\":\"swipe\",\"dir\":\"left\"}", resp, sizeof(resp));
    ctl_settle(&ctx, &h);
    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_NOW, ctx.state.active_face,
                               "ctl swipe left did not move radar -> now");

    ctl_send(&h, "{\"cmd\":\"swipe\",\"dir\":\"right\"}", resp, sizeof(resp));
    ctl_settle(&ctx, &h);
    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_RADAR, ctx.state.active_face,
                               "ctl swipe right did not move now -> radar");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* ctl `hold` (issue #70): press-and-hold at (x, y) for `ms`, then
 * release. Unlike ctl_tap, ctl_loop_hold's own internal step loop
 * already advances the mock clock and pumps lv_timer_handler enough
 * times to deliver the whole press->hold->release sequence — including,
 * for a long enough `ms`, the LONG_PRESSED event itself — before the
 * ctl command even replies. No stranded-release choreography needed
 * here the way ctl_tap needs (see that helper's comment): only
 * ctl_settle afterward, to let ff_shell_tick project whatever the
 * gesture did into ctx->state and rebuild the screen. */
static void ctl_hold(ff_ctl_handlers_t const *h, double x, double y, uint32_t ms, char *resp, size_t resp_sz)
{
    char cmd[128];
    int n = snprintf(cmd, sizeof(cmd), "{\"cmd\":\"hold\",\"x\":%.2f,\"y\":%.2f,\"ms\":%u}", x, y, (unsigned)ms);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(cmd));
    ctl_send(h, cmd, resp, resp_sz);
}

/* issue #70: tap's press/release is a fixed ~40ms, well under LVGL's
 * ~400ms long_press_time, so the ONE gesture that opens Settings
 * (scr_nav.c's nav_long_press_cb, reached by a long-press anywhere on
 * the tileview) could not be driven through the ctl socket at all —
 * blocking the Settings/Map UX reviews (PR #68) from driving those
 * faces live. Drives a REAL long-press through ff_ctl_process_line and
 * asserts the face actually reaches Settings, the same way
 * ctl_swipe_actually_changes_the_face asserts the face actually moved.
 *
 * Mutation-conscious (standing brief: "what input satisfies the proxy
 * and violates the property?"): a `hold` implementation that always
 * held "long enough" regardless of `ms` (e.g. one that hardcoded enough
 * steps and ignored the caller's value) would still pass a test that
 * only checked the positive case. The negative case below — an 80ms
 * hold, well under the 400ms threshold — pins the other direction: it
 * must NOT open Settings. (228, 228) is the puck/window center — the
 * exact reachability point app/screens/tests/test_scr_intent.c's own
 * `S16_c3_physical_long_press_on_empty_puck_space_reaches_open_settings`
 * documents as "nothing clickable sits here in this state" on Radar. */
static void ctl_hold_opens_settings_but_short_hold_does_not(void)
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
    ff_ctl_handlers_t h = ff_ctl_loop_handlers(&ctx, &quit_flag);

    ctl_settle(&ctx, &h);
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, ctx.state.active_face);

    char resp[256];

    /* --- negative control: below threshold, Settings must NOT open --- */
    ctl_hold(&h, 228.0, 228.0, 80, resp, sizeof(resp));
    ctl_settle(&ctx, &h);
    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_RADAR, ctx.state.active_face,
                              "an 80ms ctl hold (well under LVGL's 400ms long_press_time) opened Settings");

    /* --- positive: FF_CTL_HOLD_DEFAULT_MS (600ms) opens Settings ------ */
    ctl_hold(&h, 228.0, 228.0, 600, resp, sizeof(resp));
    ctl_settle(&ctx, &h);
    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_SETTINGS, ctx.state.active_face, "ctl hold did not open Settings");

    /* Read end to end through the `state` dump too, same "not just the C
     * struct" check AC10 makes at its own finish line. */
    char state_resp[FF_CTL_MAX_RESP];
    ctl_send(&h, "{\"cmd\":\"state\"}", state_resp, sizeof(state_resp));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(state_resp, "\"face\":\"settings\""), state_resp);

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* ctl `wall`: bench time-travel so real festpacks whose dates are not
 * "now" can be tested live (a finished Bass Canyon, a future Lost
 * Lands). Expected values computed independently (Python datetime):
 * 2026-09-19 21:30 EDT = unix 1789867800, day_doy 262, now_min 1290. */
static void ctl_wall_sets_bench_time_honestly(void)
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
    ff_ctl_handlers_t h = ff_ctl_loop_handlers(&ctx, &quit_flag);
    ctl_settle(&ctx, &h);

    char resp[FF_CTL_MAX_RESP];

    /* No pack, no settings offset: a plausible time still cannot resolve
     * to local time, and the reply must say so rather than "ok". */
    (void)ff_ctl_process_line("{\"cmd\":\"wall\",\"unix_s\":1789867800}", &h, resp, sizeof(resp));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(resp, "\"ok\":false"), resp);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(resp, "offset"), resp);

    /* Supply the offset through the real settings intent (EDT, -240). */
    ff_intent_t set = {.kind = FF_INTENT_SETTING_SET, .u = {.setting = {.id = FF_SETTING_UTC_OFFSET_MIN, .v = {.i = -240}}}};
    ff_shell_intent(&shell, &set);
    ctl_settle(&ctx, &h);

    ctl_send(&h, "{\"cmd\":\"wall\",\"unix_s\":1789867800}", resp, sizeof(resp));
    ctl_settle(&ctx, &h);

    ff_wall_t w = ff_shell_wall(&shell);
    TEST_ASSERT_NOT_EQUAL(FF_WALL_UNKNOWN, w.src);
    TEST_ASSERT_EQUAL_UINT16(262u, w.day_doy);
    TEST_ASSERT_EQUAL_INT16(1290, w.now_min);

    /* Beyond ff_wall's ceiling (2031): the gate, not this command,
     * rejects it — the reply says REJECTED (from the observe's own
     * return; "resolvable afterwards" stays true off the earlier latch
     * and was exactly the wrong proxy) — and the previously-set time
     * survives untouched. */
    (void)ff_ctl_process_line("{\"cmd\":\"wall\",\"unix_s\":1924992000}", &h, resp, sizeof(resp));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(resp, "\"ok\":false"), resp);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(resp, "rejected"), resp);
    w = ff_shell_wall(&shell);
    TEST_ASSERT_EQUAL_UINT16(262u, w.day_doy);

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S16_AC10_draft_typed_flare_injected_takeover_clears_draft_survives);
    RUN_TEST(S16_d_idle_ticks_never_rebuild_the_screen);
    RUN_TEST(ctl_swipe_actually_changes_the_face);
    RUN_TEST(ctl_hold_opens_settings_but_short_hold_does_not);
    RUN_TEST(ctl_wall_sets_bench_time_honestly);

    return UNITY_END();
}
