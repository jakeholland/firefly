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

/* P0 harness-hang fix (debt/test-harness PR). Every test below owns its
 * own lv_init()/lv_deinit() pairing (lv_init() happens inside
 * ff_ctl_loop_open; each test calls lv_deinit() itself as its last line)
 * instead of a shared setUp/tearDown pair — but Unity runs setUp()+the
 * test body under ONE TEST_PROTECT() and tearDown() under a SEPARATE
 * one, so a failed TEST_ASSERT anywhere before that final lv_deinit()
 * longjmps past it and leaves LVGL initialized. LVGL v9's lv_init() is
 * idempotent ("do nothing if already initialized"), so the NEXT test's
 * ff_ctl_loop_open -> lv_init() silently no-ops on top of that leaked
 * state instead of starting fresh — reproduced (see
 * tests/test_wakeonly_touch.c's own tearDown comment for the full
 * repro/verification writeup, same file layout, same bug) as a genuine,
 * silent (zero stdout) infinite hang rather than a second failure. This
 * tearDown is the safety net Unity always runs after every test
 * regardless of outcome: guarded by lv_is_initialized() so it's a no-op
 * on the normal path (the test body already deinited) and does the
 * actual cleanup on the failure path. */
void tearDown(void)
{
    if (lv_is_initialized()) {
        lv_deinit();
    }
}

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

/**
 * Maintainer-reported bug, reproduced end to end through the REAL ctl/
 * LVGL stack (not a direct ff_shell_intent call — that half is
 * app/tests/test_intent.c's `S26_notif_banner_open_navigates_to_the_
 * thread_from_every_base_face`): "tapping a message banner on the
 * launcher home screen does not open the sender's thread." From the
 * LAUNCHER (the boot default, and the maintainer's literal repro
 * state), a genuine synthetic tap on the rendered banner strip must
 * land on the sender's thread — the #157 `banner_on_launcher` fixture's
 * live counterpart, driven by an actual finger-shaped tap rather than a
 * fixture snapshot.
 */
static void S26_banner_tap_from_the_launcher_lands_on_the_thread(void)
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
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ctx.state.active_face); /* the boot default */

    /* A paired sender named DANA, then a text from her — the same
     * shape app/tests/test_shell.c's S26_AC3_paired_message_pushes_
     * banner injects, reached here through the shell's own events seam
     * (ff_shell_pair/ff_shell_events) rather than a ctl command, since
     * the ctl protocol has no "inject a text" verb (only `flare` does,
     * for AC10's own reasons — see this file's `ctl_loop_flare`). */
    enum { DANA = 0x0000DA1Au };
    TEST_ASSERT_TRUE(ff_shell_pair(&shell, DANA, true));
    mc_events_t const ev = ff_shell_events(&shell);
    mc_nodeinfo_t n;
    memset(&n, 0, sizeof(n));
    n.node_num = DANA;
    n.has_short_name = true;
    strncpy(n.short_name, "DANA", sizeof(n.short_name) - 1);
    ev.on_node(ev.user, &n);
    ev.on_text(ev.user, DANA, MC_ADDR_BROADCAST, "you close?", strlen("you close?"));
    ctl_settle(&ctx, &h);
    TEST_ASSERT_TRUE(ctx.state.banner.active);
    TEST_ASSERT_EQUAL_UINT32(DANA, ctx.state.banner.node_id);

    /* The real tap: find the rendered banner strip by its own sender-
     * name label (scr_banner.c's `name` label is a direct child of the
     * strip button it built) and tap its center — a genuine LVGL
     * press-then-release CLICKED, not a shortcut around the seam under
     * test. */
    lv_obj_t *banner_strip = find_button_with_label(lv_screen_active(), "DANA");
    TEST_ASSERT_NOT_NULL_MESSAGE(banner_strip, "banner strip not found on the launcher - did it render?");
    ctl_tap_button(&ctx, &h, banner_strip);

    TEST_ASSERT_EQUAL(FF_APP_FACE_INBOX, ctx.state.active_face);
    TEST_ASSERT_EQUAL(FF_INBOX_SUB_THREAD, ctx.state.inbox.subview);
    TEST_ASSERT_EQUAL_UINT32(DANA, ctx.state.inbox.thread_node);
    TEST_ASSERT_FALSE(ctx.state.banner.active);

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/**
 * The other half of the same maintainer report: "the FLARE takeover's
 * GO does not land on Radar." From the LAUNCHER, a real tap on GO must
 * land on Radar, locked on the sender. No `flare_takeover_on_launcher`
 * golden exists in this repo (the takeover is a full-screen, face-
 * independent overlay — `face_dispatch.c` dispatches it identically
 * regardless of which base it covers, so a launcher-specific rendering
 * golden would duplicate `flare_takeover.json`'s pixels for no
 * additional coverage); what was actually missing, and what this test
 * pins, is the ROUTE decision GO makes once dismissed — proven here via
 * a real tap through the live ctl/LVGL stack, same as the banner test
 * above.
 */
