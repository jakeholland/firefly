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
#include "scr_settings.h"
#include "scr_signals.h"

/* Frozen-by-default tick — same convention as test_scr_flare.c: nothing
 * here renders a frame, so every existing test in this file (all
 * `lv_obj_send_event` direct injection, no `lv_timer_handler` loop) never
 * observes it moving. `s_fake_tick_ms` exists for the drag() helper below
 * (S16 slice c3's swipe-gesture tests) ONLY: LVGL's indev read timer is
 * scheduled on real elapsed ticks (`LV_DEF_REFR_PERIOD`, 33 ms) even in a
 * tight synchronous test loop, so a genuinely-frozen tick would let the
 * FIRST `lv_timer_handler()` call read the press and then silently stop
 * reading every subsequent position update — a drag that "moves" but is
 * never actually seen moving, and no gesture accumulates. Reset to 0 in
 * setUp() so every OTHER test keeps seeing a frozen 0, exactly as before. */
static uint32_t s_fake_tick_ms;

static uint32_t test_tick_cb(void)
{
    return s_fake_tick_ms;
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
    s_fake_tick_ms = 0;
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
 * HIGH finding).
 *
 * `lv_obj_send_event` delivers LV_EVENT_LONG_PRESSED DIRECTLY to the
 * puck, so this test pins only the CALLBACK: when the hook fires, it
 * emits OPEN_SETTINGS, exactly once. It does NOT prove a physical
 * long-press reaches that callback — see
 * `S16_c3_physical_long_press_on_empty_puck_space_reaches_open_settings`
 * below for the probe that does, using the same technique the reviewer
 * used (`lv_indev_search_obj`) rather than assuming the fix worked.
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

/**
 * The reachability probe itself (S16 slice c3, PR #54 review, HIGH —
 * closed here). Uses `lv_indev_search_obj` — the same function LVGL's own
 * indev processing calls to decide who a real touch's events go to, and
 * the same one the reviewer used to MEASURE the original defect — rather
 * than assuming a real finger behaves like `lv_obj_send_event` does.
 *
 * Two things are proven, not one:
 *  1. The topology fact the reviewer measured is STILL true: a press over
 *     empty puck space does not resolve to the puck itself (the tileview
 *     still covers it). If a future refactor cleared CLICKABLE on the
 *     tileview instead of fixing bubbling, this assertion would fail
 *     loudly rather than the test quietly stopping to mean anything.
 *  2. What LVGL would ACTUALLY deliver LONG_PRESSED to — the resolved
 *     object, not the puck — still reaches OPEN_SETTINGS, via the
 *     EVENT_BUBBLE chain `ff_scr_nav_build` now sets up (tile ->
 *     tileview -> puck).
 */
static void S16_c3_physical_long_press_on_empty_puck_space_reaches_open_settings(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state)); /* RADAR_LIVE (mode 0): no clickable content at all */
    state.active_face = FF_APP_FACE_RADAR;

    ff_scr_nav_build(&state);

    lv_obj_t *puck = lv_obj_get_child(lv_screen_active(), 0);
    TEST_ASSERT_NOT_NULL(puck);

    lv_point_t pt = {228, 228}; /* puck/window center; nothing clickable sits here in this state */
    lv_obj_t *hit = lv_indev_search_obj(lv_screen_active(), &pt);
    TEST_ASSERT_NOT_NULL(hit);
    TEST_ASSERT_NOT_EQUAL(puck, hit); /* still the tileview/tile, not the puck — the topology is unchanged */

    lv_result_t r = lv_obj_send_event(hit, LV_EVENT_LONG_PRESSED, NULL);
    TEST_ASSERT_EQUAL(LV_RESULT_OK, r);

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_OPEN_SETTINGS, s_spy.last.kind);
}

/**
 * The other half of "don't fix it with a bare EVENT_BUBBLE flag" (the
 * TODO this slice closed): a long-press ON a content button must NOT
 * bubble up and misfire OPEN_SETTINGS. Only the tileview and its tiles
 * (container objects) carry LV_OBJ_FLAG_EVENT_BUBBLE — content controls
 * like FLARE never do — so this stays confined to the button.
 */
