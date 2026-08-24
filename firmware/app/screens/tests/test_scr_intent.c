/**
 * test_scr_intent.c — headless LVGL INTERACTION tests for every wired
 * emit site.
 *
 * S16 slice c1 wired the three navigation-only stubs: nav long-press
 * (-> OPEN_SETTINGS), Compose back "<" (-> BACK), Signals "+" (->
 * OPEN_COMPOSE). Slice c2 wires the core-mutating ones: Compose SEND (->
 * SEND_TEXT), the Signals rally-row tap (-> SELECT_RALLY) and OMW/5 MIN/
 * PULSE chips (-> CANNED_REPLY), the Radar-face CLOSE-mode FLARE button
 * (-> FLARE_START), and the S10 takeover/sender-overlay buttons GO/
 * DISMISS/CANCEL (-> TAKEOVER_GO/TAKEOVER_DISMISS/FLARE_END) — the last
 * three moved here from test_scr_flare.c once they stopped mutating a
 * live `ff_flare_t` directly (S16's `[api]` change dropping `ff_flare_t
 * *rt` from `ff_scr_flare_build_takeover`/`ff_scr_flare_build_sender_overlay`)
 * and started going through this same seam.
 *
 * Same pattern — and the same reason — as test_scr_flare.c originally
 * established (PR #20 review, HIGH finding): goldens are single-frame
 * pixel diffs and never fire an event, so nothing else in the suite can
 * prove that clicking "+" emits OPEN_COMPOSE rather than BACK, or
 * anything at all. Build the real screen, synthesize the event with
 * `lv_obj_send_event`, and assert on what came out of the seam — here, a
 * spy sink bound via `ff_intent_emit_bind`, since the seam's whole
 * contract is that screens know nothing past `ff_intent_emit()`.
 *
 * Each click must produce EXACTLY ONE intent (count asserted, not just
 * kind): a control double-registered on two callbacks would pass a
 * kind-only check while double-dispatching every tap.
 *
 * Mutation-check (hand-verified before pushing, per
 * docs/review/code-review.md item 6): swapping compose_back_cb's and
 * signals_open_compose_cb's emitted kinds fails both of their tests on
 * the kind assertion; removing an `ff_intent_emit` call entirely fails
 * on count == 0. Swapping flare_go_cb's/flare_dismiss_takeover_cb's
 * emitted kinds (the exact PR #20 regression class, now one layer up)
 * fails both GO/DISMISS tests on the kind assertion the same way.
 */
#include <string.h>

#include "unity.h"

#include "ff_app_state.h"
#include "ff_intent.h"
#include "ff_radar.h"
#include "scr_compose.h"
#include "scr_flare.h"
#include "scr_nav.h"
#include "scr_radar.h"
#include "scr_signals.h"

/* Frozen tick — same convention as test_scr_flare.c: nothing here
 * renders a frame. */
static uint32_t test_tick_cb(void)
{
    return 0;
}

/* ------------------------------------------------------------------- */
/* spy sink                                                             */
/* ------------------------------------------------------------------- */

typedef struct {
    int count;
    ff_intent_t last; /* copied — the sink contract (ff_intent.h) */
} spy_sink_t;

static spy_sink_t s_spy;

static void spy_sink_cb(void *user, ff_intent_t const *in)
{
    spy_sink_t *s = (spy_sink_t *)user;
    s->count++;
    s->last = *in;
}

void setUp(void)
{
    lv_init();
    lv_tick_set_cb(test_tick_cb);
    /* No buffers/flush callback: widget building + lv_obj_send_event
     * never touch the flush path (see test_scr_flare.c's setUp note). */
    lv_display_t *disp = lv_display_create(456, 456);
    lv_display_set_default(disp);

    memset(&s_spy, 0, sizeof(s_spy));
    ff_intent_emit_bind(spy_sink_cb, &s_spy);
}

void tearDown(void)
{
    /* The sink is process-global (ff_intent.h); never leak a binding to
     * a test that expects the unbound state. */
    ff_intent_emit_bind(NULL, NULL);
    lv_deinit();
}

/* find_button_with_label — same reusable lookup as test_scr_flare.c
 * (kept file-local there by design; duplicated rather than exported so
 * neither test file grows a shared-header dependency for one helper). */
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

