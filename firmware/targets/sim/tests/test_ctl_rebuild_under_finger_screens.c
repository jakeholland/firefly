/**
 * test_ctl_rebuild_under_finger_screens.c — fix/tap-lost-midpress-rebuild:
 * `test_ctl_rebuild_under_finger.c` proves the finger-down rebuild gate
 * for the LAUNCHER's own hub button; this file proves the SAME property
 * (a held finger's widget survives, object-identical and still PRESSED,
 * across a genuine mid-press dirty tick, with its CLICKED still
 * delivered on release) on the three faces the maintainer's on-glass
 * bench trace actually reported "most taps take a few tries" on:
 * Inbox (a conversation row), Compose (a T9 keypad key), and Settings
 * (a list row).
 *
 * Bench trace (docs/specs/S26-device-lifecycle.md's dated amendment for
 * this fix has the full excerpt): motionless taps on these three faces
 * — `dur` up to 429 ms, `move=0px`, a continuous PRESSED->RELEASED
 * indev sequence — produced RELEASED with no CLICKED roughly half the
 * time. `shell_render_key` (ff_shell.c) already carries a LONG history
 * of exactly this clobber class (the flare takeover, the launcher, the
 * power menu, Signals' popup/rally, every conversation's raw age) and
 * masks/coarsens every one of those known culprits — but carries NO
 * such mask for FF_APP_FACE_COMPOSE or the Settings list, so an
 * unrelated live fact changing ANYWHERE in `ff_app_state_t` (a fresh
 * inbound message is this file's own dirty producer, same choice as
 * `test_ctl_rebuild_under_finger.c` and its own reasoning for why) still
 * dirties the WHOLE key while either is showing. Each case below
 * presses a real target, injects that same genuine dirty producer mid-
 * hold, and asserts the SAME two properties
 * `test_ctl_rebuild_under_finger.c` established for the launcher:
 * object identity survives (`lv_obj_t *` unchanged) and PRESSED is not
 * lost, then that the deferred rebuild's CLICKED still lands on
 * release.
 *
 * Each test opens its own local ctl session (mirrors
 * `test_ctl_flare_sequence.c`'s per-test-function shape, not
 * `test_ctl_rebuild_under_finger.c`'s shared setUp/tearDown) since each
 * needs different pre-navigation (a paired conversation for Inbox, a
 * mode switch for Compose, a settings entry for Settings) before the
 * actual press-hold-dirty-release sequence under test.
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

/* An arbitrary, never-zero node id — same "any id is fine, this is a
 * synthetic bench sender" convention every other ctl-loop test in this
 * suite uses (test_ctl_flare_sequence.c, test_ctl_rebuild_under_finger.c). */
#define TEST_SENDER_NODE 0x51A1Eu

/* Same recursive lookup every other ctl-loop test file in this suite
 * carries (duplicated rather than shared — see test_ctl_flare_sequence.c's
 * own header comment on why). */
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

/* Exact-text label lookup (not wrapped in a button) — scr_inbox.c's row
 * name label sits as a sibling of the row's own tap-target button, not
 * a child of it, so find_button_with_label above cannot see it. Same
 * "duplicated, no shared header for one small helper" convention. */
static lv_obj_t *find_label_exact(lv_obj_t *root, char const *text)
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
        lv_obj_t *found = find_label_exact(child, text);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

/* An Inbox conversation row's real tap target is a transparent overlay
 * button created as the row's FIRST child (scr_inbox.c's
 * inbox_row_container) — the row's only unique on-screen text is its
 * name label, so the lookup goes by that, steps up to the row, and
 * takes child 0. Mirrors test_scr_intent.c's own find_row_hit_by_name. */
