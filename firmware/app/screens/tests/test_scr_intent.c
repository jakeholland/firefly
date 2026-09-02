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
#include "ff_settings.h" /* FF_SHARE_LIVE/_ZONES/_GHOST, for the settings-face share-mode tests */
#include "scr_compose.h"
#include "scr_flare.h"
#include "scr_launcher.h" /* S26 slice e — the BOOT-button launcher */
#include "scr_nav.h"
#include "scr_radar.h"
#include "scr_settings.h"
#include "scr_signals.h"

#include "radar_layout.h" /* RADAR_LAYOUT_DOT_PX — S17a AC4 render tests, see that section below */

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
    lv_display_t *disp = lv_display_create(412, 412);
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
/* nav long-press -> OPEN_SETTINGS — RETIRED (S26 slice e)               */
/*                                                                       */
/* scr_nav.c no longer has a long-press hook at all: `nav_long_press_cb` */
/* and the puck's LONG_PRESSED binding are gone with the carousel        */
/* (Settings is a launcher circle now — scr_launcher.h). The three tests */
/* this section used to hold (the callback proof, the physical           */
/* reachability probe via `lv_indev_search_obj`, and the                 */
/* content-button-does-not-bubble guard) tested a mechanism that no      */
/* longer exists; removed rather than left asserting nothing meaningful. */
/* A long-press anywhere now emits no intent at all — implicitly covered */
/* by every other test in this file, none of which sends LONG_PRESSED    */
/* and expects a count.                                                  */
/* =================================================================== */

/* =================================================================== */
/* S26 slice e, AC3 — a physical drag, any direction, changes NOTHING   */
/* (the carousel is retired; scr_nav.c has no gesture handler at all    */
/* any more, so there is nothing left for an unclaimed drag to become — */
/* it is simply left as whatever scroll the content underneath makes    */
/* of it, same as a vertical drag always was).                          */
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

/* Vertical counterpart to `drag` above — same real-indev press/move/
 * release shape, varying y instead of x. Horizontal-carousel rework: a
 * vertical drag through nav_swipe_gesture_cb is now a NO-OP (LV_DIR_TOP
 * and LV_DIR_BOTTOM return without emitting), so a vertical drag is left
 * to be a scroll and never a face change. */
static void drag_v(int32_t from_y, int32_t to_y, int32_t x)
{
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, probe_read_cb);

    s_probe_pt.x = (lv_coord_t)x;
    s_probe_pt.y = (lv_coord_t)from_y;
    s_probe_state = LV_INDEV_STATE_PRESSED;
    s_fake_tick_ms += 40u;
    lv_timer_handler();

    enum { STEPS = 6 };
    for (int i = 1; i <= STEPS; i++) {
        s_probe_pt.y = (lv_coord_t)(from_y + (to_y - from_y) * i / STEPS);
        s_fake_tick_ms += 40u;
        lv_timer_handler();
    }

    s_probe_state = LV_INDEV_STATE_RELEASED;
    s_fake_tick_ms += 40u;
    lv_timer_handler();

    lv_indev_delete(indev);
}

/* S26e AC3: "a horizontal drag on Radar/Now/Signals changes nothing" —
 * both directions, on all three named faces. scr_nav.c has no gesture
 * handler at all any more, so this is really testing that nothing NEW
 * grew one; a leftover call site elsewhere would still leave this
 * green, which is the point (nothing here to wire up). */
static void S26e_AC3_horizontal_drag_emits_nothing(ff_app_face_t face)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    state.active_face = face;
    ff_scr_nav_build(&state);

    drag(380, 60, 228); /* finger moving LEFT */
    TEST_ASSERT_EQUAL_INT(0, s_spy.count);

    drag(60, 380, 228); /* finger moving RIGHT */
    TEST_ASSERT_EQUAL_INT(0, s_spy.count);
}

static void S26e_AC3_horizontal_drag_on_radar_emits_nothing(void)
{
    S26e_AC3_horizontal_drag_emits_nothing(FF_APP_FACE_RADAR);
}

static void S26e_AC3_horizontal_drag_on_now_emits_nothing(void)
{
    S26e_AC3_horizontal_drag_emits_nothing(FF_APP_FACE_NOW);
}

static void S26e_AC3_horizontal_drag_on_signals_emits_nothing(void)
{
    S26e_AC3_horizontal_drag_emits_nothing(FF_APP_FACE_SIGNALS);
}

/* Vertical too, both directions — the scroll-vs-swipe property the
 * horizontal-carousel rework introduced (a vertical drag is always a
 * scroll, never a face change) still holds now that there is no
 * gesture handler left to hold it. */
static void carousel_physical_upward_drag_emits_nothing(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    state.active_face = FF_APP_FACE_RADAR;
    ff_scr_nav_build(&state);

    drag_v(380, 60, 228); /* finger moving UP */

    TEST_ASSERT_EQUAL_INT(0, s_spy.count);
}

static void carousel_physical_upward_drag_emits_nothing_from_signals_too(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    state.active_face = FF_APP_FACE_SIGNALS;
    ff_scr_nav_build(&state);

    drag_v(380, 60, 228);

    TEST_ASSERT_EQUAL_INT(0, s_spy.count);
}