static void click(lv_obj_t *obj)
{
    TEST_ASSERT_NOT_NULL(obj);
    lv_result_t r = lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL(LV_RESULT_OK, r);
}

/* =================================================================== */
/* nav long-press CALLBACK -> OPEN_SETTINGS                             */
/* =================================================================== */

/**
 * WHAT THIS PROVES — AND WHAT IT DELIBERATELY DOES NOT (PR #54 review,
 * HIGH finding, kept honest here rather than papered over).
 *
 * `lv_obj_send_event` delivers LV_EVENT_LONG_PRESSED DIRECTLY to the
 * puck, so this test pins the CALLBACK: when the hook fires, it emits
 * OPEN_SETTINGS, exactly once. It does NOT prove a physical long-press
 * reaches that callback — and today one does not: the full-size
 * tileview sits over the puck, LVGL objects are CLICKABLE by default,
 * and nothing sets LV_OBJ_FLAG_EVENT_BUBBLE, so `lv_indev_search_obj`
 * resolves every on-puck press to the tileview/tiles and the puck
 * never sees the gesture (measured by the reviewer at three probe
 * points: zero intents emitted through the indev path).
 *
 * Making the gesture reach the seam is S16 slice c3's, deliberately:
 * c3 owns the tileview's input handling — the same work that disables
 * its native swipe scrolling — and the two interact (whatever routes
 * the press must also decide when it is a swipe). The indev-driven
 * press test belongs with that fix; writing it now would pin an input
 * topology c3 is about to replace. See scr_nav.c's TODO(S16 slice c3)
 * at the hook site and S06's Amendments entry, both corrected to say
 * exactly this.
 */
static void S16_c1_nav_long_press_callback_emits_open_settings(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    state.active_face = FF_APP_FACE_RADAR;

    ff_scr_nav_build(&state);

    /* The long-press hook lives on the puck — scr_nav.c's first child of
     * the active screen (the same object it marks LV_OBJ_FLAG_CLICKABLE
     * for exactly this gesture). If the build order ever changes this
     * lookup fails loudly here rather than silently testing nothing. */
    lv_obj_t *puck = lv_obj_get_child(lv_screen_active(), 0);
    TEST_ASSERT_NOT_NULL(puck);
    TEST_ASSERT_TRUE(lv_obj_has_flag(puck, LV_OBJ_FLAG_CLICKABLE));

    lv_result_t r = lv_obj_send_event(puck, LV_EVENT_LONG_PRESSED, NULL);
    TEST_ASSERT_EQUAL(LV_RESULT_OK, r);

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_OPEN_SETTINGS, s_spy.last.kind);
}

/* =================================================================== */
/* Compose back "<" -> BACK                                             */
/* =================================================================== */

static void S16_c1_compose_back_emits_back(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose)); /* empty broadcast draft, ABC mode */

    ff_scr_compose_build(&compose);

    click(find_button_with_label(lv_screen_active(), "<"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_BACK, s_spy.last.kind);
}

/* =================================================================== */
/* Compose SEND -> SEND_TEXT (S16 slice c2)                             */
/* =================================================================== */

static void S16_c2_compose_send_emits_send_text(void)
{
    /* SEND_TEXT carries no payload (ff_intent.h: "the draft is
     * shell-owned T9 state") — asserted by kind and count only, same
     * shape as every other no-payload intent this file pins. The shell's
     * OWN handling of FF_INTENT_SEND_TEXT stays a no-op until slice c3
     * moves the draft in (test_intent.c's routing-rule-4 coverage is
     * where that eventually gets pinned); this test's job is only that
     * the button reaches the seam at all. */
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose));

    ff_scr_compose_build(&compose);

    click(find_button_with_label(lv_screen_active(), "SEND"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SEND_TEXT, s_spy.last.kind);
}

/* =================================================================== */
/* Signals "+" -> OPEN_COMPOSE, no explicit destination                 */
/* =================================================================== */