static lv_obj_t *find_row_hit_by_name(lv_obj_t *root, char const *name_text)
{
    lv_obj_t *name = find_label_exact(root, name_text);
    if (name == NULL) return NULL;
    lv_obj_t *row = lv_obj_get_parent(name);
    if (row == NULL) return NULL;
    lv_obj_t *hit = lv_obj_get_child(row, 0);
    if (hit == NULL || !lv_obj_check_type(hit, &lv_button_class)) return NULL;
    return hit;
}

/* A GENUINE dirty producer that touches no field any of the three
 * screens under test actually renders — same choice, and same
 * reasoning, as test_ctl_rebuild_under_finger.c's own
 * dirty_via_inbound_message: an inbound TEXT from a freshly-paired
 * sender, injected via ff_shell_events().on_text, the real event seam
 * (not a shortcut around the shell). A distinct body every call so a
 * re-send is never coalesced by anything downstream. */
static void dirty_via_inbound_message(ff_shell_t *shell, uint32_t sender, unsigned *counter)
{
    char body[32];
    (*counter)++;
    (void)snprintf(body, sizeof(body), "dirty %u", *counter);

    mc_events_t const ev = ff_shell_events(shell);
    TEST_ASSERT_NOT_NULL_MESSAGE(ev.on_text, "ff_shell_events().on_text unavailable — is the shell wired at all?");
    ev.on_text(ev.user, sender, MC_ADDR_BROADCAST, body, strlen(body));
}

/* Same release + double-settle choreography test_ctl_rebuild_under_finger.c's
 * own release_and_settle uses: release + one settle poll, a real
 * ff_ctl_loop_pump (so a CLICKED intent's shell-state change is mirrored
 * into ctx->state), then one more poll. */
static void release_and_settle(ff_ctl_loop_ctx_t *ctx)
{
    ff_ctl_loop_pointer_release(ctx);
    ff_ctl_loop_pointer_step(ctx);
    ff_ctl_loop_pump(ctx);
    ff_ctl_loop_pointer_step(ctx);
}

/* ---------------------------------------------------------------------
 * Case 1: an Inbox conversation row (Signals list), held while a fresh
 * message from a DIFFERENT sender arrives — a genuine unrelated dirty
 * producer while the finger sits on an unrelated row.
 * ------------------------------------------------------------------- */