static void carousel_physical_downward_drag_emits_nothing(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    state.active_face = FF_APP_FACE_RADAR;
    ff_scr_nav_build(&state);

    drag_v(60, 380, 228); /* finger moving DOWN */

    TEST_ASSERT_EQUAL_INT(0, s_spy.count);
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
/* Signals (S24 slice b): the inbox face's navigation intents            */
/* =================================================================== */

/* Small helpers to hand-build an ff_app_signals_t the screen renders —
 * the unit-test analog of the fixtures, so a test names exactly the
 * conversations it needs. */
static ff_inbox_conv_t *sig_add_conv(ff_app_signals_t *v, ff_conv_kind_t kind, uint32_t node_id,
                                     char const *name, uint16_t unread, uint8_t item_count)
{
    ff_inbox_conv_t *cv = &v->inbox.convs[v->inbox.conv_count++];
    memset(cv, 0, sizeof(*cv));
    cv->kind = kind;
    cv->node_id = node_id;
    if (name != NULL) strncpy(cv->name, name, sizeof(cv->name) - 1);
    cv->initial = (name != NULL && name[0] != '\0') ? name[0] : '\0';
    cv->unread = unread;
    cv->item_count = item_count;
    cv->has_preview = (item_count > 0);
    cv->presence_valid = (kind == FF_CONV_MEMBER);
    return cv;
}

/* Rows are plain lv_obj_t containers whose TAP TARGET is a transparent
 * overlay button created as the row's FIRST child (scr_signals.c's
 * signals_row_container — the touching-rows/adjacency-floor shape). A
 * row's only unique on-screen text is its name label, so the lookup goes
 * by that, steps up to the row, and takes child 0 — the overlay. (Shared
 * find_label_exact below is also used by the settings-face tests.) */
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

/* The FAB's tap target carries no label (the + glyph is deco); find it
 * as the one clickable button sized exactly 112x112 (scr_signals.c's
 * FF_SIGNALS_FAB_HIT_PX = PUCK_PX - 300 — the corner-anchored hit that
 * covers the whole visible amber lens). */
static lv_obj_t *find_clickable_by_size(lv_obj_t *root, int32_t w, int32_t h)
{
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        if (lv_obj_check_type(child, &lv_button_class) && lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE) &&
            lv_obj_get_style_width(child, 0) == w && lv_obj_get_style_height(child, 0) == h) {
            return child;
        }
        lv_obj_t *found = find_clickable_by_size(child, w, h);
        if (found != NULL) return found;
    }
    return NULL;
}

/* PR #142 review Design 2: the launcher's four circles are now all the
 * SAME size (96x96, up from 64x64), so `find_clickable_by_size` above
 * (which returns the FIRST match) can no longer tell them apart — and
 * Design 1 swapped their labels for LV_SYMBOL_* glyphs, so
 * `find_button_with_label` can't either (the glyph string is not a
 * stable per-circle identifier the way "NOW"/"SIG"/"MAP"/"SET" text
 * used to be). Walks the SAME depth-first, sibling-order traversal as
 * both helpers above and returns the `want`-th (0-based) same-size
 * button it finds — reliable because `ff_scr_launcher_build` adds the
 * four circles as direct puck children in a FIXED order (Now, Signals,
 * Map, Settings — the same order `launcher_idx` encodes), so tree order
 * IS circle order. `*counter` is threaded through the recursion so one
 * top-level call counts correctly across the whole subtree. */
static lv_obj_t *find_nth_clickable_by_size(lv_obj_t *root, int32_t w, int32_t h, int *counter, int want)
{
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        if (lv_obj_check_type(child, &lv_button_class) && lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE) &&
            lv_obj_get_style_width(child, 0) == w && lv_obj_get_style_height(child, 0) == h) {
            if (*counter == want) {
                return child;
            }
            (*counter)++;
        }
        lv_obj_t *found = find_nth_clickable_by_size(child, w, h, counter, want);
        if (found != NULL) return found;
    }
    return NULL;
}

/* `idx`: 0=Now, 1=Signals, 2=Map, 3=Settings (ff_intent.h's
 * FF_INTENT_LAUNCHER_SELECT payload convention == scr_launcher.c's own
 * build order). */
static lv_obj_t *launcher_circle_at(int idx)
{
    int counter = 0;
    return find_nth_clickable_by_size(lv_screen_active(), 96, 96, &counter, idx);
}

/* A member conversation row tap emits OPEN_THREAD with that member's
 * node id. */