static void S26_flare_go_from_the_launcher_lands_on_radar(void)
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
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ctx.state.active_face); /* the boot default */

    enum { FLARE_FROM = 0xDA1Au, FLARE_DUR_S = 300u };
    char resp[FF_CTL_MAX_RESP];
    char flare_cmd[128];
    int fn = snprintf(flare_cmd, sizeof(flare_cmd), "{\"cmd\":\"flare\",\"from\":%u,\"dur_s\":%u}",
                       (unsigned)FLARE_FROM, (unsigned)FLARE_DUR_S);
    TEST_ASSERT_TRUE(fn > 0 && (size_t)fn < sizeof(flare_cmd));
    ctl_send(&h, flare_cmd, resp, sizeof(resp));
    ctl_settle(&ctx, &h);
    TEST_ASSERT_TRUE(ff_shell_flare(&shell)->takeover_active);
    /* AC13: the takeover overrides on top of the launcher without ever
     * being written into active_face — face_dispatch.c's takeover
     * branch runs before its active_face dispatch, so this was already
     * true before this PR; pinned here as the "reverse direction" the
     * brief calls out, alongside the GO fix this test exists for. */
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ctx.state.active_face);

    lv_obj_t *go_btn = find_button_with_label(lv_screen_active(), "GO");
    TEST_ASSERT_NOT_NULL_MESSAGE(go_btn, "takeover GO button not found - did it render over the launcher?");
    ctl_tap_button(&ctx, &h, go_btn);

    TEST_ASSERT_FALSE(ff_shell_flare(&shell)->takeover_active);
    TEST_ASSERT_EQUAL_UINT32(FLARE_FROM, ff_shell_flare(&shell)->locked_node_id);
    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_RADAR, ctx.state.active_face,
                              "GO did not navigate to Radar - the bug this PR fixes");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
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

    /* S08 addendum: the composer now opens in predictive mode
     * (FF_APP_COMPOSE_PRED), whose keypad is PR2. This AC10 test is about
     * the MULTITAP draft surviving a takeover, so switch to the ABC page
     * first (one T9_MODE press: PRED -> ABC), where the "DEF" key lives, and
     * let the screen rebuild. */
    ff_intent_t to_abc = {.kind = FF_INTENT_T9_MODE, .u = {0}};
    ff_shell_intent(&shell, &to_abc);
    ctl_settle(&ctx, &h);
    TEST_ASSERT_EQUAL(FF_APP_COMPOSE_ABC, ctx.state.compose.mode);

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