static void S26_finger_down_on_an_inbox_row_survives_a_mid_press_dirty_tick(void)
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
    lv_refr_now(ctx.disp);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ctx.state.active_face);

    /* Two paired conversations: ROWAN is the row the finger holds; DANA
     * is the UNRELATED sender whose message is this test's dirty
     * producer (must not be the same conversation the row belongs to,
     * or the row's own preview text would legitimately need to
     * repaint — this test is about churn that renders no pixel on the
     * held row at all). */
    enum { ROWAN = 0x00A0A0u, DANA = 0x00DA1Au };
    mc_events_t const ev = ff_shell_events(&shell);
    TEST_ASSERT_TRUE(ff_shell_pair(&shell, ROWAN, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&shell, DANA, true));
    mc_nodeinfo_t n;
    memset(&n, 0, sizeof(n));
    n.node_num = ROWAN;
    n.has_short_name = true;
    strncpy(n.short_name, "ROWAN", sizeof(n.short_name) - 1);
    ev.on_node(ev.user, &n);
    memset(&n, 0, sizeof(n));
    n.node_num = DANA;
    n.has_short_name = true;
    strncpy(n.short_name, "DANA", sizeof(n.short_name) - 1);
    ev.on_node(ev.user, &n);

    /* Navigate to the Inbox list from the LAUNCHER (the boot default)
     * via a real synthetic tap on its INBOX hub icon — the same real
     * press-then-release choreography S26_banner_tap_from_the_launcher_
     * lands_on_the_thread (test_ctl_flare_sequence.c) uses for the
     * banner strip, not a shortcut around navigation. */
    lv_obj_t *inbox_hub = find_button_with_label(lv_screen_active(), "INBOX");
    TEST_ASSERT_NOT_NULL_MESSAGE(inbox_hub, "launcher INBOX hub button not found");
    {
        lv_area_t area;
        lv_obj_get_click_area(inbox_hub, &area);
        int32_t const cx = ((int32_t)area.x1 + (int32_t)area.x2) / 2;
        int32_t const cy = ((int32_t)area.y1 + (int32_t)area.y2) / 2;
        ff_ctl_loop_pointer_press(&ctx, cx, cy);
        release_and_settle(&ctx);
    }
    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_INBOX, ctx.state.active_face, "INBOX hub tap did not navigate to Inbox");
    TEST_ASSERT_EQUAL(FF_INBOX_SUB_INBOX, ctx.state.inbox.subview);

    lv_obj_t *row_before = find_row_hit_by_name(lv_screen_active(), "ROWAN");
    TEST_ASSERT_NOT_NULL_MESSAGE(row_before, "ROWAN's Inbox row not found — is the list actually rendering it?");

    lv_area_t area;
    lv_obj_get_click_area(row_before, &area);
    int32_t const cx = ((int32_t)area.x1 + (int32_t)area.x2) / 2;
    int32_t const cy = ((int32_t)area.y1 + (int32_t)area.y2) / 2;

    uint32_t const rebuilds_before_press = ctx.rebuild_count;

    ff_ctl_loop_pointer_press(&ctx, cx, cy);
    TEST_ASSERT_TRUE_MESSAGE(lv_obj_has_state(row_before, LV_STATE_PRESSED),
                              "the row never showed PRESSED on press — positive control failed");

    /* ---- mid-press dirty tick: a message from the OTHER paired sender. */
    unsigned msg_n = 0;
    dirty_via_inbound_message(&shell, DANA, &msg_n);
    ctx.mock_clock_ms += 40u; /* > the 33ms LVGL indev read period */
    ff_ctl_loop_pump(&ctx);

    TEST_ASSERT_TRUE_MESSAGE(ctx.rebuild_pending, "the mid-press dirty tick was not even latched as pending");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rebuilds_before_press, ctx.rebuild_count,
                                     "a rebuild happened while a finger was down on an Inbox row");

    lv_obj_t *row_mid_press = find_row_hit_by_name(lv_screen_active(), "ROWAN");
    TEST_ASSERT_EQUAL_PTR_MESSAGE(row_before, row_mid_press,
                                  "the row's object identity changed while held — the tree WAS cleaned/rebuilt");
    TEST_ASSERT_TRUE_MESSAGE(lv_obj_has_state(row_before, LV_STATE_PRESSED),
                              "PRESSED style was lost mid-hold on the Inbox row");

    release_and_settle(&ctx);

    TEST_ASSERT_EQUAL_MESSAGE(FF_INBOX_SUB_THREAD, ctx.state.inbox.subview,
                              "the row's CLICKED never delivered on release — the tap that opens the thread was lost");
    TEST_ASSERT_EQUAL_UINT32(ROWAN, ctx.state.inbox.thread_node);

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* ---------------------------------------------------------------------
 * Case 2: a Compose T9 keypad key, held while a fresh message arrives —
 * the composer's render key carries no mask/coarsening at all (unlike
 * the launcher/flare-takeover/power-menu branches shell_render_key
 * documents at length), so an unrelated inbound message dirties it on
 * every character typed.
 * ------------------------------------------------------------------- */