static void S16_c1_signals_plus_emits_open_compose_with_no_destination(void)
{
    ff_app_signals_t signals;
    memset(&signals, 0, sizeof(signals)); /* empty feed — the "+" is header chrome, always present */

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_signals_build(parent, &signals);

    click(find_button_with_label(parent, "+"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_OPEN_COMPOSE, s_spy.last.kind);
    /* node_id 0 = no explicit destination: the shell resolves S08's
     * "TO = selected crew member" rule — a pure-render screen cannot
     * know the selection, and must not guess one (scr_signals.c's
     * signals_open_compose_cb comment). */
    TEST_ASSERT_EQUAL_UINT32(0u, s_spy.last.u.node_id);
}

/* =================================================================== */
/* Signals rally row tap -> SELECT_RALLY (S16 slice c2)                 */
/* =================================================================== */

/* Rally rows are plain lv_obj_t containers, not lv_button_t (unlike every
 * other click site this file tests), so this file's own
 * find_button_with_label can't locate one. Rows have no unique on-screen
 * text of their own — `it->from_name` is the only per-row label — so the
 * lookup goes by that name label and steps one level up to the row it
 * belongs to. */
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

static void S16_c2_signals_rally_tap_emits_select_rally_with_its_index(void)
{
    ff_app_signals_t signals;
    memset(&signals, 0, sizeof(signals));
    signals.n_items = 2;
    signals.items[0].kind = FF_APP_FEED_TEXT; /* not tappable — see the negative half below */
    strncpy(signals.items[0].from_name, "DANA", sizeof(signals.items[0].from_name) - 1);
    signals.items[1].kind = FF_APP_FEED_RALLY; /* index 1 — the one this test taps */
    strncpy(signals.items[1].from_name, "KEV", sizeof(signals.items[1].from_name) - 1);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_signals_build(parent, &signals);

    lv_obj_t *name_label = find_label_exact(parent, "KEV");
    TEST_ASSERT_NOT_NULL(name_label);
    lv_obj_t *row = lv_obj_get_parent(name_label); /* the row: name/text/age/icon are all direct row children */
    TEST_ASSERT_NOT_NULL(row);
    TEST_ASSERT_TRUE(lv_obj_has_flag(row, LV_OBJ_FLAG_CLICKABLE));

    click(row);

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SELECT_RALLY, s_spy.last.kind);
    /* rally_idx is this row's own index into the ff_app_signals_t slice
     * this screen was handed, not some crew/core identity — the row at
     * items[1] must report 1. */
    TEST_ASSERT_EQUAL_UINT8(1, s_spy.last.u.rally_idx);

    /* Negative half: a non-RALLY row (DANA, items[0]) is not clickable at
     * all — S08 spec: only rally rows are tappable — so it can never
     * reach this seam by construction, not merely by this test not
     * trying it. */
    lv_obj_t *other_label = find_label_exact(parent, "DANA");
    TEST_ASSERT_NOT_NULL(other_label);
    lv_obj_t *other_row = lv_obj_get_parent(other_label);
    TEST_ASSERT_NOT_NULL(other_row);
    TEST_ASSERT_FALSE(lv_obj_has_flag(other_row, LV_OBJ_FLAG_CLICKABLE));
}

/* =================================================================== */
/* Signals OMW / 5 MIN / PULSE -> CANNED_REPLY (S16 slice c2)           */
/* =================================================================== */