/* S27 sounds (docs/specs/S27-sounds.md) — proves the REAL sim wiring, not
 * just the shell's own hooks (test_shell.c already covers those in
 * isolation): `ff_ctl_loop_open` sets `shell_cfg.play_sound` to
 * `ctl_loop_play_sound_cb`, which is what makes an event land in the
 * ctl-observable log this test reads via `ff_ctl_loop_sound_log_count`/
 * `_at`. FLARE_START (`ff_shell_intent`, the same entry point a real
 * `ffsim --ctl` button tap reaches through `ff_shell_intent_sink`) needs
 * no pairing to sound FLARE_SENT — see ff_shell.c's FF_INTENT_FLARE_START
 * handler. */
static void S27_ctl_loop_play_sound_hook_logs_flare_sent(void)
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

    TEST_ASSERT_EQUAL_UINT32(0, ff_ctl_loop_sound_log_count(&ctx));

    ff_intent_t flare_start = {.kind = FF_INTENT_FLARE_START, .u = {0}};
    ff_shell_intent(&shell, &flare_start);

    TEST_ASSERT_EQUAL_UINT32(1, ff_ctl_loop_sound_log_count(&ctx));
    TEST_ASSERT_EQUAL(FF_SOUND_FLARE_SENT, ff_ctl_loop_sound_log_at(&ctx, 0));
    /* Out of range: the vocabulary's own "not a real event" sentinel. */
    TEST_ASSERT_EQUAL(FF_SOUND_COUNT, ff_ctl_loop_sound_log_at(&ctx, 1));

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

/* S26 slice e: the ctl `swipe` command still exists (nothing in
 * ctl_server.c/ctl_loop.c changed — see that pair's own files), but it
 * dispatches FF_INTENT_SWIPE, which ff_shell.c's own case now retires as
 * a documented no-op (the carousel is gone; BOOT/the launcher own
 * navigation). This regression-guards the retirement end to end through
 * the REAL ctl socket path — the same one a leftover UI/tooling caller
 * would actually hit — replacing the old `ctl_swipe_actually_changes_
 * the_face` positive proof (which is now, correctly, the wrong
 * property to assert). */
static void ctl_swipe_no_longer_changes_the_face(void)
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
    /* S26e amended 2026-09-01: the boot default is the launcher, not
     * Radar — this test's property (swipe moves nothing) holds either
     * way, so it is pinned against whichever face BOOT actually opens
     * on rather than assuming Radar specifically. */
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ctx.state.active_face);

    char resp[256];
    ctl_send(&h, "{\"cmd\":\"swipe\",\"dir\":\"left\"}", resp, sizeof(resp));
    ctl_settle(&ctx, &h);
    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_LAUNCHER, ctx.state.active_face, "ctl swipe left moved the face — SWIPE should be retired");

    ctl_send(&h, "{\"cmd\":\"swipe\",\"dir\":\"right\"}", resp, sizeof(resp));
    ctl_settle(&ctx, &h);
    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_LAUNCHER, ctx.state.active_face, "ctl swipe right moved the face — SWIPE should be retired");

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

/* issue #70's `hold` command (press-and-hold, driving a REAL
 * LONG_PRESSED through the ctl socket) still exists — nothing in
 * ctl_server.c/ctl_loop.c changed — but S26 slice e retired the one
 * thing it used to prove: `scr_nav.c`'s `nav_long_press_cb` is gone
 * along with the carousel (Settings is a launcher circle now,
 * scr_launcher.h). This regression-guards the retirement through the
 * REAL ctl socket path, both below and above LVGL's ~400ms
 * long_press_time — neither duration opens anything, because there is
 * no long-press handler left to open it.
 *
 * (206, 134) — amended AGAIN 2026-09-01 (S26e's compass-ring visual
 * refresh) from this test's own prior probe point, the exact center
 * (206, 206): the pre-amendment 2-over-3 grid deliberately kept that
 * exact point clear of every circle, but the compass ring puts the
 * RADAR HUB disc — a real 120px clickable — centered exactly there on
 * purpose (scr_launcher.c's own top comment), so a stationary hold at
 * dead-center now correctly clicks the hub (that's a real circle doing
 * its job, not a bug this test should paper over). (206, 134) sits in
 * the vertical gap between the hub's own top edge (y=146,
 * center 206 - 120/2) and the Inbox satellite's bottom edge (y=122,
 * center 78 + 88/2) — 24px tall, and, since both of those circles are
 * horizontally centered on x=206 same as this probe, no OTHER circle
 * (Lineup/Map/Settings, all off that x) can reach it either. Still one
 * point, still verified empty by construction rather than eyeballed —
 * same discipline the prior point's own comment used. */