static void S16_c3_content_button_long_press_does_not_reach_open_settings(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    state.active_face = FF_APP_FACE_RADAR;
    state.radar.mode = RADAR_CLOSE;
    strncpy(state.radar.name, "DANA", sizeof(state.radar.name) - 1);
    strncpy(state.radar.dist_str, "15 m", sizeof(state.radar.dist_str) - 1);

    ff_scr_nav_build(&state);

    lv_obj_t *flare_btn = find_button_with_label(lv_screen_active(), "FLARE");
    TEST_ASSERT_NOT_NULL(flare_btn);

    lv_result_t r = lv_obj_send_event(flare_btn, LV_EVENT_LONG_PRESSED, NULL);
    TEST_ASSERT_EQUAL(LV_RESULT_OK, r);

    TEST_ASSERT_EQUAL_INT(0, s_spy.count); /* did not bubble up to OPEN_SETTINGS */
}

/* =================================================================== */
/* Physical swipe -> SWIPE (S16 slice c3: ff_route owns face nav now,   */
/* not the tileview's own native drag-to-scroll)                        */
/* =================================================================== */

static lv_point_t s_probe_pt;
static lv_indev_state_t s_probe_state;

static void probe_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = s_probe_pt;
    data->state = s_probe_state;
}

/* A press -> several move steps -> release sequence through a REAL
 * pointer indev, same shape as targets/sim/main.c's ff_loop_swipe (the
 * production ctl-socket "swipe" command). */
static void drag(int32_t from_x, int32_t to_x, int32_t y)
{
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, probe_read_cb);

    /* LVGL's indev read timer only re-fires on real elapsed ticks (see
     * s_fake_tick_ms's comment above) — advance past LV_DEF_REFR_PERIOD
     * (33 ms) before every handler call, not just the first, or every
     * position update after the initial press is silently never read. */
    s_probe_pt.x = (lv_coord_t)from_x;
    s_probe_pt.y = (lv_coord_t)y;
    s_probe_state = LV_INDEV_STATE_PRESSED;
    s_fake_tick_ms += 40u;
    lv_timer_handler();

    enum { STEPS = 6 };
    for (int i = 1; i <= STEPS; i++) {
        s_probe_pt.x = (lv_coord_t)(from_x + (to_x - from_x) * i / STEPS);
        s_fake_tick_ms += 40u;
        lv_timer_handler();
    }

    s_probe_state = LV_INDEV_STATE_RELEASED;
    s_fake_tick_ms += 40u;
    lv_timer_handler();

    lv_indev_delete(indev);
}

static void S16_c3_physical_leftward_drag_emits_swipe_toward_signals(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    state.active_face = FF_APP_FACE_RADAR;
    ff_scr_nav_build(&state);

    drag(380, 60, 228); /* finger moving LEFT */

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SWIPE, s_spy.last.kind);
    TEST_ASSERT_EQUAL_INT8(1, s_spy.last.u.swipe_dir); /* toward SIGNALS */
}

static void S16_c3_physical_rightward_drag_emits_swipe_toward_radar(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    state.active_face = FF_APP_FACE_RADAR;
    ff_scr_nav_build(&state);

    drag(60, 380, 228); /* finger moving RIGHT */

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SWIPE, s_spy.last.kind);
    TEST_ASSERT_EQUAL_INT8(-1, s_spy.last.u.swipe_dir); /* toward RADAR */
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
     * shape as every other no-payload intent this file pins. What the
     * shell DOES with it once dispatched (actually sends, as of S16
     * slice c3) is test_shell.c's job; this test's job is only that the
     * button reaches the seam at all. */
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose));

    ff_scr_compose_build(&compose);

    click(find_button_with_label(lv_screen_active(), "SEND"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SEND_TEXT, s_spy.last.kind);
}

/* =================================================================== */
/* Compose keypad -> T9_KEY / T9_SPACE / T9_BACKSPACE / T9_MODE /       */
/* T9_INSERT (S16 slice c3)                                             */
/* =================================================================== */

/**
 * ABC mode (the build-time default): a letter key -> T9_KEY carrying the
 * raw key number, and the bottom-row key -> T9_SPACE. Only `.kind`/
 * `.u.t9_key` are asserted for T9_KEY (a scalar, safely copied by value);
 * a T9_INSERT payload is a BORROWED pointer into a stack buffer that's
 * already gone by the time `click()` returns (ff_intent.h, "Payload
 * ownership") — dereferencing it here would be exactly the bug that
 * contract forbids, so the 123/SYM tests below assert `.kind` only, and
 * the actual bytes are pinned at the shell level instead
 * (test_shell.c's S16_AC3b/S16_c3_send_text_* tests, which own the
 * intent's payload for the whole call by construction).
 */