static void S26_finger_down_on_a_compose_key_survives_a_mid_press_dirty_tick(void)
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
    lv_refr_now(ctx.disp);

    enum { DANA = 0x00DA1Au };
    mc_events_t const ev = ff_shell_events(&shell);
    TEST_ASSERT_TRUE(ff_shell_pair(&shell, DANA, true));
    mc_nodeinfo_t n;
    memset(&n, 0, sizeof(n));
    n.node_num = DANA;
    n.has_short_name = true;
    strncpy(n.short_name, "DANA", sizeof(n.short_name) - 1);
    ev.on_node(ev.user, &n);

    /* Reach Compose through the real intent seam (test setup — same
     * convention test_ctl_flare_sequence.c's AC10 test uses), then flip
     * to the ABC keypad page (PRED's cold-open candidate strip has no
     * fixed key layout to press reliably). */
    ff_intent_t open_compose = {.kind = FF_INTENT_OPEN_COMPOSE, .u = {.node_id = 0u}};
    ff_shell_intent(&shell, &open_compose);
    ff_intent_t to_abc = {.kind = FF_INTENT_T9_MODE, .u = {0}};
    ff_shell_intent(&shell, &to_abc);
    ff_ctl_loop_pump(&ctx);
    lv_refr_now(ctx.disp);
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, ctx.state.active_face);
    TEST_ASSERT_EQUAL(FF_APP_COMPOSE_ABC, ctx.state.compose.mode);

    lv_obj_t *def_key = find_button_with_label(lv_screen_active(), "DEF");
    TEST_ASSERT_NOT_NULL_MESSAGE(def_key, "compose keypad's DEF key not found");

    lv_area_t area;
    lv_obj_get_click_area(def_key, &area);
    int32_t const cx = ((int32_t)area.x1 + (int32_t)area.x2) / 2;
    int32_t const cy = ((int32_t)area.y1 + (int32_t)area.y2) / 2;

    uint32_t const rebuilds_before_press = ctx.rebuild_count;

    ff_ctl_loop_pointer_press(&ctx, cx, cy);
    TEST_ASSERT_TRUE_MESSAGE(lv_obj_has_state(def_key, LV_STATE_PRESSED),
                              "the DEF key never showed PRESSED on press — positive control failed");

    unsigned msg_n = 0;
    dirty_via_inbound_message(&shell, DANA, &msg_n);
    ctx.mock_clock_ms += 40u;
    ff_ctl_loop_pump(&ctx);

    TEST_ASSERT_TRUE_MESSAGE(ctx.rebuild_pending, "the mid-press dirty tick was not even latched as pending");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rebuilds_before_press, ctx.rebuild_count,
                                     "a rebuild happened while a finger was down on a compose key");

    lv_obj_t *key_mid_press = find_button_with_label(lv_screen_active(), "DEF");
    TEST_ASSERT_EQUAL_PTR_MESSAGE(def_key, key_mid_press,
                                  "the DEF key's object identity changed while held — the tree WAS cleaned/rebuilt");
    TEST_ASSERT_TRUE_MESSAGE(lv_obj_has_state(def_key, LV_STATE_PRESSED),
                              "PRESSED style was lost mid-hold on the compose key");

    release_and_settle(&ctx);

    TEST_ASSERT_EQUAL_STRING_MESSAGE("d", ctx.state.compose.text,
                                     "the DEF key's CLICKED never delivered on release — no character was typed");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* ---------------------------------------------------------------------
 * Case 3: a Settings list row, held while a fresh message arrives — the
 * Settings face gets no render-key mask either (only its CREW/
 * DIAGNOSTICS/NAME_EDIT/COMPASS_CAL sub-pages carry any age-coarsening,
 * and even that is for THEIR OWN raw ages, not for churn originating
 * elsewhere in the state, e.g. an inbound message).
 * ------------------------------------------------------------------- */