static void S24b_inbox_member_row_tap_emits_open_thread(void)
{
    ff_app_signals_t v;
    memset(&v, 0, sizeof(v));
    sig_add_conv(&v, FF_CONV_CREW, 0u, NULL, 0, 0);
    sig_add_conv(&v, FF_CONV_MEMBER, 111u, "DANA", 1, 2);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_signals_build(parent, &v, false);

    click(find_row_hit_by_name(parent, "DANA"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_OPEN_THREAD, s_spy.last.kind);
    TEST_ASSERT_EQUAL_UINT32(111u, s_spy.last.u.node_id);
}

/* The CREW row emits OPEN_THREAD with the CREW key (node 0). */
static void S24b_inbox_crew_row_tap_emits_open_thread_crew(void)
{
    ff_app_signals_t v;
    memset(&v, 0, sizeof(v));
    sig_add_conv(&v, FF_CONV_CREW, 0u, NULL, 2, 3);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_signals_build(parent, &v, false);

    click(find_row_hit_by_name(parent, "CREW"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_OPEN_THREAD, s_spy.last.kind);
    TEST_ASSERT_EQUAL_UINT32(0u, s_spy.last.u.node_id);
}

/* A QUIET member's row (no traffic) is a tap target too — the thread is
 * where you'd go to signal them. */
static void S24b_inbox_quiet_member_row_is_tappable(void)
{
    ff_app_signals_t v;
    memset(&v, 0, sizeof(v));
    sig_add_conv(&v, FF_CONV_CREW, 0u, NULL, 0, 0);
    ff_inbox_conv_t *max = sig_add_conv(&v, FF_CONV_MEMBER, 222u, "MAX", 0, 0);
    max->presence = FF_PRESENCE_LINKED;

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_signals_build(parent, &v, false);

    click(find_row_hit_by_name(parent, "MAX"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_OPEN_THREAD, s_spy.last.kind);
    TEST_ASSERT_EQUAL_UINT32(222u, s_spy.last.u.node_id);
}

/* The + FAB emits INBOX_NEW (the recipient-picker step). */
static void S24b_inbox_fab_emits_inbox_new(void)
{
    ff_app_signals_t v;
    memset(&v, 0, sizeof(v));
    sig_add_conv(&v, FF_CONV_CREW, 0u, NULL, 0, 0);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_signals_build(parent, &v, false);

    click(find_clickable_by_size(parent, 112, 112));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_NEW, s_spy.last.kind);
}

/* Picker rows emit INBOX_PICK with the picked scope; CREW is pinned and
 * carries the CREW key. */
static void S24b_picker_rows_emit_pick(void)
{
    ff_app_signals_t v;
    memset(&v, 0, sizeof(v));
    v.subview = FF_SIG_SUB_PICKER;
    sig_add_conv(&v, FF_CONV_CREW, 0u, NULL, 0, 0);
    sig_add_conv(&v, FF_CONV_MEMBER, 111u, "DANA", 0, 0);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_signals_build(parent, &v, false);

    click(find_row_hit_by_name(parent, "WHOLE CREW"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_PICK, s_spy.last.kind);
    TEST_ASSERT_EQUAL_UINT32(0u, s_spy.last.u.node_id);

    click(find_row_hit_by_name(parent, "DANA"));
    TEST_ASSERT_EQUAL_INT(2, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_PICK, s_spy.last.kind);
    TEST_ASSERT_EQUAL_UINT32(111u, s_spy.last.u.node_id);
}

/* Picker and thread-stub back buttons emit BACK (the shell pops the
 * sub-view back to the inbox). */
static void S24b_picker_and_thread_back_emit_back(void)
{
    ff_app_signals_t v;
    memset(&v, 0, sizeof(v));
    v.subview = FF_SIG_SUB_PICKER;
    sig_add_conv(&v, FF_CONV_CREW, 0u, NULL, 0, 0);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_signals_build(parent, &v, false);
    click(find_button_with_label(parent, LV_SYMBOL_LEFT));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_BACK, s_spy.last.kind);

    lv_obj_clean(parent);
    v.subview = FF_SIG_SUB_THREAD;
    v.thread_node = 0u;
    strncpy(v.thread_name, "CREW", sizeof(v.thread_name) - 1);
    ff_scr_signals_build(parent, &v, false);
    click(find_button_with_label(parent, LV_SYMBOL_LEFT));
    TEST_ASSERT_EQUAL_INT(2, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_BACK, s_spy.last.kind);
}

/* S24 slice (c) — a rendered 1:1 thread view for the chip tests: the
 * DANA scope, one inbound and one outgoing message. */
static void s24c_make_direct_thread(ff_app_signals_t *v)
{
    memset(v, 0, sizeof(*v));
    v->subview = FF_SIG_SUB_THREAD;
    v->thread_node = 111u;
    strncpy(v->thread_name, "DANA", sizeof(v->thread_name) - 1);
    sig_add_conv(v, FF_CONV_CREW, 0u, NULL, 0, 0);
    ff_inbox_conv_t *dana = sig_add_conv(v, FF_CONV_MEMBER, 111u, "DANA", 0, 2);
    dana->presence = FF_PRESENCE_SEEN;
    dana->presence_age_ms = 60000u;

    ff_inbox_msg_t *out = &v->thread.msgs[v->thread.msg_count++];
    memset(out, 0, sizeof(*out));
    out->kind = FEED_TEXT;
    out->dir = FEED_DIR_OUT;
    strncpy(out->text, "at the tower", sizeof(out->text) - 1);
    ff_inbox_msg_t *in_m = &v->thread.msgs[v->thread.msg_count++];
    memset(in_m, 0, sizeof(*in_m));
    in_m->kind = FEED_PULSE;
    in_m->dir = FEED_DIR_DIRECT;
    in_m->identity_known = true;
    in_m->node_id = 111u;
    strncpy(in_m->name, "DANA", sizeof(in_m->name) - 1);
}

/* The 1:1 quick chips: OMW and IN 5 MIN emit CANNED_REPLY with the right
 * canned id (the shell aims them at the thread scope — its own test). */
static void S24c_thread_omw_chip_emits_canned_reply_omw(void)
{
    ff_app_signals_t v;
    s24c_make_direct_thread(&v);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_signals_build(parent, &v, false);

    click(find_button_with_label(parent, "OMW"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_CANNED_REPLY, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_WIRING_REPLY_OMW, s_spy.last.u.reply);
}

static void S24c_thread_in5min_chip_emits_canned_reply_5min(void)
{
    ff_app_signals_t v;
    s24c_make_direct_thread(&v);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_signals_build(parent, &v, false);

    click(find_button_with_label(parent, "IN 5 MIN"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_CANNED_REPLY, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_WIRING_REPLY_5MIN, s_spy.last.u.reply);
}

/* The FLARE chip emits the outbound send intent, no payload (the outbound
 * quick signal is a flare, not a pulse). */
static void S24c_thread_flare_chip_emits_sig_flare(void)
{
    ff_app_signals_t v;
    s24c_make_direct_thread(&v);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_signals_build(parent, &v, false);

    click(find_button_with_label(parent, "FLARE"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SIG_FLARE, s_spy.last.kind);
}

/* The thread FAB emits INBOX_NEW (slice (d) routes it to the scoped
 * action popup; until then the shell's INBOX_NEW handler is a documented
 * no-op outside the inbox). Both thread shapes carry the FAB. */
static void S24c_thread_fab_emits_inbox_new(void)
{
    ff_app_signals_t v;
    s24c_make_direct_thread(&v);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_signals_build(parent, &v, false);
    click(find_clickable_by_size(parent, 112, 112));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_NEW, s_spy.last.kind);

    /* CREW thread too. */
    lv_obj_clean(parent);
    memset(&v, 0, sizeof(v));
    v.subview = FF_SIG_SUB_THREAD;
    v.thread_node = 0u;
    strncpy(v.thread_name, "CREW", sizeof(v.thread_name) - 1);
    sig_add_conv(&v, FF_CONV_CREW, 0u, NULL, 0, 0);
    ff_scr_signals_build(parent, &v, false);
    click(find_clickable_by_size(parent, 112, 112));
    TEST_ASSERT_EQUAL_INT(2, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_NEW, s_spy.last.kind);
}

/* The CREW thread renders NO quick chips (they are the 1:1 screen's) —
 * a chip label must not be findable there. */
static void S24c_crew_thread_has_no_quick_chips(void)
{
    ff_app_signals_t v;
    memset(&v, 0, sizeof(v));
    v.subview = FF_SIG_SUB_THREAD;
    v.thread_node = 0u;
    strncpy(v.thread_name, "CREW", sizeof(v.thread_name) - 1);
    sig_add_conv(&v, FF_CONV_CREW, 0u, NULL, 0, 0);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_signals_build(parent, &v, false);
    TEST_ASSERT_NULL(find_button_with_label(parent, "OMW"));
    TEST_ASSERT_NULL(find_button_with_label(parent, "FLARE"));
}

/* =================================================================== */
/* S24 slice d — action popup + Rally screen emitters                  */
/* =================================================================== */

/* Build the action popup sub-view scoped to a member (DANA). */
static lv_obj_t *build_popup(ff_app_signals_t *v)
{
    memset(v, 0, sizeof(*v));
    v->subview = FF_SIG_SUB_POPUP;
    v->thread_node = 111u;
    strncpy(v->thread_name, "DANA", sizeof(v->thread_name) - 1);
    sig_add_conv(v, FF_CONV_CREW, 0u, NULL, 0, 0);
    sig_add_conv(v, FF_CONV_MEMBER, 111u, "DANA", 0, 0);
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_signals_build(parent, v, false);
    return parent;
}

static void S24d_popup_compose_row_emits_popup_compose(void)
{
    ff_app_signals_t v;
    lv_obj_t *parent = build_popup(&v);
    click(find_button_with_label(parent, "Compose"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_POPUP_COMPOSE, s_spy.last.kind);
}

static void S24d_popup_rally_row_emits_popup_rally(void)
{
    ff_app_signals_t v;
    lv_obj_t *parent = build_popup(&v);
    click(find_button_with_label(parent, "Rally"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_POPUP_RALLY, s_spy.last.kind);
}

static void S24d_popup_flare_row_emits_popup_flare(void)
{
    ff_app_signals_t v;
    lv_obj_t *parent = build_popup(&v);
    click(find_button_with_label(parent, "Flare"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_POPUP_FLARE, s_spy.last.kind);
}

static void S24d_popup_close_emits_back(void)
{
    ff_app_signals_t v;
    lv_obj_t *parent = build_popup(&v);
    click(find_button_with_label(parent, LV_SYMBOL_CLOSE));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_BACK, s_spy.last.kind);
}

/* Build the Rally sub-view with On Me + one landmark, the landmark
 * selected, WHEN=Now, Send enabled. */
static lv_obj_t *build_rally(ff_app_signals_t *v)
{
    memset(v, 0, sizeof(*v));
    v->subview = FF_SIG_SUB_RALLY;
    v->thread_node = 111u;
    strncpy(v->thread_name, "DANA", sizeof(v->thread_name) - 1);
    sig_add_conv(v, FF_CONV_CREW, 0u, NULL, 0, 0);
    sig_add_conv(v, FF_CONV_MEMBER, 111u, "DANA", 0, 0);
    v->rally.on_me_ok = true;
    v->rally.place_count = 1;
    strncpy(v->rally.place_names[0], "Main Stage", sizeof(v->rally.place_names[0]) - 1);
    v->rally.sel = 1; /* the landmark */
    v->rally.when = FF_RALLY_WHEN_NOW;
    strncpy(v->rally.echo_place, "Main Stage", sizeof(v->rally.echo_place) - 1);
    strncpy(v->rally.echo_when, "Now", sizeof(v->rally.echo_when) - 1);
    v->rally.can_send = true;
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_signals_build(parent, v, false);
    return parent;
}

static void S24d_rally_on_me_row_emits_select_place_zero(void)
{
    ff_app_signals_t v;
    lv_obj_t *parent = build_rally(&v);
    click(find_row_hit_by_name(parent, "On Me"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_RALLY_SELECT_PLACE, s_spy.last.kind);
    TEST_ASSERT_EQUAL_UINT8(0u, s_spy.last.u.rally_idx);
}

static void S24d_rally_landmark_row_emits_select_place_index(void)
{
    ff_app_signals_t v;
    lv_obj_t *parent = build_rally(&v);
    click(find_row_hit_by_name(parent, "Main Stage"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_RALLY_SELECT_PLACE, s_spy.last.kind);
    TEST_ASSERT_EQUAL_UINT8(1u, s_spy.last.u.rally_idx); /* 0 = On Me, 1 = first landmark */
}

static void S24d_rally_when_chip_emits_cycle_when(void)
{
    ff_app_signals_t v;
    lv_obj_t *parent = build_rally(&v);
    click(find_button_with_label(parent, "WHEN"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_RALLY_CYCLE_WHEN, s_spy.last.kind);
}

static void S24d_rally_send_button_emits_rally_send(void)
{
    ff_app_signals_t v;
    lv_obj_t *parent = build_rally(&v);
    click(find_button_with_label(parent, "Send Rally"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_RALLY_SEND, s_spy.last.kind);
}

/* A disabled On Me (no fix) is NOT a tap target — the honest reason shows
 * but the row cannot be picked (never a fabricated position). */
static void S24d_rally_disabled_on_me_is_not_tappable(void)
{
    ff_app_signals_t v;
    memset(&v, 0, sizeof(v));
    v.subview = FF_SIG_SUB_RALLY;
    sig_add_conv(&v, FF_CONV_CREW, 0u, NULL, 0, 0);
    v.rally.on_me_ok = false; /* no fix */
    v.rally.place_count = 0;
    v.rally.can_send = false;
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_signals_build(parent, &v, false);
    /* The On Me row renders (its label is present) but has no clickable
     * tap target, and Send is a dead (non-button) control. */
    TEST_ASSERT_NOT_NULL(find_label_exact(parent, "On Me"));
    TEST_ASSERT_NULL(find_row_hit_by_name(parent, "On Me"));
    TEST_ASSERT_NULL(find_button_with_label(parent, "Send Rally"));
}

/* find_pill_in_row — the settings-face toggle rows (HAPTICS/GLOW/COLORBLIND)
 * all render generic "ON"/"OFF" pills, so a bare find_button_with_label would
 * ambiguously match the first such pill in the tree. This scopes the pill
 * lookup to the ROW that owns a given caption: find the caption label, step up
 * to its row container (the caption is a direct child of the row), then find
 * the pill by text within that subtree. Matches scr_settings.c's row shape
 * (row container -> [caption label, pill, pill]). */
static lv_obj_t *find_pill_in_row(lv_obj_t *root, char const *row_caption, char const *pill_text)
{
    lv_obj_t *cap = find_label_exact(root, row_caption);
    if (cap == NULL) {
        return NULL;
    }
    lv_obj_t *row = lv_obj_get_parent(cap);
    if (row == NULL) {
        return NULL;
    }
    return find_button_with_label(row, pill_text);
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
    ff_scr_radar_build(parent, &r, false);

    click(find_button_with_label(parent, "FLARE"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_FLARE_START, s_spy.last.kind);
}

/* =================================================================== */
/* Radar imprecise-dot RENDER styling (S17 slice a, issue #74)          */
/*                                                                       */
/* Code review finding on PR #83: the flag computation (ff_radar_dot_t  */
/* .imprecise, core/ff_radar.c) was assertion-tested                    */
/* (test_radar.c's S17a_AC4_dot_imprecise_flag_is_set_per_member...),   */
/* but the RENDER branch that actually satisfies AC4's wording           */
/* ("renders with the fuzzy treatment... not a crisp dot",               */
/* scr_radar.c:332-364) had NO assertion coverage — only the             */
/* radar_dot_precise.json/radar_dot_imprecise.json golden pair, whose    */
/* shared 0.5% pixel-diff threshold does not reliably catch a disabled   */
/* render branch on an element this small (measured: disabling the whole */
/* `if (d->imprecise)` block moves radar_dot_imprecise.png by 0.4506% —  */
/* under threshold, so the golden alone PASSES a real regression). Same  */
/* proxy-check class this PR already found and fixed for AC3's wedge-gap */
/* mutation (test_radar_layout.c's geometry assertions, not the golden). */
/* Closing the equivalent gap for AC4 here, per                          */
/* docs/review/code-review.md item 6 / AGENTS.md's standing brief:       */
/* measure the actual rendered style state, don't trust a shared         */
/* threshold to catch every behavior it happens to touch.                */
/* =================================================================== */

/* find_obj_by_size — recursive walk for the one crew-ring dot object in
 * a single-dot scene: radar_build_dots (scr_radar.c) sizes the dot's OWN
 * lv_obj_t (not its child label) to exactly RADAR_LAYOUT_DOT_PX square,
 * and checks it's a plain lv_obj (not a button/label/line/arc — every
 * other sized element on this face is one of those, or the whole-puck
 * gesture region, or a custom-drawn triangle). With n_dots == 1 there is
 * exactly one object this predicate can match, by construction. */
static lv_obj_t *find_obj_by_size(lv_obj_t *root, int32_t size)
{
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        if (lv_obj_check_type(child, &lv_obj_class) && lv_obj_get_width(child) == size &&
            lv_obj_get_height(child) == size) {
            return child;
        }
        lv_obj_t *found = find_obj_by_size(child, size);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

/* first_label_text — the dot's own child label (its initial letter, or
 * "" for the imprecise case) — every dot object radar_build_dots creates
 * has exactly one label child, added first, so index 0 is always it. */
static char const *first_label_text(lv_obj_t *dot)
{
    TEST_ASSERT_TRUE(lv_obj_get_child_count(dot) >= 1);
    lv_obj_t *label = lv_obj_get_child(dot, 0);
    TEST_ASSERT_TRUE(lv_obj_check_type(label, &lv_label_class));
    return lv_label_get_text(label);
}

/* Positive control for the imprecise test below: a full-precision dot
 * must NOT carry the fuzzy styling (AC4's own wording: "a full-precision
 * member is unchanged"). Solid fill, no border, its initial shown. */
static void S17a_AC4_radar_precise_dot_renders_filled_with_its_initial(void)
{
    ff_radar_view_t r;
    memset(&r, 0, sizeof(r));
    r.mode = RADAR_LIVE;
    r.arrow_valid = true;
    strncpy(r.name, "DANA", sizeof(r.name) - 1);
    strncpy(r.dist_str, "320 m", sizeof(r.dist_str) - 1);
    r.n_dots = 1;
    r.dots[0].ring_deg = 42.0f; /* clear of every reserved chrome rect — see radar_layout.h */
    r.dots[0].initial = 'R';
    r.dots[0].color_idx = 1;
    r.dots[0].stale = false;
    r.dots[0].imprecise = false;

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_radar_build(parent, &r, false);
    /* lv_obj_set_size only records the SPEC; actual lv_obj_get_width/
     * height() (what find_obj_by_size below reads) aren't resolved until
     * a layout pass runs — same requirement test_scr_flare.c's own
     * geometry assertions document and rely on. */
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *dot = find_obj_by_size(parent, (int32_t)RADAR_LAYOUT_DOT_PX);
    TEST_ASSERT_NOT_NULL(dot);

    TEST_ASSERT_EQUAL(LV_OPA_COVER, lv_obj_get_style_bg_opa(dot, LV_PART_MAIN));
    TEST_ASSERT_EQUAL_INT32(0, lv_obj_get_style_border_width(dot, LV_PART_MAIN));
    TEST_ASSERT_EQUAL_STRING("R", first_label_text(dot));
}

/* The finding's own assertion: hollow (transparent fill), a markedly
 * thicker/softer border than the crisp `stale` ghost ring's 2px/70% (see
 * scr_radar.c's comment on the exact values), and NO initial letter —
 * the three style facts the `d->imprecise` branch sets, checked directly
 * against the rendered lv_obj_t rather than inferred from a pixel diff. */
static void S17a_AC4_radar_imprecise_dot_renders_as_hollow_ring_with_no_initial(void)
{
    ff_radar_view_t r;
    memset(&r, 0, sizeof(r));
    r.mode = RADAR_LIVE;
    r.arrow_valid = true;
    strncpy(r.name, "DANA", sizeof(r.name) - 1);
    strncpy(r.dist_str, "320 m", sizeof(r.dist_str) - 1);
    r.n_dots = 1;
    r.dots[0].ring_deg = 42.0f;
    r.dots[0].initial = 'R';
    r.dots[0].color_idx = 1;
    r.dots[0].stale = false;
    r.dots[0].imprecise = true;

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_radar_build(parent, &r, false);
    /* lv_obj_set_size only records the SPEC; actual lv_obj_get_width/
     * height() (what find_obj_by_size below reads) aren't resolved until
     * a layout pass runs — same requirement test_scr_flare.c's own
     * geometry assertions document and rely on. */
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *dot = find_obj_by_size(parent, (int32_t)RADAR_LAYOUT_DOT_PX);
    TEST_ASSERT_NOT_NULL(dot);

    TEST_ASSERT_EQUAL(LV_OPA_TRANSP, lv_obj_get_style_bg_opa(dot, LV_PART_MAIN));
    TEST_ASSERT_EQUAL_INT32(6, lv_obj_get_style_border_width(dot, LV_PART_MAIN));
    TEST_ASSERT_EQUAL(LV_OPA_40, lv_obj_get_style_border_opa(dot, LV_PART_MAIN));
    TEST_ASSERT_EQUAL_STRING("", first_label_text(dot));
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
/* Settings face — every control emits FF_INTENT_SETTING_SET. There is   */
/* no BACK control any more: the horizontal-carousel rework made          */
/* Settings a swipe tile you leave by swiping, not a modal with a back    */
/* button (see scr_settings.h), so the old back-emits-BACK test is gone.  */
/* =================================================================== */

static void S11b_settings_units_chip_toggles_imperial(void)
{
    ff_app_settings_t s;
    memset(&s, 0, sizeof(s));
    s.imperial = true; /* renders "FT" */

    ff_scr_settings_build(lv_screen_active(), &s);

    click(find_button_with_label(lv_screen_active(), "FT"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SETTING_SET, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_SETTING_IMPERIAL, s_spy.last.u.setting.id);
    TEST_ASSERT_EQUAL_INT32(0, s_spy.last.u.setting.v.i); /* FT -> M */
}

/* PR #68 UX review (Bailey, blocking finding 1): ZONES is functionally
 * LIVE in this build (docs/specs/S11-settings.md's own "v1: LIVE/GHOST
 * honored; ZONES=LIVE + issue"), so the chip must never cycle a tap onto
 * it — LIVE and GHOST only, a plain two-stop loop. */
static void S11b_settings_share_chip_cycles_live_to_ghost_skipping_zones(void)
{
    ff_app_settings_t s;
    memset(&s, 0, sizeof(s)); /* share_mode 0 = FF_SHARE_LIVE, renders "LIVE" */

    ff_scr_settings_build(lv_screen_active(), &s);

    click(find_button_with_label(lv_screen_active(), "LIVE"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SETTING_SET, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_SETTING_SHARE_MODE, s_spy.last.u.setting.id);
    TEST_ASSERT_EQUAL_INT32(FF_SHARE_GHOST, s_spy.last.u.setting.v.i); /* LIVE -> GHOST, never ZONES */
}

/* The other direction, and the persisted-ZONES edge case: even if
 * `share_mode` somehow already reads ZONES, a tap moves it to GHOST —
 * never back into ZONES, and never stuck. */
static void S11b_settings_share_chip_from_ghost_and_from_zones_both_avoid_zones(void)
{
    ff_app_settings_t s;
    memset(&s, 0, sizeof(s));
    s.share_mode = FF_SHARE_GHOST;

    ff_scr_settings_build(lv_screen_active(), &s);
    click(find_button_with_label(lv_screen_active(), "GHOST"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_SETTING_SHARE_MODE, s_spy.last.u.setting.id);
    TEST_ASSERT_EQUAL_INT32(FF_SHARE_LIVE, s_spy.last.u.setting.v.i); /* GHOST -> LIVE */

    memset(&s_spy, 0, sizeof(s_spy));
    lv_obj_clean(lv_screen_active());
    memset(&s, 0, sizeof(s));
    s.share_mode = FF_SHARE_ZONES; /* the persisted-ZONES edge case */

    ff_scr_settings_build(lv_screen_active(), &s);
    /* The [LIVE|GHOST] pair shows neither pill active for a persisted ZONES,
     * but tapping either still moves it to GHOST (never back to ZONES). */
    click(find_pill_in_row(lv_screen_active(), "SHARE", "LIVE"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_SETTING_SHARE_MODE, s_spy.last.u.setting.id);
    TEST_ASSERT_EQUAL_INT32(FF_SHARE_GHOST, s_spy.last.u.setting.v.i); /* ZONES -> GHOST, not back to ZONES */
}

static void S11b_settings_haptics_chip_toggles(void)
{
    ff_app_settings_t s;
    memset(&s, 0, sizeof(s));
    s.haptics = false; /* OFF pill active */

    ff_scr_settings_build(lv_screen_active(), &s);

    /* Tapping either pill of the two-state pair flips it. */
    click(find_pill_in_row(lv_screen_active(), "HAPTICS", "OFF"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SETTING_SET, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_SETTING_HAPTICS, s_spy.last.u.setting.id);
    TEST_ASSERT_EQUAL_INT32(1, s_spy.last.u.setting.v.i); /* OFF -> ON */
}

static void S11b_settings_night_glow_chip_toggles(void)
{
    ff_app_settings_t s;
    memset(&s, 0, sizeof(s));
    s.night_glow = true; /* ON pill active */

    ff_scr_settings_build(lv_screen_active(), &s);

    click(find_pill_in_row(lv_screen_active(), "GLOW", "ON"));

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

    ff_scr_settings_build(lv_screen_active(), &s);

    click(find_button_with_label(lv_screen_active(), "90 MIN"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SETTING_SET, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_SETTING_WATER_MIN, s_spy.last.u.setting.id);
    TEST_ASSERT_EQUAL_INT32(120, s_spy.last.u.setting.v.i); /* 90 -> 120, spec's v1 cycle */
}

/* PR #68 UX review (Bailey, non-blocking finding, fixed here): the row
 * LABEL, not just the value chip, now forwards to the same intent — no
 * more silent dead zone on the left half of the row. */
static void S11b_settings_water_label_tap_also_cycles(void)
{
    ff_app_settings_t s;
    memset(&s, 0, sizeof(s));
    s.water_min = 90;

    ff_scr_settings_build(lv_screen_active(), &s);

    lv_obj_t *label = find_label_exact(lv_screen_active(), "WATER NUDGE");
    TEST_ASSERT_NOT_NULL(label);
    lv_obj_t *hit = lv_obj_get_parent(label);
    TEST_ASSERT_NOT_NULL(hit);
    TEST_ASSERT_TRUE(lv_obj_has_flag(hit, LV_OBJ_FLAG_CLICKABLE));

    click(hit);

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SETTING_SET, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_SETTING_WATER_MIN, s_spy.last.u.setting.id);
    TEST_ASSERT_EQUAL_INT32(120, s_spy.last.u.setting.v.i);
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

    ff_scr_settings_build(lv_screen_active(), &s);

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

/* The manual UTC-offset stepper was dropped from the Settings face (the
 * festpack supplies the timezone via fp_pack_t.utc_offset_min). The
 * FF_SETTING_UTC_OFFSET_MIN intent + its shell validation/clamp remain the
 * seam the wall clock reads, and are still covered by the shell-level
 * test_intent.c / test_ctl_flare_sequence.c cases — there is simply no
 * longer a screen control to emit it, so the two screen-level stepper tests
 * that used to live here are gone. */

/* #100/#bug2 — brightness is a −/+ STEPPER (two lv_button pills), not a slider:
 * a draggable control inside a vertical scroll list fought the list's scroll on
 * device with no reliable disambiguation, so brightness is discrete taps and a
 * drag always scrolls. Each −/+ tap steps by FF_SETTINGS_BRIGHT_STEP and COMMITS
 * (persisted; no transient/commit split for discrete taps — that split, and the
 * NVS-coalescing, is covered at the shell level by test_intent.c's bug1 tests).
 * Found by the pill's "-"/"+" label like every other pill test. */
static void S100_settings_brightness_stepper_steps_and_clamps(void)
{
    ff_app_settings_t s;
    memset(&s, 0, sizeof(s));
    s.brightness_pct = 70;

    ff_scr_settings_build(lv_screen_active(), &s);

    lv_obj_t *minus = find_button_with_label(lv_screen_active(), "-");
    lv_obj_t *plus = find_button_with_label(lv_screen_active(), "+");
    TEST_ASSERT_NOT_NULL(minus);
    TEST_ASSERT_NOT_NULL(plus);

    click(minus);
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_SETTING_SET, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_SETTING_BRIGHTNESS, s_spy.last.u.setting.id);
    TEST_ASSERT_EQUAL_INT32(60, s_spy.last.u.setting.v.i); /* 70 - 10% step */
    TEST_ASSERT_FALSE(s_spy.last.u.setting.transient); /* committed, not a live preview */

    click(plus);
    TEST_ASSERT_EQUAL_INT32(70, s_spy.last.u.setting.v.i);

    /* Clamps at the floor: many − taps settle at MIN and never below. */
    for (int i = 0; i < 20; i++) {
        click(minus);
    }
    TEST_ASSERT_EQUAL_INT32((int32_t)FF_BRIGHTNESS_MIN_PCT, s_spy.last.u.setting.v.i);
}

/* S21 §3 — the "CALIBRATE TOUCH" row emits the shell-owned
 * FF_INTENT_CALIBRATE_TOUCH (the screen only reports the tap; the shell runs
 * the device crosshair flow via its injected hook, a no-op on the sim). The
 * row is far down the scrolling list, but click() injects the event
 * directly, so its scroll position is irrelevant to this seam test. */
static void S21_settings_calibrate_touch_row_emits_calibrate_intent(void)
{
    ff_app_settings_t s;
    memset(&s, 0, sizeof(s));

    ff_scr_settings_build(lv_screen_active(), &s);

    click(find_button_with_label(lv_screen_active(), "CALIBRATE TOUCH"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_CALIBRATE_TOUCH, s_spy.last.kind);
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
    /* The inbox's `+` FAB is a wired emit site, always present on the
     * face (S24 — the S22 RALLY action button this used to click is gone
     * with its screen). Found by its 112px corner-bleed tap-target size;
     * the FAB's glyph is deco, not a button label. */
    click(find_clickable_by_size(lv_screen_active(), 112, 112));

    TEST_ASSERT_EQUAL_INT(0, s_spy.count); /* nothing reached the (unbound) spy — and nothing crashed */
}

/* =================================================================== */
/* S26 slice e — the launcher's four circles -> LAUNCHER_SELECT          */
/* =================================================================== */

static void S26e_launcher_circle_click_emits_launcher_select(int circle_idx, uint8_t expect_idx)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    ff_scr_launcher_build(&state);

    click(launcher_circle_at(circle_idx));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_LAUNCHER_SELECT, s_spy.last.kind);
    TEST_ASSERT_EQUAL_UINT8(expect_idx, s_spy.last.u.launcher_idx);
}

static void S26e_launcher_now_circle_emits_index_0(void)
{
    S26e_launcher_circle_click_emits_launcher_select(0, 0u);
}

static void S26e_launcher_signals_circle_emits_index_1(void)
{
    S26e_launcher_circle_click_emits_launcher_select(1, 1u);
}

static void S26e_launcher_map_circle_emits_index_2(void)
{
    S26e_launcher_circle_click_emits_launcher_select(2, 2u);
}

static void S26e_launcher_settings_circle_emits_index_3(void)
{
    S26e_launcher_circle_click_emits_launcher_select(3, 3u);
}

/* Each click must produce EXACTLY ONE intent (same discipline this
 * file's header comment states for every other wired site) — a control
 * double-registered on two callbacks would pass a kind-only check while
 * double-dispatching every tap. */
static void S26e_launcher_click_emits_exactly_one_intent(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    ff_scr_launcher_build(&state);

    click(launcher_circle_at(1)); /* Signals */
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S26e_AC3_horizontal_drag_on_radar_emits_nothing);
    RUN_TEST(S26e_AC3_horizontal_drag_on_now_emits_nothing);
    RUN_TEST(S26e_AC3_horizontal_drag_on_signals_emits_nothing);
    RUN_TEST(carousel_physical_upward_drag_emits_nothing);
    RUN_TEST(carousel_physical_upward_drag_emits_nothing_from_signals_too);
    RUN_TEST(carousel_physical_downward_drag_emits_nothing);
    RUN_TEST(S16_c1_compose_back_emits_back);
    RUN_TEST(S16_c2_compose_send_emits_send_text);
    RUN_TEST(S16_c3_abc_letter_key_emits_t9_key);
    RUN_TEST(S16_c3_abc_space_key_emits_t9_space);
    RUN_TEST(S16_c3_del_key_emits_t9_backspace);
    RUN_TEST(S16_c3_123_digit_key_emits_t9_insert);
    RUN_TEST(S16_c3_sym_symbol_key_emits_t9_insert);
    RUN_TEST(S16_c3_sym_space_key_emits_t9_space);
    RUN_TEST(S16_c3_mode_chip_emits_t9_mode);
    RUN_TEST(S24b_inbox_member_row_tap_emits_open_thread);
    RUN_TEST(S24b_inbox_crew_row_tap_emits_open_thread_crew);
    RUN_TEST(S24b_inbox_quiet_member_row_is_tappable);
    RUN_TEST(S24b_inbox_fab_emits_inbox_new);
    RUN_TEST(S24b_picker_rows_emit_pick);
    RUN_TEST(S24b_picker_and_thread_back_emit_back);
    RUN_TEST(S24c_thread_omw_chip_emits_canned_reply_omw);
    RUN_TEST(S24c_thread_in5min_chip_emits_canned_reply_5min);
    RUN_TEST(S24c_thread_flare_chip_emits_sig_flare);
    RUN_TEST(S24c_thread_fab_emits_inbox_new);
    RUN_TEST(S24c_crew_thread_has_no_quick_chips);
    RUN_TEST(S24d_popup_compose_row_emits_popup_compose);
    RUN_TEST(S24d_popup_rally_row_emits_popup_rally);
    RUN_TEST(S24d_popup_flare_row_emits_popup_flare);
    RUN_TEST(S24d_popup_close_emits_back);
    RUN_TEST(S24d_rally_on_me_row_emits_select_place_zero);
    RUN_TEST(S24d_rally_landmark_row_emits_select_place_index);
    RUN_TEST(S24d_rally_when_chip_emits_cycle_when);
    RUN_TEST(S24d_rally_send_button_emits_rally_send);
    RUN_TEST(S24d_rally_disabled_on_me_is_not_tappable);
    RUN_TEST(S16_c2_radar_flare_button_emits_flare_start);
    RUN_TEST(S17a_AC4_radar_precise_dot_renders_filled_with_its_initial);
    RUN_TEST(S17a_AC4_radar_imprecise_dot_renders_as_hollow_ring_with_no_initial);
    RUN_TEST(S16_c2_flare_takeover_go_emits_takeover_go);
    RUN_TEST(S16_c2_flare_takeover_dismiss_emits_takeover_dismiss);
    RUN_TEST(S16_c2_sender_overlay_cancel_emits_flare_end);
    RUN_TEST(S11b_settings_units_chip_toggles_imperial);
    RUN_TEST(S11b_settings_share_chip_cycles_live_to_ghost_skipping_zones);
    RUN_TEST(S11b_settings_share_chip_from_ghost_and_from_zones_both_avoid_zones);
    RUN_TEST(S11b_settings_haptics_chip_toggles);
    RUN_TEST(S11b_settings_night_glow_chip_toggles);
    RUN_TEST(S11b_settings_water_chip_cycles_presets);
    RUN_TEST(S11b_settings_water_label_tap_also_cycles);
    RUN_TEST(S11b_settings_quiet_chip_sets_both_from_and_to);
    RUN_TEST(S100_settings_brightness_stepper_steps_and_clamps);
    RUN_TEST(S21_settings_calibrate_touch_row_emits_calibrate_intent);
    RUN_TEST(S16_c1_wired_sites_are_noops_while_the_seam_is_unbound);

    RUN_TEST(S26e_launcher_now_circle_emits_index_0);
    RUN_TEST(S26e_launcher_signals_circle_emits_index_1);
    RUN_TEST(S26e_launcher_map_circle_emits_index_2);
    RUN_TEST(S26e_launcher_settings_circle_emits_index_3);
    RUN_TEST(S26e_launcher_click_emits_exactly_one_intent);

    return UNITY_END();
}