static void S16_c3_abc_letter_key_emits_t9_key(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose)); /* mode 0 = FF_APP_COMPOSE_ABC */

    ff_scr_compose_build(&compose);

    click(find_button_with_label(lv_screen_active(), "DEF")); /* key 3 */

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_T9_KEY, s_spy.last.kind);
    TEST_ASSERT_EQUAL_UINT8(3, s_spy.last.u.t9_key);
}

static void S16_c3_abc_space_key_emits_t9_space(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose));

    ff_scr_compose_build(&compose);

    click(find_button_with_label(lv_screen_active(), "SPACE"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_T9_SPACE, s_spy.last.kind);
}

static void S16_c3_del_key_emits_t9_backspace(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose));

    ff_scr_compose_build(&compose);

    click(find_button_with_label(lv_screen_active(), "DEL"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_T9_BACKSPACE, s_spy.last.kind);
}

/* 123 mode: every key (0-9 alike) is a literal digit insert. */
static void S16_c3_123_digit_key_emits_t9_insert(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose));
    compose.mode = FF_APP_COMPOSE_123;

    ff_scr_compose_build(&compose);

    click(find_button_with_label(lv_screen_active(), "7"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_T9_INSERT, s_spy.last.kind);
}

/* SYM mode: a symbol key is a literal (multi-char) insert; key 0 is still
 * SPACE (typed sentences with emoticons still need spaces — S08
 * Amendments). */
static void S16_c3_sym_symbol_key_emits_t9_insert(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose));
    compose.mode = FF_APP_COMPOSE_SYM;

    ff_scr_compose_build(&compose);

    click(find_button_with_label(lv_screen_active(), ":)"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_T9_INSERT, s_spy.last.kind);
}

static void S16_c3_sym_space_key_emits_t9_space(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose));
    compose.mode = FF_APP_COMPOSE_SYM;

    ff_scr_compose_build(&compose);

    click(find_button_with_label(lv_screen_active(), "SPACE"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_T9_SPACE, s_spy.last.kind);
}

/**
 * Mode chip -> T9_MODE, no payload (the shell owns the cycle as of this
 * slice — see ff_shell.c). Built in 123 mode specifically: the chip's
 * OWN label is the current mode's name (`compose_update_mode_chip_label`),
 * and in ABC mode that label ("ABC") COLLIDES with the ABC grid's own
 * key-2 legend (also "ABC") — `find_button_with_label`'s depth-first
 * search would find the mode chip first only by creation-order luck, so
 * this test deliberately picks a mode ("123") whose current label text
 * doesn't collide with any grid legend, rather than depend on that luck.
 */