static void S26_finger_down_on_a_settings_row_survives_a_mid_press_dirty_tick(void)
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
    lv_refr_now(ctx.disp);

    enum { DANA = 0x00DA1Au };
    mc_events_t const ev = ff_shell_events(&shell);
    TEST_ASSERT_TRUE(ff_shell_pair(&shell, DANA, true));
    mc_nodeinfo_t n;
    memset(&n, 0, sizeof(n));
    n.node_num = DANA;
    n.has_short_name = true;
    strncpy(n.short_name, "DANA", sizeof(n.short_name) - 1);
    ev.on_node(ev.user, &n);

    ff_intent_t open_settings = {.kind = FF_INTENT_OPEN_SETTINGS, .u = {0}};
    ff_shell_intent(&shell, &open_settings);
    ff_ctl_loop_pump(&ctx);
    lv_refr_now(ctx.disp);
    TEST_ASSERT_EQUAL(FF_APP_FACE_SETTINGS, ctx.state.active_face);
    TEST_ASSERT_EQUAL(FF_SETTINGS_SUB_LIST, ctx.state.settings.subview);

    /* The CLOCK row's "24H" pill (settings_build_toggle_row, scr_settings.c
     * — DISPLAY's second row, close enough to the top of the list to sit
     * in the viewport with no scroll needed) — a genuine
     * `ff_scr_pill_create` button, unlike the COMPASS row's caption half
     * (a plain clickable `lv_obj_t`, find_button_with_label's
     * lv_button_class check would never match it) or CALIBRATE TOUCH/
     * DIAGNOSTICS (real buttons, but their own intents are device-only
     * no-ops in the sim — nothing observable to assert a delivered
     * CLICKED against). Tapping it flips `settings.clock_24h`, a plain
     * bool this test can assert on release. */
    bool const clock_24h_before = ctx.state.settings.clock_24h;
    lv_obj_t *clock_24h_btn = find_button_with_label(lv_screen_active(), "24H");
    TEST_ASSERT_NOT_NULL_MESSAGE(clock_24h_btn, "Settings CLOCK row's 24H pill not found");

    lv_area_t area;
    lv_obj_get_click_area(clock_24h_btn, &area);
    int32_t const cx = ((int32_t)area.x1 + (int32_t)area.x2) / 2;
    int32_t const cy = ((int32_t)area.y1 + (int32_t)area.y2) / 2;

    uint32_t const rebuilds_before_press = ctx.rebuild_count;

    ff_ctl_loop_pointer_press(&ctx, cx, cy);
    TEST_ASSERT_TRUE_MESSAGE(lv_obj_has_state(clock_24h_btn, LV_STATE_PRESSED),
                              "the Settings row never showed PRESSED on press — positive control failed");

    unsigned msg_n = 0;
    dirty_via_inbound_message(&shell, DANA, &msg_n);
    ctx.mock_clock_ms += 40u;
    ff_ctl_loop_pump(&ctx);

    TEST_ASSERT_TRUE_MESSAGE(ctx.rebuild_pending, "the mid-press dirty tick was not even latched as pending");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rebuilds_before_press, ctx.rebuild_count,
                                     "a rebuild happened while a finger was down on a Settings row");

    lv_obj_t *row_mid_press = find_button_with_label(lv_screen_active(), "24H");
    TEST_ASSERT_EQUAL_PTR_MESSAGE(clock_24h_btn, row_mid_press,
                                  "the Settings row's object identity changed while held — the tree WAS cleaned/rebuilt");
    TEST_ASSERT_TRUE_MESSAGE(lv_obj_has_state(clock_24h_btn, LV_STATE_PRESSED),
                              "PRESSED style was lost mid-hold on the Settings row");

    release_and_settle(&ctx);

    TEST_ASSERT_TRUE_MESSAGE(ctx.state.settings.clock_24h != clock_24h_before,
                             "the Settings row's CLICKED never delivered on release — CLOCK never flipped");

    ff_ctl_loop_close(&ctx);
    ff_shell_close(&shell);
    lv_deinit();
}

/* Each test function above opens and closes its own local ctl session
 * (this file's top comment explains why: per-test-function shape, not
 * shared setUp/tearDown) — Unity still requires both symbols to exist. */
void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S26_finger_down_on_an_inbox_row_survives_a_mid_press_dirty_tick);
    RUN_TEST(S26_finger_down_on_a_compose_key_survives_a_mid_press_dirty_tick);
    RUN_TEST(S26_finger_down_on_a_settings_row_survives_a_mid_press_dirty_tick);
    return UNITY_END();
}