static void ctl_hold_no_longer_opens_settings(void)
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
    /* S26e amended 2026-09-01: boots on the launcher, not Radar — see
     * ctl_swipe_no_longer_changes_the_face's own note just above. */
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ctx.state.active_face);

    char resp[256];

    /* A short hold (well under the old 400ms threshold) still does
     * nothing — unchanged. */
    ctl_hold(&h, 206.0, 134.0, 80, resp, sizeof(resp));
    ctl_settle(&ctx, &h);
    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_LAUNCHER, ctx.state.active_face, "an 80ms ctl hold moved the face");

    /* A long hold (the old FF_CTL_HOLD_DEFAULT_MS, 600ms) is the
     * retirement itself: it used to open Settings and must not any
     * more. */
    ctl_hold(&h, 206.0, 134.0, 600, resp, sizeof(resp));
    ctl_settle(&ctx, &h);
    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_LAUNCHER, ctx.state.active_face,
                              "a 600ms ctl hold opened Settings — long-press-anywhere should be retired");

    char state_resp[FF_CTL_MAX_RESP];
    ctl_send(&h, "{\"cmd\":\"state\"}", state_resp, sizeof(state_resp));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(state_resp, "\"face\":\"launcher\""), state_resp);

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

/* The live-demo report: inbound flares arrive repeatedly; the FIRST
 * takeover's DISMISS works, a SECOND one will not dismiss. Drives it the
 * only way that can catch a render-lifecycle / stale-object / stale-hit-
 * target bug the pure ff_shell_intent path (test_shell.c's
 * flare_second_takeover_dismisses) cannot: TWO flares in a row, each
 * DISMISSed with a REAL ctl tap whose coordinates are discovered from the
 * actually-built LVGL tree, through the same rebuild-on-dirty loop the
 * device runs. If the second takeover's DISMISS button is missing, stale,
 * or covered by a leftover object, ctl_tap_button either fails to find it
 * or the tap lands on nothing and takeover_active stays true. */