static void S16_c3_mode_chip_emits_t9_mode(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose));
    compose.mode = FF_APP_COMPOSE_123;

    ff_scr_compose_build(&compose);

    click(find_button_with_label(lv_screen_active(), "123"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_T9_MODE, s_spy.last.kind);
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
/* Settings face (S11 slice b) — every control emits FF_INTENT_SETTING_SET */
/* except BACK, which is the same FF_INTENT_BACK every other modal uses.  */
/* =================================================================== */

static void S11b_settings_back_emits_back(void)
{
    ff_app_settings_t s;
    memset(&s, 0, sizeof(s));

    ff_scr_settings_build(&s);

    click(find_button_with_label(lv_screen_active(), "<"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_BACK, s_spy.last.kind);
}

static void S11b_settings_units_chip_toggles_imperial(void)
{
    ff_app_settings_t s;
    memset(&s, 0, sizeof(s));
    s.imperial = true; /* renders "FT" */

    ff_scr_settings_build(&s);

    click(find_button_with_label(lv_screen_active(), "FT"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SETTING_SET, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_SETTING_IMPERIAL, s_spy.last.u.setting.id);
    TEST_ASSERT_EQUAL_INT32(0, s_spy.last.u.setting.v.i); /* FT -> M */
}

static void S11b_settings_share_chip_cycles_live_to_zones(void)
{
    ff_app_settings_t s;
    memset(&s, 0, sizeof(s)); /* share_mode 0 = FF_SHARE_LIVE, renders "LIVE" */

    ff_scr_settings_build(&s);

    click(find_button_with_label(lv_screen_active(), "LIVE"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SETTING_SET, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_SETTING_SHARE_MODE, s_spy.last.u.setting.id);
    TEST_ASSERT_EQUAL_INT32(1, s_spy.last.u.setting.v.i); /* LIVE -> ZONES */
}

static void S11b_settings_haptics_chip_toggles(void)
{
    ff_app_settings_t s;
    memset(&s, 0, sizeof(s));
    s.haptics = false; /* renders "BUZZ OFF" */

    ff_scr_settings_build(&s);

    click(find_button_with_label(lv_screen_active(), "BUZZ OFF"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SETTING_SET, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_SETTING_HAPTICS, s_spy.last.u.setting.id);
    TEST_ASSERT_EQUAL_INT32(1, s_spy.last.u.setting.v.i); /* OFF -> ON */
}

static void S11b_settings_night_glow_chip_toggles(void)
{
    ff_app_settings_t s;
    memset(&s, 0, sizeof(s));
    s.night_glow = true; /* renders "GLOW ON" */

    ff_scr_settings_build(&s);

    click(find_button_with_label(lv_screen_active(), "GLOW ON"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SETTING_SET, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_SETTING_NIGHT_GLOW, s_spy.last.u.setting.id);
    TEST_ASSERT_EQUAL_INT32(0, s_spy.last.u.setting.v.i); /* ON -> OFF */
}

static void S11b_settings_water_chip_cycles_presets(void)
{
    ff_app_settings_t s;
    memset(&s, 0, sizeof(s));
    s.water_min = 90; /* renders "90 MIN" */

    ff_scr_settings_build(&s);

    click(find_button_with_label(lv_screen_active(), "90 MIN"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SETTING_SET, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_SETTING_WATER_MIN, s_spy.last.u.setting.id);
    TEST_ASSERT_EQUAL_INT32(120, s_spy.last.u.setting.v.i); /* 90 -> 120, spec's v1 cycle */
}

/* A quiet-hours tap sets TWO fields (quiet_from_min AND quiet_to_min) —
 * two separate FF_INTENT_SETTING_SET emits per click. The shared s_spy
 * only remembers the LAST intent, which is enough for every other test in
 * this file (one control, one emit) but not this one, so this test binds
 * its own small history spy for the duration of the click. */
typedef struct {
    int count;
    ff_intent_t items[4];
} history_spy_t;

static void history_spy_cb(void *user, ff_intent_t const *in)
{
    history_spy_t *h = (history_spy_t *)user;
    if (h->count < (int)(sizeof(h->items) / sizeof(h->items[0]))) {
        h->items[h->count] = *in;
    }
    h->count++;
}

static void S11b_settings_quiet_chip_sets_both_from_and_to(void)
{
    ff_app_settings_t s;
    memset(&s, 0, sizeof(s));
    s.quiet_from_min = 240;
    s.quiet_to_min = 600; /* the "4A-10A" preset */

    ff_scr_settings_build(&s);

    history_spy_t hist;
    memset(&hist, 0, sizeof(hist));
    ff_intent_emit_bind(history_spy_cb, &hist);

    click(find_button_with_label(lv_screen_active(), "4A-10A"));

    /* Cycle order per docs/specs/S11-settings.md: off -> 2a-8a -> 4a-10a
     * -> off. From 4A-10A, the next preset is OFF (0, 0). */
    TEST_ASSERT_EQUAL_INT(2, hist.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SETTING_SET, hist.items[0].kind);
    TEST_ASSERT_EQUAL(FF_SETTING_QUIET_FROM_MIN, hist.items[0].u.setting.id);
    TEST_ASSERT_EQUAL_INT32(0, hist.items[0].u.setting.v.i);
    TEST_ASSERT_EQUAL(FF_INTENT_SETTING_SET, hist.items[1].kind);
    TEST_ASSERT_EQUAL(FF_SETTING_QUIET_TO_MIN, hist.items[1].u.setting.id);
    TEST_ASSERT_EQUAL_INT32(0, hist.items[1].u.setting.v.i);

    /* Restore the shared spy for tearDown's unbind + any later test. */
    ff_intent_emit_bind(spy_sink_cb, &s_spy);
}

static void S11b_settings_utc_offset_stepper_minus_and_plus(void)
{
    ff_app_settings_t s;
    memset(&s, 0, sizeof(s));
    s.utc_offset_set = true;
    s.utc_offset_min = -300; /* renders "UTC-5:00" */

    ff_scr_settings_build(&s);

    click(find_button_with_label(lv_screen_active(), "-"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SETTING_SET, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_SETTING_UTC_OFFSET_MIN, s_spy.last.u.setting.id);
    TEST_ASSERT_EQUAL_INT32(-360, s_spy.last.u.setting.v.i);

    click(find_button_with_label(lv_screen_active(), "+"));
    TEST_ASSERT_EQUAL_INT(2, s_spy.count);
    TEST_ASSERT_EQUAL(FF_SETTING_UTC_OFFSET_MIN, s_spy.last.u.setting.id);
    TEST_ASSERT_EQUAL_INT32(-240, s_spy.last.u.setting.v.i);
}

/* Unset (never configured) starts stepping from 0 (UTC), not from garbage
 * or a rejected/no-op tap — there is no honest "current" numeric value to
 * step from otherwise (scr_settings.c's settings_utc_base). */
static void S11b_settings_utc_offset_stepper_starts_from_utc_when_unset(void)
{
    ff_app_settings_t s;
    memset(&s, 0, sizeof(s)); /* utc_offset_set false: renders "UNSET" */

    ff_scr_settings_build(&s);

    click(find_button_with_label(lv_screen_active(), "+"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SETTING_SET, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_SETTING_UTC_OFFSET_MIN, s_spy.last.u.setting.id);
    TEST_ASSERT_EQUAL_INT32(60, s_spy.last.u.setting.v.i);
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
    RUN_TEST(S16_c3_physical_long_press_on_empty_puck_space_reaches_open_settings);
    RUN_TEST(S16_c3_content_button_long_press_does_not_reach_open_settings);
    RUN_TEST(S16_c3_physical_leftward_drag_emits_swipe_toward_signals);
    RUN_TEST(S16_c3_physical_rightward_drag_emits_swipe_toward_radar);
    RUN_TEST(S16_c1_compose_back_emits_back);
    RUN_TEST(S16_c2_compose_send_emits_send_text);
    RUN_TEST(S16_c3_abc_letter_key_emits_t9_key);
    RUN_TEST(S16_c3_abc_space_key_emits_t9_space);
    RUN_TEST(S16_c3_del_key_emits_t9_backspace);
    RUN_TEST(S16_c3_123_digit_key_emits_t9_insert);
    RUN_TEST(S16_c3_sym_symbol_key_emits_t9_insert);
    RUN_TEST(S16_c3_sym_space_key_emits_t9_space);
    RUN_TEST(S16_c3_mode_chip_emits_t9_mode);
    RUN_TEST(S16_c1_signals_plus_emits_open_compose_with_no_destination);
    RUN_TEST(S16_c2_signals_rally_tap_emits_select_rally_with_its_index);
    RUN_TEST(S16_c2_signals_canned_reply_chips_emit_canned_reply);
    RUN_TEST(S16_c2_radar_flare_button_emits_flare_start);
    RUN_TEST(S16_c2_flare_takeover_go_emits_takeover_go);
    RUN_TEST(S16_c2_flare_takeover_dismiss_emits_takeover_dismiss);
    RUN_TEST(S16_c2_sender_overlay_cancel_emits_flare_end);
    RUN_TEST(S11b_settings_back_emits_back);
    RUN_TEST(S11b_settings_units_chip_toggles_imperial);
    RUN_TEST(S11b_settings_share_chip_cycles_live_to_zones);
    RUN_TEST(S11b_settings_haptics_chip_toggles);
    RUN_TEST(S11b_settings_night_glow_chip_toggles);
    RUN_TEST(S11b_settings_water_chip_cycles_presets);
    RUN_TEST(S11b_settings_quiet_chip_sets_both_from_and_to);
    RUN_TEST(S11b_settings_utc_offset_stepper_minus_and_plus);
    RUN_TEST(S11b_settings_utc_offset_stepper_starts_from_utc_when_unset);
    RUN_TEST(S16_c1_wired_sites_are_noops_while_the_seam_is_unbound);

    return UNITY_END();
}
