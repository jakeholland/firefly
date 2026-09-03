/**
 * test_ctl_rebuild_under_finger.c — debt/sim-window-lifecycle:
 * `ctl_loop.c`'s rebuild gate was `rebuild_pending && has_screen &&
 * !screen_blank` — missing the finger-down term `app_main.c`'s own gate
 * has had since the original rebuild-mid-tap fix
 * (`!ff_display_touch_is_down()`, docs/specs/S26-device-lifecycle.md's
 * amendment); the comment above it even said the OFF-window deferral
 * only mirrored that fix "in spirit". This file proves the REAL parity
 * now in place (via `ff_sim_lifecycle_pump`, sim_lifecycle.h): a dirty
 * tick that lands while a ctl-injected touch is down must NOT rebuild
 * the screen (the button under the finger survives as the SAME object,
 * with its CLICKED still delivered normally on release — the gesture
 * began ACTIVE, so S26's wake-only gate does not apply to it); a dirty
 * tick with no finger down rebuilds immediately (the negative control
 * that isolates the finger-down term specifically, proving the property
 * is a real gate and not "nothing was ever dirty").
 *
 * THE PROXY, stated up front (AGENTS.md standing rule item 6): "no
 * rebuild while held" is trivially true if nothing was ever dirty
 * during the hold — every press below injects a GENUINE dirty event: an
 * inbound TEXT message from a freshly-PAIRED sender (`ff_shell_pair` +
 * `ff_shell_events().on_text`, the same injection seam
 * `ctl_loop_flare`'s own FLARE-injection handler uses for `on_private`).
 * This is deliberately NOT a settings toggle: the launcher's render key
 * (`shell_render_key`'s `FF_APP_FACE_LAUNCHER` branch, ff_shell.c) masks
 * EVERYTHING to zero except `active_face` and the inbox's total unread
 * count — an unrelated live value (e.g. the radar arrow, or a settings
 * flag) ticking underneath must NOT dirty the key and rebuild the
 * launcher's tree out from under a finger, so a settings-only dirty
 * producer would silently never dirty at all while the launcher is
 * showing (caught empirically while writing this test: a screen_flip
 * toggle here produced zero dirty ticks). A new unread message is
 * exactly the one live fact the launcher's own badge renders, so it is
 * both a GENUINE dirty producer here and the spec's own "a banner" example
 * in spirit (an unpaired sender's text never reaches the badge at all —
 * S22's stranger rule — so pairing first is required, not incidental).
 *
 * The property that actually matters is OBJECT IDENTITY, not just a
 * rebuild counter: this file checks the launcher's RADAR hub button is
 * the SAME `lv_obj_t *` (and still shows `LV_STATE_PRESSED`) immediately
 * after the mid-press dirty tick — a version that miscounted
 * `rebuild_count` but still tore the tree down would fail this even if
 * the counter looked right.
 *
 * Drives a REAL `ff_ctl_loop_*` session (same object main.c's
 * `--headless --ctl PORT` uses) through `ff_ctl_loop_pointer_press` /
 * `_step` / `_release` (ctl_loop.h) — the press/release primitives this
 * PR split `ctl_loop_pointer_gesture` into specifically so a test can
 * pump OTHER things (a real `ff_ctl_loop_pump`, a dirtying inbound
 * message) BETWEEN a press and a release, which the old atomic
 * tap/swipe/hold commands have no seam for.
 *
 * lv_init/lv_deinit live in setUp/tearDown (not in the test bodies) so
 * cleanup always runs even if a TEST_ASSERT_* fails mid-test — Unity
 * longjmps out of the test body on a failed assertion but still calls
 * tearDown() afterward, so anything the CLEANUP needs must not sit past
 * an assertion inside the body itself. `s_shell`/`s_pack`/`s_ctx` are
 * safe to reuse across the setUp/tearDown cycle each RUN_TEST gets:
 * `ff_ctl_loop_open` memsets `*ctx` itself, and `ff_shell_init` memsets
 * its own internal state at the top of every call (ff_shell.c) — a
 * fresh session every time regardless of what the previous test left
 * behind.
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
 * synthetic bench sender" convention test_ctl_flare_sequence.c's own
 * flare-injection tests use. */
#define TEST_SENDER_NODE 0x424242u