static void flare_second_takeover_dismisses_via_real_tap(void)
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

    /* --- FIRST flare (member A) -> takeover -> real DISMISS tap -------- */
    ctl_send(&h, "{\"cmd\":\"flare\",\"from\":55834,\"dur_s\":300}", resp, sizeof(resp)); /* 0xDA1A */
    ctl_settle(&ctx, &h);
    TEST_ASSERT_TRUE(ff_shell_flare(&shell)->takeover_active);

    lv_obj_t *dismiss1 = find_button_with_label(lv_screen_active(), "DISMISS");
    TEST_ASSERT_NOT_NULL_MESSAGE(dismiss1, "first takeover DISMISS button not found");
    ctl_tap_button(&ctx, &h, dismiss1);
    TEST_ASSERT_FALSE_MESSAGE(ff_shell_flare(&shell)->takeover_active, "first DISMISS did not clear the takeover");

    /* --- SECOND flare (a DIFFERENT member B) -> takeover -> DISMISS ---- */
    ctl_send(&h, "{\"cmd\":\"flare\",\"from\":52960,\"dur_s\":300}", resp, sizeof(resp)); /* 0xCEE0 */
    ctl_settle(&ctx, &h);
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_flare(&shell)->takeover_active, "second flare did not raise a takeover");

    lv_obj_t *dismiss2 = find_button_with_label(lv_screen_active(), "DISMISS");
    TEST_ASSERT_NOT_NULL_MESSAGE(dismiss2,
                                 "second takeover DISMISS button not found — screen did not rebuild the takeover");
    ctl_tap_button(&ctx, &h, dismiss2);
    TEST_ASSERT_FALSE_MESSAGE(ff_shell_flare(&shell)->takeover_active,
                              "SECOND DISMISS did not clear the takeover (the on-glass report)");

    /* --- THIRD flare, the SAME member A again (the demo repeats) ------- */
    ctl_send(&h, "{\"cmd\":\"flare\",\"from\":55834,\"dur_s\":300}", resp, sizeof(resp));
    ctl_settle(&ctx, &h);
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_flare(&shell)->takeover_active, "repeat flare from member A did not take over");

    lv_obj_t *dismiss3 = find_button_with_label(lv_screen_active(), "DISMISS");
    TEST_ASSERT_NOT_NULL_MESSAGE(dismiss3, "repeat-member takeover DISMISS button not found");
    ctl_tap_button(&ctx, &h, dismiss3);
    TEST_ASSERT_FALSE_MESSAGE(ff_shell_flare(&shell)->takeover_active, "repeat-member DISMISS did not clear");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* The takeover's attention pulse (S10 "flare to grab attention") actually
 * animates — guarded, not trusted. The mark is a full-puck container whose
 * opacity breathes from LV_OPA_COVER down to a floor and back; here we
 * build the takeover, advance the mock clock into the down-ramp, and assert
 * the mark's opacity has dropped below full. A no-op animation (or a future
 * refactor that drops lv_anim_start) leaves it pinned at COVER and fails.
 * The resting frame staying == golden is covered separately by the golden
 * suite (flare_takeover.png), which renders at frozen tick 0. */
static void flare_takeover_mark_pulse_animates(void)
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
    ctl_send(&h, "{\"cmd\":\"flare\",\"from\":55834,\"dur_s\":300}", resp, sizeof(resp));
    ctl_settle(&ctx, &h);
    TEST_ASSERT_TRUE(ff_shell_flare(&shell)->takeover_active);

    /* The takeover puck is the screen's child; the animated mark container
     * is that puck's first child (built before the headline/buttons). */
    lv_obj_t *puck = lv_obj_get_child(lv_screen_active(), 0);
    TEST_ASSERT_NOT_NULL_MESSAGE(puck, "takeover puck not found");
    lv_obj_t *mark = lv_obj_get_child(puck, 0);
    TEST_ASSERT_NOT_NULL_MESSAGE(mark, "takeover mark container not found");

    /* Advance ~400 ms — comfortably into the 750 ms down-ramp — pumping the
     * LVGL timers so the animation actually steps. */
    for (int i = 0; i < 8; i++) {
        ctl_clock_advance(&h, 50);
        lv_timer_handler();
    }
    lv_refr_now(ctx.disp);

    lv_opa_t const opa = lv_obj_get_style_opa(mark, 0);
    TEST_ASSERT_LESS_THAN_UINT_MESSAGE(LV_OPA_COVER, opa,
                                       "takeover mark opacity never dropped — the attention pulse is not animating");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S26_banner_tap_from_the_launcher_lands_on_the_thread);
    RUN_TEST(S26_flare_go_from_the_launcher_lands_on_radar);
    RUN_TEST(S16_AC10_draft_typed_flare_injected_takeover_clears_draft_survives);
    RUN_TEST(S27_ctl_loop_play_sound_hook_logs_flare_sent);
    RUN_TEST(flare_second_takeover_dismisses_via_real_tap);
    RUN_TEST(flare_takeover_mark_pulse_animates);
    RUN_TEST(S16_d_idle_ticks_never_rebuild_the_screen);
    RUN_TEST(ctl_swipe_no_longer_changes_the_face);
    RUN_TEST(ctl_hold_no_longer_opens_settings);
    RUN_TEST(ctl_wall_sets_bench_time_honestly);

    return UNITY_END();
}