static void S16_c2_signals_canned_reply_chips_emit_canned_reply(void)
{
    /* Reply-context resolution (which sender OMW/5 MIN/PULSE address) is
     * the SHELL's job, per ff_wiring_send_canned_reply's documented
     * contract (AC7, pinned at the shell level in test_shell.c) — this
     * screen only reports which chip was pressed, so an empty feed is
     * fine here: the chips render unconditionally as header chrome. */
    ff_app_signals_t signals;
    memset(&signals, 0, sizeof(signals));

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_signals_build(parent, &signals);

    click(find_button_with_label(parent, "OMW"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_CANNED_REPLY, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_WIRING_REPLY_OMW, s_spy.last.u.reply);

    click(find_button_with_label(parent, "5 MIN"));
    TEST_ASSERT_EQUAL_INT(2, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_CANNED_REPLY, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_WIRING_REPLY_5MIN, s_spy.last.u.reply);

    click(find_button_with_label(parent, "PULSE"));
    TEST_ASSERT_EQUAL_INT(3, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_CANNED_REPLY, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_WIRING_REPLY_PULSE, s_spy.last.u.reply);
}

/* =================================================================== */
/* Radar CLOSE-mode FLARE -> FLARE_START (S16 slice c2)                 */
/* =================================================================== */

static void S16_c2_radar_flare_button_emits_flare_start(void)
{
    ff_radar_view_t r;
    memset(&r, 0, sizeof(r));
    r.mode = RADAR_CLOSE;
    strncpy(r.name, "DANA", sizeof(r.name) - 1);
    strncpy(r.dist_str, "15 m", sizeof(r.dist_str) - 1);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_radar_build(parent, &r);

    click(find_button_with_label(parent, "FLARE"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_FLARE_START, s_spy.last.kind);
}

/* =================================================================== */
/* Flare takeover GO / DISMISS -> TAKEOVER_GO / TAKEOVER_DISMISS        */
/* (S16 slice c2 — moved from test_scr_flare.c, which used to build      */
/* against a live ff_flare_t and assert on the CORE struct; the button   */
/* no longer takes one at all, so the seam is now the only thing to      */
/* assert against here.)                                                */
/* =================================================================== */

static void S16_c2_flare_takeover_go_emits_takeover_go(void)
{
    ff_app_flare_t disp;
    memset(&disp, 0, sizeof(disp));
    disp.takeover_active = true;
    strncpy(disp.takeover_from_name, "KEV", sizeof(disp.takeover_from_name) - 1);

    ff_scr_flare_build_takeover(&disp);

    click(find_button_with_label(lv_screen_active(), "GO"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_TAKEOVER_GO, s_spy.last.kind);
}

static void S16_c2_flare_takeover_dismiss_emits_takeover_dismiss(void)
{
    ff_app_flare_t disp;
    memset(&disp, 0, sizeof(disp));
    disp.takeover_active = true;
    strncpy(disp.takeover_from_name, "KEV", sizeof(disp.takeover_from_name) - 1);

    ff_scr_flare_build_takeover(&disp);

    click(find_button_with_label(lv_screen_active(), "DISMISS"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_TAKEOVER_DISMISS, s_spy.last.kind);
}

/* =================================================================== */
/* Sender overlay CANCEL -> FLARE_END (S16 slice c2)                    */
/* =================================================================== */

static void S16_c2_sender_overlay_cancel_emits_flare_end(void)
{
    ff_app_flare_t disp;
    memset(&disp, 0, sizeof(disp));
    disp.sending = true;
    disp.send_expires_in_ms = 299000;

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_flare_build_sender_overlay(parent, &disp);

    click(find_button_with_label(parent, "CANCEL"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_FLARE_END, s_spy.last.kind);
}

/* =================================================================== */
/* Unbound seam (goldens/headless) — every wired site is a safe no-op   */
/* =================================================================== */

static void S16_c1_wired_sites_are_noops_while_the_seam_is_unbound(void)
{
    ff_intent_emit_bind(NULL, NULL); /* the goldens/headless state */

    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    state.active_face = FF_APP_FACE_SIGNALS;
    ff_scr_nav_build(&state);

    lv_obj_t *puck = lv_obj_get_child(lv_screen_active(), 0);
    TEST_ASSERT_NOT_NULL(puck);
    (void)lv_obj_send_event(puck, LV_EVENT_LONG_PRESSED, NULL);
    click(find_button_with_label(lv_screen_active(), "+"));

    TEST_ASSERT_EQUAL_INT(0, s_spy.count); /* nothing reached the (unbound) spy — and nothing crashed */
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S16_c1_nav_long_press_callback_emits_open_settings);
    RUN_TEST(S16_c1_compose_back_emits_back);
    RUN_TEST(S16_c2_compose_send_emits_send_text);
    RUN_TEST(S16_c1_signals_plus_emits_open_compose_with_no_destination);
    RUN_TEST(S16_c2_signals_rally_tap_emits_select_rally_with_its_index);
    RUN_TEST(S16_c2_signals_canned_reply_chips_emit_canned_reply);
    RUN_TEST(S16_c2_radar_flare_button_emits_flare_start);
    RUN_TEST(S16_c2_flare_takeover_go_emits_takeover_go);
    RUN_TEST(S16_c2_flare_takeover_dismiss_emits_takeover_dismiss);
    RUN_TEST(S16_c2_sender_overlay_cancel_emits_flare_end);
    RUN_TEST(S16_c1_wired_sites_are_noops_while_the_seam_is_unbound);

    return UNITY_END();
}