static ff_shell_t s_shell;
static fp_pack_t s_pack;
static ff_ctl_loop_ctx_t s_ctx;

void setUp(void)
{
    ff_shell_cfg_t shell_cfg;
    memset(&shell_cfg, 0, sizeof(shell_cfg));

    ff_ctl_loop_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mock_clock = true;

    TEST_ASSERT_EQUAL_INT(0, ff_ctl_loop_open(&s_ctx, &s_shell, &s_pack, &shell_cfg, &cfg));

    /* Installs ctl_loop_pointer_read_cb on the pointer indev — without
     * this, lv_timer_handler() reads nothing and neither the gate nor a
     * real click ever fires (every ctl-driven test in this suite relies
     * on this same call). */
    bool quit_flag = false;
    (void)ff_ctl_loop_handlers(&s_ctx, &quit_flag);

    ff_ctl_loop_pump(&s_ctx); /* settle the always-dirty first tick */
    lv_refr_now(s_ctx.disp);  /* force layout so lv_obj_get_click_area returns real coords, not (0,0) */

    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, s_ctx.state.active_face); /* S26e boot default */
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&s_ctx.idle));

    /* A PAIRED sender, so the message injected below actually reaches
     * the feed (and the launcher's unread badge) instead of being
     * dropped by the S22 stranger rule. */
    (void)ff_shell_pair(&s_shell, TEST_SENDER_NODE, true);
}

void tearDown(void)
{
    ff_ctl_loop_close(&s_ctx);
    ff_shell_close(&s_shell);
    lv_deinit();
}

/* Same recursive lookup test_ctl_flare_sequence.c and
 * test_wakeonly_touch.c each already carry (duplicated rather than
 * shared — see those files' own comments: no shared-header dependency
 * for one small helper). */
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

/* A GENUINE dirty producer that does not navigate away from the
 * launcher — see this file's top comment for why it has to be an
 * inbound message (the launcher's masked render key) rather than a
 * settings toggle. Each call sends a distinct body so a re-send is
 * never coalesced by anything downstream that dedupes identical text. */
static void dirty_via_inbound_message(ff_shell_t *shell, unsigned *counter)
{
    char body[32];
    (*counter)++;
    (void)snprintf(body, sizeof(body), "hi %u", *counter);

    mc_events_t const ev = ff_shell_events(shell);
    TEST_ASSERT_NOT_NULL_MESSAGE(ev.on_text, "ff_shell_events().on_text unavailable — is the shell wired at all?");
    ev.on_text(ev.user, TEST_SENDER_NODE, MC_ADDR_BROADCAST, body, strlen(body));
}

/* Same release + double-settle choreography test_wakeonly_touch.c's own
 * wg_release_and_settle uses, rebuilt on top of the newly-exposed
 * ff_ctl_loop_pointer_release/_step (ctl_loop.h) instead of touching
 * ctx fields directly: release + one settle poll, ANOTHER settle poll,
 * a real ff_ctl_loop_pump (so a CLICKED intent's shell-state change is
 * mirrored into ctx->state), then one more poll. */
static void release_and_settle(void)
{
    ff_ctl_loop_pointer_release(&s_ctx);
    ff_ctl_loop_pointer_step(&s_ctx);
    ff_ctl_loop_pump(&s_ctx);
    ff_ctl_loop_pointer_step(&s_ctx);
}

/* ---- Positive case: a finger held down defers a real dirty tick, and
 * the deferred rebuild (plus the click) lands on release. ---- */
static void S26_finger_down_defers_ctl_rebuild_then_delivers_click_on_release(void)
{
    lv_obj_t *hub_before = find_button_with_label(lv_screen_active(), "RADAR");
    TEST_ASSERT_NOT_NULL_MESSAGE(hub_before, "launcher RADAR hub button not found — is the launcher actually built?");

    lv_area_t area;
    lv_obj_get_click_area(hub_before, &area);
    int32_t const cx = ((int32_t)area.x1 + (int32_t)area.x2) / 2;
    int32_t const cy = ((int32_t)area.y1 + (int32_t)area.y2) / 2;

    uint32_t const rebuilds_before_press = s_ctx.rebuild_count;

    /* ---- Press and hold (begins ACTIVE — delivered normally, no S26
     * wake-only gating involved). ---- */
    ff_ctl_loop_pointer_press(&s_ctx, cx, cy);
    TEST_ASSERT_TRUE_MESSAGE(lv_obj_has_state(hub_before, LV_STATE_PRESSED),
                              "the hub never showed PRESSED on press — positive control failed");

    /* ---- Inject a GENUINE dirty event while the finger is still down,
     * then pump a real tick (mirrors app_main.c's per-frame tick — a
     * dirty tick lands mid-gesture exactly like a live tick would). ---- */
    unsigned msg_n = 0;
    dirty_via_inbound_message(&s_shell, &msg_n);
    s_ctx.mock_clock_ms += 40u; /* > the 33ms LVGL indev read period */
    ff_ctl_loop_pump(&s_ctx);

    TEST_ASSERT_TRUE_MESSAGE(s_ctx.rebuild_pending, "the mid-press dirty tick was not even latched as pending");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        rebuilds_before_press, s_ctx.rebuild_count,
        "a rebuild happened while a finger was down — the finger-down term is missing from the gate");

    lv_obj_t *hub_mid_press = find_button_with_label(lv_screen_active(), "RADAR");
    TEST_ASSERT_EQUAL_PTR_MESSAGE(hub_before, hub_mid_press,
                                   "the button object's identity changed while held — the tree WAS cleaned/rebuilt");
    TEST_ASSERT_TRUE_MESSAGE(lv_obj_has_state(hub_before, LV_STATE_PRESSED),
                              "PRESSED style was lost mid-hold — something tore the button down under the finger");

    /* ---- Release: the deferred rebuild drains, and the click — which
     * began ACTIVE — still fires normally (S26 rules: only a press that
     * BEGINS non-ACTIVE is ever swallowed; this one began ACTIVE). ---- */
    release_and_settle();

    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(
        rebuilds_before_press, s_ctx.rebuild_count, "the dirty view accumulated while held was never rebuilt on release");
    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_RADAR, s_ctx.state.active_face,
                               "the click never delivered on release — FF_INTENT_LAUNCHER_SELECT did not fire");
}

/* ---- Negative control: the SAME kind of dirty event, with no finger
 * down anywhere, rebuilds immediately — isolates the finger-down term
 * from "nothing was dirty" (this file's own top-comment proxy note). ---- */
static void S26_no_finger_down_ctl_rebuild_is_immediate(void)
{
    lv_obj_t *hub_before = find_button_with_label(lv_screen_active(), "RADAR");
    TEST_ASSERT_NOT_NULL(hub_before);

    uint32_t const rebuilds_before = s_ctx.rebuild_count;

    /* No press anywhere — s_ctx.pointer_state is LV_INDEV_STATE_RELEASED
     * (the default setUp leaves it in after settling the first tick). */
    unsigned msg_n = 0;
    dirty_via_inbound_message(&s_shell, &msg_n);
    s_ctx.mock_clock_ms += 40u;
    ff_ctl_loop_pump(&s_ctx);

    TEST_ASSERT_FALSE_MESSAGE(s_ctx.rebuild_pending, "a dirty tick with no finger down should drain immediately");
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(rebuilds_before, s_ctx.rebuild_count,
                                             "no finger was down, but the dirty tick was still deferred — negative "
                                             "control failed (is anything actually dirtying here?)");

    /* Deliberately NOT asserting the button object's pointer differs
     * here: LVGL's allocator can (and in practice does) hand the freed
     * button's exact address back to the very next allocation of the
     * same size, so object-pointer inequality is not a reliable signal
     * for "a rebuild happened" — only for its converse (the positive
     * case above: pointer EQUALITY reliably proves NO clean/rebuild
     * touched the tree, since a live object's address cannot change
     * without one). `rebuild_count`/`rebuild_pending` above are what
     * actually prove this control fired an immediate rebuild. */
    lv_obj_t *hub_after = find_button_with_label(lv_screen_active(), "RADAR");
    TEST_ASSERT_NOT_NULL_MESSAGE(hub_after, "the RADAR hub button is gone after the immediate rebuild");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S26_finger_down_defers_ctl_rebuild_then_delivers_click_on_release);
    RUN_TEST(S26_no_finger_down_ctl_rebuild_is_immediate);
    return UNITY_END();
}
