/**
 * test_scr_intent.c — headless LVGL INTERACTION tests for every wired
 * emit site.
 *
 * S16 slice c1 wired the three navigation-only stubs: nav long-press
 * (-> OPEN_SETTINGS), Compose back "<" (-> BACK), Signals "+" (->
 * OPEN_COMPOSE). Slice c2 wires the core-mutating ones: Compose SEND (->
 * SEND_TEXT), the Signals rally-row tap (-> SELECT_RALLY) and OMW/5 MIN
 * chips (-> CANNED_REPLY; a third PULSE chip existed here before its
 * 2026-09-02 retirement, see ff_intent.h's header note), the Radar-face
 * CLOSE-mode FLARE button (-> FLARE_START), and the S10 takeover/sender-overlay buttons GO/
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
 * inbox_open_compose_cb's emitted kinds fails both of their tests on
 * the kind assertion; removing an `ff_intent_emit` call entirely fails
 * on count == 0. Swapping flare_go_cb's/flare_dismiss_takeover_cb's
 * emitted kinds (the exact PR #20 regression class, now one layer up)
 * fails both GO/DISMISS tests on the kind assertion the same way.
 */
#include <math.h> /* sqrtf — the S99 compose SEND corner-distance test */
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
#include "scr_power_menu.h" /* PL_ press-lock drag-off tests — the power menu's own PRESS_LOCK coverage */
#include "scr_radar.h"
#include "scr_settings.h"
#include "scr_inbox.h"

#include "ff_theme.h" /* FF_THEME_PUCK_RADIUS_PX — the S99 compose SEND corner-distance test */
#include "radar_layout.h" /* RADAR_LAYOUT_DOT_PX — S17a AC4 render tests, see that section below */
#include "ff_theme.h" /* FF_THEME_FONT_MSG_BODY — S24 thread-bubble-not-compressed test */

/* setUp/tearDown, the spy sink, the frozen-but-advanceable tick, the
 * label/button tree-search helpers, and click()/drag()/drag_v()/tap_at()
 * (including the one real hazard in the last two — the LVGL indev
 * scroll-throw-animation use-after-free) now live in ONE shared header
 * (debt/test-naming-harness), used by this file and test_scr_banner.c —
 * see support/lv_test_harness.h's own top comment for the extraction
 * rationale and the workaround's full derivation. */
#include "support/lv_test_harness.h"

void setUp(void)
{
    ff_test_lv_setup(412);
}

void tearDown(void)
{
    ff_test_lv_teardown();
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
/* of it, same as a vertical drag always was). drag()/drag_v()/tap_at() */
/* are the shared header's synthetic-pointer-indev helpers.             */
/* =================================================================== */

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

static void S26e_AC3_horizontal_drag_on_lineup_emits_nothing(void)
{
    S26e_AC3_horizontal_drag_emits_nothing(FF_APP_FACE_LINEUP);
}

static void S26e_AC3_horizontal_drag_on_inbox_emits_nothing(void)
{
    S26e_AC3_horizontal_drag_emits_nothing(FF_APP_FACE_INBOX);
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

static void carousel_physical_upward_drag_emits_nothing_from_inbox_too(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    state.active_face = FF_APP_FACE_INBOX;
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
/* Compose SEND relocation (maintainer: "move SEND away from SPACE to   */
/* avoid accidental press") + full-key press-state / full-shape         */
/* hit-area audit.                                                      */
/*                                                                       */
/* `click()`'s direct LV_EVENT_CLICKED injection (every test above)     */
/* proves a control is WIRED to the right intent, but not that its      */
/* whole visible area is genuinely tappable, nor that it sits where a   */
/* real thumb would land relative to its neighbors — a label-only hit   */
/* trap or a too-close SPACE/SEND pair would pass every `click()` test   */
/* in this file unchanged. `tap_at()` (defined above, real synthetic    */
/* indev press/release through `lv_indev_search_obj`'s own hit-testing) */
/* is what proves those, the same reasoning S24's real-touch OMW test   */
/* already established for Signals.                                    */
/* =================================================================== */

static void S99_compose_tap_on_space_center_emits_space_never_send(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose));
    ff_scr_compose_build(&compose);
    lv_obj_update_layout(lv_screen_active()); /* coords are lazily computed — force it before reading any */

    lv_obj_t *space = find_button_with_label(lv_screen_active(), "SPACE");
    TEST_ASSERT_NOT_NULL(space);
    lv_area_t a;
    lv_obj_get_coords(space, &a);

    tap_at((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_T9_SPACE, s_spy.last.kind);
}

/* A real physical tap anywhere on SEND's full visible rect — center and
 * all four corners — must resolve to SEND and ONLY SEND, exactly once
 * per tap. This is the test the mutation check below (shrinking SEND's
 * hit area to its label) is written to catch: a label-only hit trap
 * would still pass a center tap but miss every corner. */
static void S99_compose_send_full_area_tap_emits_send_exactly_once(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose));
    ff_scr_compose_build(&compose);
    lv_obj_update_layout(lv_screen_active()); /* coords are lazily computed — force it before reading any */

    lv_obj_t *send = find_button_with_label(lv_screen_active(), "SEND");
    TEST_ASSERT_NOT_NULL(send);
    lv_area_t a;
    lv_obj_get_coords(send, &a);

    /* 1px inset from the true boundary: indev coordinates address pixel
     * centers, so a tap exactly ON the last-pixel boundary is an
     * off-by-one hazard unrelated to what this test is proving (the
     * WHOLE visible area, corners included, is real estate — not just
     * the geometric center). */
    int32_t const pts[5][2] = {
        {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2}, /* center */
        {a.x1 + 1, a.y1 + 1},                   /* top-left corner */
        {a.x2 - 1, a.y1 + 1},                   /* top-right corner */
        {a.x1 + 1, a.y2 - 1},                   /* bottom-left corner */
        {a.x2 - 1, a.y2 - 1},                   /* bottom-right corner */
    };
    char const *const names[5] = {"center", "top-left", "top-right", "bottom-left", "bottom-right"};

    for (int i = 0; i < 5; i++) {
        memset(&s_spy, 0, sizeof(s_spy));
        tap_at(pts[i][0], pts[i][1]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_spy.count, names[i]);
        TEST_ASSERT_EQUAL_MESSAGE(FF_INTENT_SEND_TEXT, s_spy.last.kind, names[i]);
    }
}

/* Direct geometric proof of the maintainer's own ask ("at least one
 * key-width, or a >=12px dead gap, between SPACE and SEND") — belt and
 * suspenders alongside test_face_hit_targets.c's generic 8px adjacency
 * sweep, which this pair clears by over 20x (see this test's own
 * assertion message for the measured number). */
static void S99_compose_space_and_send_clear_the_maintainer_gap_ask(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose));
    ff_scr_compose_build(&compose);
    lv_obj_update_layout(lv_screen_active()); /* coords are lazily computed — force it before reading any */

    lv_obj_t *space = find_button_with_label(lv_screen_active(), "SPACE");
    lv_obj_t *send = find_button_with_label(lv_screen_active(), "SEND");
    TEST_ASSERT_NOT_NULL(space);
    TEST_ASSERT_NOT_NULL(send);

    lv_area_t sa, se;
    lv_obj_get_coords(space, &sa);
    lv_obj_get_coords(send, &se);

    /* SEND sits in the header, well above SPACE at the bottom of the
     * keypad — they do not even overlap on the x-axis, so the vertical
     * edge-to-edge gap alone is the right quantity here. */
    TEST_ASSERT_TRUE_MESSAGE(se.y2 < sa.y1, "SEND must be entirely above SPACE (no shared row)");
    int32_t gap_y = sa.y1 - se.y2;
    TEST_ASSERT_GREATER_OR_EQUAL_INT32_MESSAGE(12, gap_y,
                                               "SEND must clear SPACE by >= 12px (maintainer ask); see test output "
                                               "for the actual measured gap");
}

/* has_press_feedback — true iff `obj` registers a LOCAL style rule for
 * (LV_PART_MAIN | LV_STATE_PRESSED) on either bg_opa or bg_color — the
 * two properties every press treatment in this file's target
 * (compose_key_press_feedback, and SEND's own dim-on-press) actually
 * sets. This is a SELECTOR query (lv_obj_has_style_prop with an
 * explicit part|state pair), not a "what does this look like right
 * now" query — it works without the object ever actually being
 * pressed, so a headless sweep like the one below can check every
 * control in one pass instead of driving a real press on each. */
static bool has_press_feedback(lv_obj_t *obj)
{
    return lv_obj_has_style_prop(obj, LV_PART_MAIN | LV_STATE_PRESSED, LV_STYLE_BG_OPA) ||
           lv_obj_has_style_prop(obj, LV_PART_MAIN | LV_STATE_PRESSED, LV_STYLE_BG_COLOR);
}

/* walk_assert_press_feedback — recursively asserts every CLICKABLE
 * object in the tree rooted at `obj` carries press-state feedback
 * (S24 AC7 / the device-polish "presses must register" convention every
 * control in this file follows), incrementing *out_n for each one
 * checked so the caller can rule out a vacuous (zero-controls-found)
 * pass, same discipline test_face_hit_targets.c's own sweep uses. */
static void walk_assert_press_feedback(lv_obj_t *obj, int *out_n)
{
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) {
            (*out_n)++;
            TEST_ASSERT_TRUE_MESSAGE(has_press_feedback(child),
                                      "every clickable compose control must carry an LV_STATE_PRESSED style");
        }
        walk_assert_press_feedback(child, out_n);
    }
}

/* Every key/button on every keypad page (ABC/123/SYM; PRED is covered by
 * the dedicated test right below, since it also swaps in the candidate
 * chips) must have press-state feedback — the full audit request, not
 * just SEND/SPACE. */
static void S99_compose_every_key_has_press_state_feedback(void)
{
    ff_app_compose_mode_t const modes[] = {FF_APP_COMPOSE_ABC, FF_APP_COMPOSE_123, FF_APP_COMPOSE_SYM};
    for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        lv_obj_clean(lv_screen_active());
        ff_app_compose_t compose;
        memset(&compose, 0, sizeof(compose));
        compose.mode = modes[m];
        ff_scr_compose_build(&compose);

        int checked = 0;
        walk_assert_press_feedback(lv_screen_active(), &checked);
        TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, checked, "swept zero clickable controls — test is vacuous");
    }
}

/* PRED mode's candidate chips (including the selected/amber chip's
 * dim-on-press variant and the trailing "more" chip) are a separate
 * render path from the ABC/123/SYM keypad — swept on their own fixture
 * so the audit actually covers them, not just the shared keypad. */
static void S99_compose_pred_candidates_have_press_state_feedback(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose));
    compose.mode = FF_APP_COMPOSE_PRED;
    strncpy(compose.text, "omw to ", sizeof(compose.text) - 1);
    strncpy(compose.word, "the", sizeof(compose.word) - 1);
    compose.n_cand = 3;
    compose.total_cand = 5; /* > n_cand, so the trailing "more" chip renders too */
    strncpy(compose.cand[0].text, "the", sizeof(compose.cand[0].text) - 1);
    strncpy(compose.cand[1].text, "tie", sizeof(compose.cand[1].text) - 1);
    strncpy(compose.cand[2].text, "vie", sizeof(compose.cand[2].text) - 1);
    compose.sel_cand = 0;

    ff_scr_compose_build(&compose);

    int checked = 0;
    walk_assert_press_feedback(lv_screen_active(), &checked);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, checked, "swept zero clickable controls — test is vacuous");
}

/* =================================================================== */
/* Compose drag-off (PR #148 review, FAIL 1): LVGL's default             */
/* LV_OBJ_FLAG_PRESS_LOCK keeps an object "pressed" — and still fires    */
/* CLICKED on release — even after a real touch has slid off it. A       */
/* finger that presses SEND (or any key) and drags away before lifting   */
/* must never commit that key; compose_clear_press_lock (the #145        */
/* launcher-lesson fix, scr_launcher.c) clears the flag on every         */
/* clickable control in this file. Both tests drag through a REAL        */
/* synthetic indev (drag_v — LVGL's own press/move/release path, the     */
/* same mechanism a real finger drives), not a synthetic CLICKED event:  */
/* only a real drag can exercise PRESS_LOCK at all.                      */
/* =================================================================== */

static void S99_compose_drag_off_send_emits_nothing(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose));
    ff_scr_compose_build(&compose);
    lv_obj_update_layout(lv_screen_active()); /* coords are lazily computed — force it before reading any */

    lv_obj_t *send = find_button_with_label(lv_screen_active(), "SEND");
    TEST_ASSERT_NOT_NULL(send);
    lv_area_t a;
    lv_obj_get_coords(send, &a);
    int32_t cx = (a.x1 + a.x2) / 2;
    int32_t cy = (a.y1 + a.y2) / 2;

    /* Press on SEND's own center, drag straight down 150px (>= the
     * review's 100px ask — comfortably off SEND, which is only 44px
     * tall), release far away, never back on SEND. */
    drag_v(cy, cy + 150, cx);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_spy.count, "a slide-off of SEND must never commit SEND_TEXT");
}

/* Same proof for an ordinary T9 key (DEF, key 3) — the review's "and the
 * same for a T9 key" ask, not just SEND. */
static void S99_compose_drag_off_key_emits_nothing(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose));
    ff_scr_compose_build(&compose);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *def = find_button_with_label(lv_screen_active(), "DEF");
    TEST_ASSERT_NOT_NULL(def);
    lv_area_t a;
    lv_obj_get_coords(def, &a);
    int32_t cx = (a.x1 + a.x2) / 2;
    int32_t cy = (a.y1 + a.y2) / 2;

    drag_v(cy, cy + 150, cx);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_spy.count, "a slide-off of a T9 key must never commit T9_KEY");
}

/* =================================================================== */
/* Compose SEND corner-distance (PR #148 review, should-fix 3): SEND's   */
/* farthest corner must sit within (radius - safety) = 196px of the      */
/* puck's own center — the true 2D Euclidean bezel-margin bar, not just  */
/* inside the raw 206px circle (which test_face_hit_targets.c's sweep    */
/* already asserts) and not just safe along its own row's horizontal     */
/* chord (compose_safe_margin_x's guarantee, which is a DIFFERENT,       */
/* weaker quantity for a corner point — see compose_send_x's own         */
/* comment in scr_compose.c for why).                                    */
/* =================================================================== */

static void S99_compose_send_corner_clears_bezel_margin_bar(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose));
    ff_scr_compose_build(&compose);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *send = find_button_with_label(lv_screen_active(), "SEND");
    TEST_ASSERT_NOT_NULL(send);
    lv_area_t a;
    lv_obj_get_coords(send, &a);

    /* Puck center (window == puck, no margin — ff_theme.h). Check all
     * four corners; the farthest one is what matters, but checking all
     * four is what actually proves it rather than assuming which one. */
    float const cx = (float)FF_THEME_PUCK_RADIUS_PX;
    float const cy = (float)FF_THEME_PUCK_RADIUS_PX;
    float const safe_r = (float)FF_THEME_PUCK_RADIUS_PX - 10.0f; /* FF_COMPOSE_SAFETY_PX, scr_compose.c */
    float const corners_x[4] = {(float)a.x1, (float)a.x2, (float)a.x1, (float)a.x2};
    float const corners_y[4] = {(float)a.y1, (float)a.y1, (float)a.y2, (float)a.y2};
    float max_dist = 0.0f;
    for (int i = 0; i < 4; i++) {
        float dx = corners_x[i] - cx;
        float dy = corners_y[i] - cy;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist > max_dist) max_dist = dist;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "SEND's farthest corner measures %.1fpx from center (bar: %.1fpx)", (double)max_dist,
              (double)safe_r);
    TEST_ASSERT_LESS_OR_EQUAL_FLOAT_MESSAGE(safe_r, max_dist, msg);
}

/* =================================================================== */
/* Compose TO readability (PR #148 review round 3, blocking): round 2's  */
/* fix bounded TO to the ~52px BACK-to-SEND header gap, which made even  */
/* an ORDINARY short name unreadable ("TO: DANA" -> "TO: D..."). TO now  */
/* lives on its own row below the header, wide enough for every demo    */
/* crew name in full, while a genuinely long one still ellipsizes.      */
/* =================================================================== */

/* find_label_with_prefix — like find_button_with_label, but for a bare
 * (non-button) label whose text starts with `prefix` — TO isn't a
 * button, so that helper (which only descends into lv_button_class
 * children) can't find it. */
static lv_obj_t *find_label_with_prefix(lv_obj_t *root, char const *prefix)
{
    uint32_t n = lv_obj_get_child_count(root);
    size_t plen = strlen(prefix);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            char const *txt = lv_label_get_text(child);
            if (txt != NULL && strncmp(txt, prefix, plen) == 0) {
                return child;
            }
        }
        lv_obj_t *found = find_label_with_prefix(child, prefix);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

/* compose_mid.json's ordinary short recipient (DANA, a real demo crew
 * name) must render in FULL — no ellipsis at all. LVGL's
 * LV_LABEL_LONG_MODE_DOTS mutates the label's OWN stored text (not just
 * how it paints) when it truncates, so checking lv_label_get_text() for
 * the absence of the "..." it appends is a direct, sufficient proof —
 * not an approximation of what the pixels show. */
static void S99_compose_to_short_name_renders_in_full_no_dots(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose));
    strncpy(compose.to_name, "DANA", sizeof(compose.to_name) - 1);

    ff_scr_compose_build(&compose);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *to = find_label_with_prefix(lv_screen_active(), "TO:");
    TEST_ASSERT_NOT_NULL_MESSAGE(to, "TO label not found");
    char const *txt = lv_label_get_text(to);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("TO: DANA", txt, "a real demo crew name must render in full");
    TEST_ASSERT_NULL_MESSAGE(strstr(txt, "..."), "a short, ordinary recipient name must never ellipsize");
}

/* Every other demo crew name (+ EVERYONE, the broadcast fallback) must
 * also render in full — not just DANA. */
static void S99_compose_to_every_demo_crew_name_renders_in_full(void)
{
    char const *const names[] = {"DANA", "KEV", "RILEY", "MAYA", "SAM", "EVERYONE"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        lv_obj_clean(lv_screen_active());
        ff_app_compose_t compose;
        memset(&compose, 0, sizeof(compose));
        if (strcmp(names[i], "EVERYONE") != 0) {
            strncpy(compose.to_name, names[i], sizeof(compose.to_name) - 1);
        } /* else: leave to_name empty — the code's own "EVERYONE" fallback */

        ff_scr_compose_build(&compose);
        lv_obj_update_layout(lv_screen_active());

        lv_obj_t *to = find_label_with_prefix(lv_screen_active(), "TO:");
        TEST_ASSERT_NOT_NULL_MESSAGE(to, names[i]);
        char const *txt = lv_label_get_text(to);
        TEST_ASSERT_NULL_MESSAGE(strstr(txt, "..."), names[i]);
    }
}

/* compose_to_long.json's genuinely long recipient ("WHOLE CREW") must
 * still ellipsize cleanly — the DOTS path stays provably reachable, not
 * dead code now that real names all fit. */
static void S99_compose_to_long_name_ellipsizes_cleanly(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose));
    strncpy(compose.to_name, "WHOLE CREW", sizeof(compose.to_name) - 1);

    ff_scr_compose_build(&compose);
    lv_obj_update_layout(lv_screen_active()); /* DOTS truncation is computed during layout, not at label_set_text */

    lv_obj_t *to = find_label_with_prefix(lv_screen_active(), "TO:");
    TEST_ASSERT_NOT_NULL(to);
    char const *txt = lv_label_get_text(to);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(txt, "..."), "a genuinely long recipient name must ellipsize, not overflow");
}

/* PRED mode has no separate TO row (see scr_compose.c's own comment,
 * "TO — PR #148 review round 3") — the recipient is prepended to the
 * draft line itself instead. Prove it's actually there (and the
 * dedicated TO row is NOT double-built at the same y, the bug an
 * earlier draft of this fix shipped). */
static void S99_compose_pred_mode_shows_recipient_on_draft_line(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose));
    compose.mode = FF_APP_COMPOSE_PRED;
    strncpy(compose.to_name, "DANA", sizeof(compose.to_name) - 1);

    ff_scr_compose_build(&compose);

    lv_obj_t *prefix = find_label_with_prefix(lv_screen_active(), "DANA:");
    TEST_ASSERT_NOT_NULL_MESSAGE(prefix, "PRED draft line must carry the recipient prefix");
    /* And the dedicated TO row (which would sit at the identical y) must
     * NOT also exist — that's the double-render bug this test guards. */
    lv_obj_t *to_row = find_label_with_prefix(lv_screen_active(), "TO:");
    TEST_ASSERT_NULL_MESSAGE(to_row, "PRED mode must not ALSO build the dedicated TO row at the same y");
}

/* =================================================================== */
/* Compose device follow-up 2 ("the keyboard's SPACE, DEL and T9 (mode)  */
/* buttons are too small — make them a bit larger" + "the T9 autocomplete*/
/* words should be horizontally scrollable if possible"): the relocated  */
/* header MODE chip, the bigger DEL/SPACE, and the PRED candidate row's  */
/* new horizontal scroll.                                                */
/* =================================================================== */

/* Every dimension asserted below is the LITERAL measured pixel number
 * this PR's own header comment (scr_compose.c) claims — not a
 * re-derivation of the same formula the production code uses, which
 * would pass vacuously even if the underlying design regressed (the
 * proxy-check rule, docs/review/code-review.md item 6). Mutation-verified
 * (fresh build, object hash confirmed changed): reverting
 * FF_COMPOSE_DEL_W to its pre-this-PR 52 recomputes SPACE's width to 104
 * (not 92) and fails the SPACE assertion below — see this PR's body for
 * the exact `ctest` output. SYM mode is used throughout this section
 * (not the default ABC) purely so "DEL"/"SPACE"/"SYM" are each
 * unambiguous find_button_with_label lookups — ABC mode's grid carries
 * its own "ABC"-legended key (row0, key 2) alongside the mode chip's own
 * "ABC" label, and since the mode chip now sits EARLIER in the tree
 * (the header, built before the keypad) it would still resolve correctly
 * by depth-first order, but SYM sidesteps relying on that order at all. */
static void S99_compose_space_del_mode_pinned_dimensions(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose));
    compose.mode = FF_APP_COMPOSE_SYM;
    ff_scr_compose_build(&compose);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *del = find_button_with_label(lv_screen_active(), "DEL");
    lv_obj_t *space = find_button_with_label(lv_screen_active(), "SPACE");
    lv_obj_t *mode = find_button_with_label(lv_screen_active(), "SYM");
    TEST_ASSERT_NOT_NULL(del);
    TEST_ASSERT_NOT_NULL(space);
    TEST_ASSERT_NOT_NULL(mode);

    lv_area_t da, sa, ma;
    lv_obj_get_coords(del, &da);
    lv_obj_get_coords(space, &sa);
    lv_obj_get_coords(mode, &ma);

    TEST_ASSERT_EQUAL_INT32_MESSAGE(64, da.x2 - da.x1 + 1, "DEL width pinned to 64px");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(56, da.y2 - da.y1 + 1, "DEL height pinned to 56px (matches the digit keys)");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(92, sa.x2 - sa.x1 + 1, "SPACE width pinned to 92px");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(56, sa.y2 - sa.y1 + 1, "SPACE height pinned to 56px (matches the digit keys)");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(51, ma.x2 - ma.x1 + 1, "MODE header-slot width pinned to 51px");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(44, ma.y2 - ma.y1 + 1, "MODE header-slot height pinned to 44px");
}

/* MODE's relocated header slot: same header row as BACK/SEND, clearing
 * both the 44px hit floor and the FF_HIT_MIN_GAP_PX adjacency floor
 * against its new neighbors on both sides — the semantic (floor-relative,
 * not exact-value) counterpart to the pinned-dimensions test above. */
static void S99_compose_mode_header_chip_clears_floor_and_gaps(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose));
    compose.mode = FF_APP_COMPOSE_SYM;
    ff_scr_compose_build(&compose);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *back = find_button_with_label(lv_screen_active(), "<");
    lv_obj_t *send = find_button_with_label(lv_screen_active(), "SEND");
    lv_obj_t *mode = find_button_with_label(lv_screen_active(), "SYM");
    TEST_ASSERT_NOT_NULL(back);
    TEST_ASSERT_NOT_NULL(send);
    TEST_ASSERT_NOT_NULL(mode);

    lv_area_t ba, sa, ma;
    lv_obj_get_coords(back, &ba);
    lv_obj_get_coords(send, &sa);
    lv_obj_get_coords(mode, &ma);

    TEST_ASSERT_EQUAL_INT32_MESSAGE(ba.y1, ma.y1, "MODE must sit in the SAME header row as BACK/SEND");
    int32_t w = ma.x2 - ma.x1 + 1;
    int32_t h = ma.y2 - ma.y1 + 1;
    TEST_ASSERT_GREATER_OR_EQUAL_INT32_MESSAGE(FF_THEME_MIN_HIT_PX, w, "MODE must clear the 44px hit floor");
    TEST_ASSERT_GREATER_OR_EQUAL_INT32_MESSAGE(FF_THEME_MIN_HIT_PX, h, "MODE must clear the 44px hit floor");
    int32_t gap_back = ma.x1 - ba.x2 - 1;
    int32_t gap_send = sa.x1 - ma.x2 - 1;
    TEST_ASSERT_GREATER_OR_EQUAL_INT32_MESSAGE(FF_HIT_MIN_GAP_PX, gap_back, "MODE must clear BACK by the adjacency floor");
    TEST_ASSERT_GREATER_OR_EQUAL_INT32_MESSAGE(FF_HIT_MIN_GAP_PX, gap_send, "MODE must clear SEND by the adjacency floor");
}

/* Six real candidates (matching tests/fixtures/compose_pred_scroll.json)
 * whose combined chip width genuinely overflows the strip's own
 * chord-derived viewport at this Y (measured: ~377px of chips vs. ~320px
 * actually safe here) — the exact shape needed to exercise real
 * scrolling, not just wire the mechanism without ever triggering it. */
static void compose_fill_six_candidates(ff_app_compose_t *compose)
{
    static char const *const words[6] = {"the", "tie", "vie", "side", "ride", "wide"};
    memset(compose, 0, sizeof(*compose));
    compose->mode = FF_APP_COMPOSE_PRED;
    strncpy(compose->to_name, "DANA", sizeof(compose->to_name) - 1);
    strncpy(compose->text, "omw to ", sizeof(compose->text) - 1);
    strncpy(compose->word, "the", sizeof(compose->word) - 1);
    compose->n_cand = 6;
    compose->total_cand = 6;
    for (int i = 0; i < 6; i++) {
        strncpy(compose->cand[i].text, words[i], sizeof(compose->cand[i].text) - 1);
    }
    compose->sel_cand = 0;
}

/* find_pred_strip — the PRED candidate strip lv_obj itself (the scrolling
 * container), found by locating a known chip by label and stepping up to
 * its parent. */
static lv_obj_t *find_pred_strip(lv_obj_t *root, char const *first_chip_label)
{
    lv_obj_t *chip = find_button_with_label(root, first_chip_label);
    if (chip == NULL) return NULL;
    return lv_obj_get_parent(chip);
}

/* tap_settled — a single press-then-release with no intermediate reads,
 * for tapping a control that sits INSIDE a container already scrolled to
 * a non-zero offset (a programmatic lv_obj_scroll_to_x, not a drag).
 * Measured directly (this test file, in development): the shared
 * tap_at()/drag_v() (support/lv_test_harness.h) samples the SAME
 * stationary point six times before releasing, which works everywhere
 * else in this file (ordinary, unscrolled controls) but reliably drops
 * the click specifically when the target's container already sits at
 * scroll_x > 0 — a single press+release at the identical coordinates
 * fires the click correctly every time. Scoped local to this file rather
 * than changed in the shared harness (used by test_scr_banner.c too, and
 * every other current caller already passes with the six-step version)
 * — this is the minimal, targeted fix for the one new case this PR
 * introduces. */
static void tap_settled(int32_t x, int32_t y)
{
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, probe_read_cb);

    s_probe_pt.x = (lv_coord_t)x;
    s_probe_pt.y = (lv_coord_t)y;
    s_probe_state = LV_INDEV_STATE_PRESSED;
    s_fake_tick_ms += 40u;
    lv_timer_handler();

    s_probe_state = LV_INDEV_STATE_RELEASED;
    s_fake_tick_ms += 40u;
    lv_timer_handler();

    ff_test_release_probe_indev(indev);
}

/* A horizontal drag that starts on a real candidate chip and crosses at
 * least two others must scroll the strip — and must NEVER select a
 * candidate, the same PRESS_LOCK-cleared drag-off proof
 * S99_compose_drag_off_key_emits_nothing already gives the T9 grid,
 * applied here to a genuine scroll gesture instead of a slide-off-and-
 * release. Drags from "side" (chip index 3) leftward past "the" (index
 * 0)'s own original left edge — crossing "vie" (2) and "tie" (1) along
 * the way, i.e. "across two chips" — which also scrolls the strip
 * FORWARD (finger moves left -> content shifts left -> scroll_x
 * increases), the same direction convention every other horizontal list
 * a real thumb swipes uses. */
static void S99_compose_pred_strip_drag_scrolls_not_selects(void)
{
    ff_app_compose_t compose;
    compose_fill_six_candidates(&compose);
    ff_scr_compose_build(&compose);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *strip = find_pred_strip(lv_screen_active(), "the");
    TEST_ASSERT_NOT_NULL(strip);
    lv_obj_t *chip_the = find_button_with_label(lv_screen_active(), "the");
    lv_obj_t *chip_side = find_button_with_label(lv_screen_active(), "side");
    TEST_ASSERT_NOT_NULL(chip_the);
    TEST_ASSERT_NOT_NULL(chip_side);

    lv_area_t a_the, a_side;
    lv_obj_get_coords(chip_the, &a_the);
    lv_obj_get_coords(chip_side, &a_side);
    int32_t y = (a_side.y1 + a_side.y2) / 2;
    int32_t start_x = (a_side.x1 + a_side.x2) / 2;
    int32_t end_x = a_the.x1 - 40; /* comfortably left of "the"'s own original edge */

    drag(start_x, end_x, y);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_spy.count, "a horizontal drag across candidate chips must never select one");
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(0, lv_obj_get_scroll_x(strip),
                                           "the same drag must actually scroll the candidate strip");
}

/* After scrolling, a tap on a chip that is now on-screen must select
 * exactly that candidate — proves the strip stays genuinely tappable
 * per-chip post-scroll, not just that scrolling itself works. */
static void S99_compose_pred_strip_tap_after_scroll_selects_right_word(void)
{
    ff_app_compose_t compose;
    compose_fill_six_candidates(&compose);
    ff_scr_compose_build(&compose);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *strip = find_pred_strip(lv_screen_active(), "the");
    TEST_ASSERT_NOT_NULL(strip);

    /* lv_obj_get_scroll_x + lv_obj_get_scroll_right, the same "prove
     * genuine overflow, not just SOME scroll position" idiom S24's own
     * thread/Rally scroll tests already use. */
    int32_t max_scroll = lv_obj_get_scroll_x(strip) + lv_obj_get_scroll_right(strip);
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(0, max_scroll, "6 real candidates must genuinely overflow the strip");
    lv_obj_scroll_to_x(strip, max_scroll, LV_ANIM_OFF);
    lv_obj_update_layout(lv_screen_active());
    TEST_ASSERT_GREATER_THAN_INT32(0, lv_obj_get_scroll_x(strip));

    lv_obj_t *wide = find_button_with_label(lv_screen_active(), "wide");
    TEST_ASSERT_NOT_NULL_MESSAGE(wide, "the last candidate must still be reachable once scrolled fully into view");
    lv_area_t a;
    lv_obj_get_coords(wide, &a);
    tap_settled((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_spy.count, "a tap on a chip after scrolling must select exactly it");
    TEST_ASSERT_EQUAL_MESSAGE(FF_INTENT_T9_SELECT, s_spy.last.kind, "must emit T9_SELECT");
    TEST_ASSERT_EQUAL_INT_MESSAGE(5, s_spy.last.u.t9_key, "\"wide\" is candidate index 5 (0-based, 6 candidates)");
}

/* A new prediction set (a new keystroke, in shell terms) rebuilds this
 * screen from scratch — same "one screen built per process/frame"
 * convention this file's own top comment documents — so the candidate
 * row's scroll position must NOT carry over from whatever it was before:
 * the fresh strip starts at scroll_x == 0 by construction (a brand-new
 * lv_obj), which is what keeps the first candidate visible at the left
 * after every rebuild without any explicit reset code to get wrong. */
static void S99_compose_pred_strip_scroll_resets_on_new_prediction_set(void)
{
    ff_app_compose_t compose;
    compose_fill_six_candidates(&compose);
    ff_scr_compose_build(&compose);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *strip = find_pred_strip(lv_screen_active(), "the");
    TEST_ASSERT_NOT_NULL(strip);
    int32_t max_scroll = lv_obj_get_scroll_x(strip) + lv_obj_get_scroll_right(strip);
    TEST_ASSERT_GREATER_THAN_INT32(0, max_scroll);
    lv_obj_scroll_to_x(strip, max_scroll, LV_ANIM_OFF);
    TEST_ASSERT_GREATER_THAN_INT32(0, lv_obj_get_scroll_x(strip));

    /* Simulate the next keystroke's rebuild in-test — same clean+rebuild
     * shape S99_compose_every_key_has_press_state_feedback already uses
     * between modes — with a DIFFERENT in-progress selection. */
    lv_obj_clean(lv_screen_active());
    ff_app_compose_t compose2;
    compose_fill_six_candidates(&compose2);
    strncpy(compose2.word, "vie", sizeof(compose2.word) - 1);
    compose2.sel_cand = 2;
    ff_scr_compose_build(&compose2);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *strip2 = find_pred_strip(lv_screen_active(), "the");
    TEST_ASSERT_NOT_NULL(strip2);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(0, lv_obj_get_scroll_x(strip2),
                                    "a fresh prediction-set rebuild must start the candidate row scrolled to the left");
}

/* =================================================================== */
/* Signals (S24 slice b): the inbox face's navigation intents            */
/* =================================================================== */

/* Small helpers to hand-build an ff_app_inbox_t the screen renders —
 * the unit-test analog of the fixtures, so a test names exactly the
 * conversations it needs. */
static ff_inbox_conv_t *sig_add_conv(ff_app_inbox_t *v, ff_conv_kind_t kind, uint32_t node_id,
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
 * overlay button created as the row's FIRST child (scr_inbox.c's
 * inbox_row_container — the touching-rows/adjacency-floor shape). A
 * row's only unique on-screen text is its name label, so the lookup goes
 * by that, steps up to the row, and takes child 0 — the overlay.
 * find_label_exact is the shared header's (support/lv_test_harness.h) —
 * also used by the settings-face tests below. */
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
 * as the one clickable button sized exactly 112x112 (scr_inbox.c's
 * FF_INBOX_FAB_HIT_PX = PUCK_PX - 300 — the corner-anchored hit that
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

/* PR #142 review Design 2: the launcher's circles are all the SAME
 * size (96x96, up from 64x64), so `find_clickable_by_size` above
 * (which returns the FIRST match) can no longer tell them apart — and
 * Design 1 swapped their labels for LV_SYMBOL_* glyphs, so
 * `find_button_with_label` can't either (the glyph string is not a
 * stable per-circle identifier the way "NOW"/"SIG"/"MAP"/"SET" text
 * used to be). Walks the SAME depth-first, sibling-order traversal as
 * both helpers above and returns the `want`-th (0-based) same-size
 * button it finds — reliable because `ff_scr_launcher_build` adds the
 * circles as direct puck children in a FIXED order (S26e, amended
 * 2026-09-01: FIVE circles now — Radar, Now, Signals, Map, Settings —
 * the same order `launcher_idx` encodes), so tree order IS circle
 * order. `*counter` is threaded through the recursion so one top-level
 * call counts correctly across the whole subtree. */
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

/* `idx`: 0=Radar, 1=Now, 2=Signals, 3=Map, 4=Settings (ff_intent.h's
 * FF_INTENT_LAUNCHER_SELECT payload convention).
 *
 * S26e VISUAL REFRESH (2026-09-01, compass ring): the launcher no
 * longer draws five uniform 96x96 circles — idx 0 (Radar) is now the
 * 120x120 HUB disc and idx 1-4 are 88x88 SATELLITE discs
 * (scr_launcher.c). Circle CREATION order still equals launcher_idx
 * order (see that file's satellite descriptor table comment, which
 * exists specifically so this helper doesn't have to change beyond its
 * size constants) — so idx 0 is the sole 120x120 clickable, and idx
 * 1..4 are the Nth 88x88 clickable in creation order. */
static lv_obj_t *launcher_circle_at(int idx)
{
    int counter = 0;
    if (idx == 0) {
        return find_nth_clickable_by_size(lv_screen_active(), 120, 120, &counter, 0);
    }
    return find_nth_clickable_by_size(lv_screen_active(), 88, 88, &counter, idx - 1);
}

/* A member conversation row tap emits OPEN_THREAD with that member's
 * node id. */
static void S24b_inbox_member_row_tap_emits_open_thread(void)
{
    ff_app_inbox_t v;
    memset(&v, 0, sizeof(v));
    sig_add_conv(&v, FF_CONV_CREW, 0u, NULL, 0, 0);
    sig_add_conv(&v, FF_CONV_MEMBER, 111u, "DANA", 1, 2);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_inbox_build(parent, &v, false);

    click(find_row_hit_by_name(parent, "DANA"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_OPEN_THREAD, s_spy.last.kind);
    TEST_ASSERT_EQUAL_UINT32(111u, s_spy.last.u.node_id);
}

/* The CREW row emits OPEN_THREAD with the CREW key (node 0). */
static void S24b_inbox_crew_row_tap_emits_open_thread_crew(void)
{
    ff_app_inbox_t v;
    memset(&v, 0, sizeof(v));
    sig_add_conv(&v, FF_CONV_CREW, 0u, NULL, 2, 3);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_inbox_build(parent, &v, false);

    click(find_row_hit_by_name(parent, "CREW"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_OPEN_THREAD, s_spy.last.kind);
    TEST_ASSERT_EQUAL_UINT32(0u, s_spy.last.u.node_id);
}

/* A QUIET member's row (no traffic) is a tap target too — the thread is
 * where you'd go to signal them. */
static void S24b_inbox_quiet_member_row_is_tappable(void)
{
    ff_app_inbox_t v;
    memset(&v, 0, sizeof(v));
    sig_add_conv(&v, FF_CONV_CREW, 0u, NULL, 0, 0);
    ff_inbox_conv_t *max = sig_add_conv(&v, FF_CONV_MEMBER, 222u, "MAX", 0, 0);
    max->presence = FF_PRESENCE_LINKED;

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_inbox_build(parent, &v, false);

    click(find_row_hit_by_name(parent, "MAX"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_OPEN_THREAD, s_spy.last.kind);
    TEST_ASSERT_EQUAL_UINT32(222u, s_spy.last.u.node_id);
}

/* The + FAB emits INBOX_NEW (the recipient-picker step). */
static void S24b_inbox_fab_emits_inbox_new(void)
{
    ff_app_inbox_t v;
    memset(&v, 0, sizeof(v));
    sig_add_conv(&v, FF_CONV_CREW, 0u, NULL, 0, 0);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_inbox_build(parent, &v, false);

    click(find_clickable_by_size(parent, 112, 112));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_NEW, s_spy.last.kind);
}

/* Picker rows emit INBOX_PICK with the picked scope; CREW is pinned and
 * carries the CREW key. */
static void S24b_picker_rows_emit_pick(void)
{
    ff_app_inbox_t v;
    memset(&v, 0, sizeof(v));
    v.subview = FF_INBOX_SUB_PICKER;
    sig_add_conv(&v, FF_CONV_CREW, 0u, NULL, 0, 0);
    sig_add_conv(&v, FF_CONV_MEMBER, 111u, "DANA", 0, 0);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_inbox_build(parent, &v, false);

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
    ff_app_inbox_t v;
    memset(&v, 0, sizeof(v));
    v.subview = FF_INBOX_SUB_PICKER;
    sig_add_conv(&v, FF_CONV_CREW, 0u, NULL, 0, 0);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_inbox_build(parent, &v, false);
    click(find_button_with_label(parent, LV_SYMBOL_LEFT));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_BACK, s_spy.last.kind);

    lv_obj_clean(parent);
    v.subview = FF_INBOX_SUB_THREAD;
    v.thread_node = 0u;
    strncpy(v.thread_name, "CREW", sizeof(v.thread_name) - 1);
    ff_scr_inbox_build(parent, &v, false);
    click(find_button_with_label(parent, LV_SYMBOL_LEFT));
    TEST_ASSERT_EQUAL_INT(2, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_BACK, s_spy.last.kind);
}

/* S24 slice (c) — a rendered 1:1 thread view for the chip tests: the
 * DANA scope, one inbound and one outgoing message. */
static void s24c_make_direct_thread(ff_app_inbox_t *v)
{
    memset(v, 0, sizeof(*v));
    v->subview = FF_INBOX_SUB_THREAD;
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
    in_m->kind = FEED_TEXT; /* 2026-09-02: was FEED_PULSE, retired — see
                              * ff_feed.h; this fixture only needs SOME
                              * rendered inbound row for the chip-click
                              * tests below, kind is otherwise immaterial */
    in_m->dir = FEED_DIR_DIRECT;
    in_m->identity_known = true;
    in_m->node_id = 111u;
    strncpy(in_m->name, "DANA", sizeof(in_m->name) - 1);
    strncpy(in_m->text, "on my way", sizeof(in_m->text) - 1);
}

/* The 1:1 quick chips: OMW and IN 5 MIN emit CANNED_REPLY with the right
 * canned id (the shell aims them at the thread scope — its own test). */
static void S24c_thread_omw_chip_emits_canned_reply_omw(void)
{
    ff_app_inbox_t v;
    s24c_make_direct_thread(&v);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_inbox_build(parent, &v, false);

    click(find_button_with_label(parent, "OMW"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_CANNED_REPLY, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_WIRING_REPLY_OMW, s_spy.last.u.reply);
}

static void S24c_thread_in5min_chip_emits_canned_reply_5min(void)
{
    ff_app_inbox_t v;
    s24c_make_direct_thread(&v);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_inbox_build(parent, &v, false);

    click(find_button_with_label(parent, "IN 5 MIN"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_CANNED_REPLY, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_WIRING_REPLY_5MIN, s_spy.last.u.reply);
}

/* The FLARE chip emits the outbound send intent, no payload (the outbound
 * quick signal is a flare, not a pulse). */
static void S24c_thread_flare_chip_emits_sig_flare(void)
{
    ff_app_inbox_t v;
    s24c_make_direct_thread(&v);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_inbox_build(parent, &v, false);

    click(find_button_with_label(parent, "FLARE"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_FLARE, s_spy.last.kind);
}

/* The thread FAB emits INBOX_NEW (slice (d) routes it to the scoped
 * action popup; until then the shell's INBOX_NEW handler is a documented
 * no-op outside the inbox). Both thread shapes carry the FAB. */
static void S24c_thread_fab_emits_inbox_new(void)
{
    ff_app_inbox_t v;
    s24c_make_direct_thread(&v);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_inbox_build(parent, &v, false);
    click(find_clickable_by_size(parent, 112, 112));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_NEW, s_spy.last.kind);

    /* CREW thread too. */
    lv_obj_clean(parent);
    memset(&v, 0, sizeof(v));
    v.subview = FF_INBOX_SUB_THREAD;
    v.thread_node = 0u;
    strncpy(v.thread_name, "CREW", sizeof(v.thread_name) - 1);
    sig_add_conv(&v, FF_CONV_CREW, 0u, NULL, 0, 0);
    ff_scr_inbox_build(parent, &v, false);
    click(find_clickable_by_size(parent, 112, 112));
    TEST_ASSERT_EQUAL_INT(2, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_NEW, s_spy.last.kind);
}

/* The CREW thread renders NO quick chips (they are the 1:1 screen's) —
 * a chip label must not be findable there. */
static void S24c_crew_thread_has_no_quick_chips(void)
{
    ff_app_inbox_t v;
    memset(&v, 0, sizeof(v));
    v.subview = FF_INBOX_SUB_THREAD;
    v.thread_node = 0u;
    strncpy(v.thread_name, "CREW", sizeof(v.thread_name) - 1);
    sig_add_conv(&v, FF_CONV_CREW, 0u, NULL, 0, 0);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_inbox_build(parent, &v, false);
    TEST_ASSERT_NULL(find_button_with_label(parent, "OMW"));
    TEST_ASSERT_NULL(find_button_with_label(parent, "FLARE"));
}

/* =================================================================== */
/* Thread message list scroll (maintainer bug report: "CREW thread      */
/* messages don't scroll") — the scroll-vs-tap property this file's     */
/* drag()/drag_v() helpers already exist to test. Twelve messages, well */
/* past the CREW list's 190px band, so the content is guaranteed to     */
/* overflow regardless of row-height tuning.                            */
/* =================================================================== */

static void s24_make_crew_thread_long(ff_app_inbox_t *v)
{
    memset(v, 0, sizeof(*v));
    v->subview = FF_INBOX_SUB_THREAD;
    v->thread_node = 0u;
    strncpy(v->thread_name, "CREW", sizeof(v->thread_name) - 1);
    sig_add_conv(v, FF_CONV_CREW, 0u, NULL, 0, 12);

    static char const *const names[] = {"DANA", "RILEY", "MAX", "JAMIE", "KEV"};
    for (int i = 0; i < 12; i++) {
        ff_inbox_msg_t *m = &v->thread.msgs[v->thread.msg_count++];
        memset(m, 0, sizeof(*m));
        m->kind = FEED_TEXT;
        m->dir = FEED_DIR_BROADCAST;
        m->identity_known = true;
        m->node_id = 111u + (uint32_t)(i % 5);
        strncpy(m->name, names[i % 5], sizeof(m->name) - 1);
        m->initial = names[i % 5][0];
        m->color_idx = (uint8_t)(i % 5);
        snprintf(m->text, sizeof(m->text), "signal number %d checking in", i);
        m->age_ms = (uint32_t)(60000u * (uint32_t)(12 - i));
    }
}

/* Same shape, DANA's 1:1 thread — used to prove the property holds there
 * too (it already did; this is the comparison the investigation brief
 * asks for, not a regression guard for a prior bug). */
static void s24_make_direct_thread_long(ff_app_inbox_t *v)
{
    memset(v, 0, sizeof(*v));
    v->subview = FF_INBOX_SUB_THREAD;
    v->thread_node = 111u;
    strncpy(v->thread_name, "DANA", sizeof(v->thread_name) - 1);
    sig_add_conv(v, FF_CONV_CREW, 0u, NULL, 0, 0);
    ff_inbox_conv_t *dana = sig_add_conv(v, FF_CONV_MEMBER, 111u, "DANA", 0, 12);
    dana->presence = FF_PRESENCE_SEEN;
    dana->presence_age_ms = 60000u;

    for (int i = 0; i < 12; i++) {
        ff_inbox_msg_t *m = &v->thread.msgs[v->thread.msg_count++];
        memset(m, 0, sizeof(*m));
        m->kind = FEED_TEXT;
        m->dir = (i % 2 == 0) ? FEED_DIR_DIRECT : FEED_DIR_OUT;
        m->identity_known = true;
        m->node_id = 111u;
        strncpy(m->name, "DANA", sizeof(m->name) - 1);
        m->initial = 'D';
        snprintf(m->text, sizeof(m->text), "message number %d", i);
        m->age_ms = (uint32_t)(60000u * (uint32_t)(12 - i));
    }
}

/* Finds the one PLAIN lv_obj (not a label/button — lv_label_create
 * leaves LV_OBJ_FLAG_SCROLLABLE set by default too, so filtering on the
 * flag alone would match the first header label instead) carrying
 * LV_OBJ_FLAG_SCROLLABLE. Every other lv_obj_create object scr_inbox.c
 * builds under a thread (message rows, the crew dot, the bottom fade) is
 * explicitly decorated via inbox_child_deco, which clears this flag,
 * so it uniquely identifies the message list regardless of whether the
 * list itself is also CLICKABLE. */
static lv_obj_t *find_scrollable(lv_obj_t *root)
{
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        if (lv_obj_check_type(child, &lv_obj_class) && lv_obj_has_flag(child, LV_OBJ_FLAG_SCROLLABLE)) {
            return child;
        }
        lv_obj_t *found = find_scrollable(child);
        if (found != NULL) return found;
    }
    return NULL;
}

/* A physical vertical drag over the thread's message content must move
 * the list's scroll offset — content that overflows a bounded list is
 * useless if touch can never reach it. Covers CREW (the maintainer's
 * report) and 1:1 (the investigation brief's required comparison): both
 * must scroll, and neither drag may be mistaken for a row-open tap (the
 * thread has none, so this also just asserts zero spurious intents). */
static void thread_overflow_scrolls_on_drag(bool crew)
{
    ff_app_inbox_t v;
    if (crew) {
        s24_make_crew_thread_long(&v);
    } else {
        s24_make_direct_thread_long(&v);
    }

    /* Real-indev hit-testing (unlike click()'s direct event injection the
     * other Signals tests use) needs `parent`'s own geometry to actually
     * contain the drag point — lv_obj_create's 100x100 default leaves
     * anything past x/y=100 unreachable by lv_indev_search_obj, same as
     * the real shell sizing its container to the full 412x412 display. */
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, 412, 412);
    ff_scr_inbox_build(parent, &v, false);

    lv_obj_t *list = find_scrollable(parent);
    TEST_ASSERT_NOT_NULL(list);

    /* The build scrolls to newest-at-bottom already: confirm the content
     * actually overflows (scroll range > 0) rather than trusting the
     * fixture's message count. */
    lv_obj_update_layout(list);
    int32_t const max_scroll = lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list);
    TEST_ASSERT_GREATER_THAN_INT32(0, max_scroll);

    /* Reset to the top so an upward drag has room to move, then drag a
     * finger up (content follows the finger toward the newest message,
     * increasing the scroll offset) inside the list's on-glass band. */
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    TEST_ASSERT_EQUAL_INT32(0, lv_obj_get_scroll_y(list));

    lv_area_t list_area;
    lv_obj_get_coords(list, &list_area);
    int32_t const list_top = list_area.y1;
    int32_t const list_center_x = (list_area.x1 + list_area.x2) / 2;
    drag_v(list_top + 90, list_top + 10, list_center_x);

    TEST_ASSERT_GREATER_THAN_INT32(0, lv_obj_get_scroll_y(list));
    TEST_ASSERT_EQUAL_INT(0, s_spy.count);
}

static void S24_crew_thread_overflow_scrolls_on_drag(void)
{
    thread_overflow_scrolls_on_drag(true);
}

static void S24_direct_thread_overflow_scrolls_on_drag(void)
{
    thread_overflow_scrolls_on_drag(false);
}

/* =================================================================== */
/* Thread message bubbles are NOT compressed (investigation brief's first */
/* hypothesis to test for "the thread view looks a bit smashed" — see    */
/* scr_inbox.c's own header comment on inbox_build_thread: DISPROVEN */
/* by inspection, no flex/shrink layout anywhere in this file, but kept  */
/* as a standing regression guard rather than thrown away — a future     */
/* change that DID introduce a shrink-to-fit container should fail this. */
/* =================================================================== */

/* A message bubble's text label is a DIRECT child of the bubble
 * container (inbox_msg_bubble: `lv_obj_t *l = inbox_mk_label(bub,
 * text, ...)`), positioned at a fixed (13, 8) offset — never wrapped, so
 * its natural (unclamped) height is exactly its font's line height. This
 * asserts that height survives all the way to the rendered object (proof
 * nothing upstream shrank it) AND that the bubble container is tall
 * enough to show it without clipping — the literal "not compressed below
 * its natural height" property for both the label and its container. */
static void S24_thread_message_bubble_not_compressed(void)
{
    ff_app_inbox_t v;
    memset(&v, 0, sizeof(v));
    v.subview = FF_INBOX_SUB_THREAD;
    v.thread_node = 0u;
    strncpy(v.thread_name, "CREW", sizeof(v.thread_name) - 1);
    sig_add_conv(&v, FF_CONV_CREW, 0u, NULL, 0, 1);

    ff_inbox_msg_t *m = &v.thread.msgs[v.thread.msg_count++];
    memset(m, 0, sizeof(*m));
    m->kind = FEED_TEXT;
    m->dir = FEED_DIR_OUT; /* OUT text -> plain bubble, no sender line above it */
    strncpy(m->text, "copy, see you there", sizeof(m->text) - 1);
    m->age_ms = 60000u;

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, 412, 412);
    ff_scr_inbox_build(parent, &v, false);

    lv_obj_t *label = find_label_exact(parent, "copy, see you there");
    TEST_ASSERT_NOT_NULL_MESSAGE(label, "message bubble text not found");
    lv_obj_t *bubble = lv_obj_get_parent(label);
    TEST_ASSERT_NOT_NULL(bubble);

    lv_obj_update_layout(label);
    int32_t const natural_h = (int32_t)lv_font_get_line_height(FF_THEME_FONT_MSG_BODY);
    TEST_ASSERT_GREATER_OR_EQUAL_INT32_MESSAGE(natural_h, lv_obj_get_height(label),
                                               "the label itself must not be squeezed under its font's natural "
                                               "line height");
    int32_t const label_bottom = lv_obj_get_y(label) + lv_obj_get_height(label);
    TEST_ASSERT_LESS_OR_EQUAL_INT32_MESSAGE(lv_obj_get_height(bubble), label_bottom,
                                            "the bubble container must be tall enough to show its label's full "
                                            "natural height without clipping it");
}

/* =================================================================== */
/* PR #149 review round 2 (FAIL 1 — "the thread still reads smashed:      */
/* only ~3 of 12 messages are visible at rest") + round 3 (the deep CREW  */
/* band buried the NEWEST message under the FAB slice, faded and         */
/* partly hidden — a row count gained by burying it is not a win).       */
/*                                                                        */
/* Round 3 pulled the CREW band back to stop EXACTLY at FF_INBOX_FAB_   */
/* DECO_Y (the FAB's own visible top edge) — `list` clips its children    */
/* to its own bounds, so nothing in it can ever paint past that y         */
/* regardless of x, the simplest way to guarantee no bubble/age is ever   */
/* under the slice. A deeper band was rejected: PR #143's scroll-         */
/* preservation restores an arbitrary saved scroll_y on a same-thread     */
/* rebuild (not just "newest at bottom"), so a fully correct per-row FAB  */
/* inset would have to protect EVERY row, not just the newest — any one   */
/* could end up settled at the viewport's bottom edge — which collapses   */
/* to the same width cap as never entering the FAB zone at all. Measured  */
/* (not assumed) that 5 FULLY-contained rows is unreachable within that   */
/* boundary even at zero gap; the honest AC for the CREW fixture is       */
/* therefore >= 4 FULLY-contained rows (count_rows_full_visible — no      */
/* partial-overlap credit, per the review). The 1:1 thread's own boundary */
/* (the chip strip, unmoved, unaffected by the FAB fix) is untouched from */
/* round 2: full containment is still mathematically unreachable there    */
/* (4 rows cost >= 4*(BUBBLE_H+AGE_H) = 200px at zero gap against a hard  */
/* ~182px ceiling neither the font floor nor the design's own 8px bubble  */
/* padding can close), so it keeps the round-2 "at least half the row's   */
/* own height on-glass" criterion (count_rows_half_visible).              */
/* ------------------------------------------------------------------- */

/* Counts message rows (plain lv_obj children of `list`, skipping the
 * FLOATING scroll catcher) that are FULLY contained within the list's
 * own viewport — no part clipped top or bottom. */
static int count_rows_full_visible(lv_obj_t *list)
{
    lv_area_t la;
    lv_obj_get_coords(list, &la);
    uint32_t n = lv_obj_get_child_count(list);
    int count = 0;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(list, i);
        if (!lv_obj_check_type(child, &lv_obj_class)) continue;
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_FLOATING)) continue; /* the scroll catcher, not a message row */
        lv_area_t ra;
        lv_obj_get_coords(child, &ra);
        if (ra.y1 >= la.y1 && ra.y2 <= la.y2) {
            count++;
        }
    }
    return count;
}

/* Counts message rows (plain lv_obj children of `list`, skipping the
 * FLOATING scroll catcher — same shape as find_scrollable's own
 * decoration convention) whose rect overlaps the list's own viewport by
 * at least half of the row's own height. */
static int count_rows_half_visible(lv_obj_t *list)
{
    lv_area_t la;
    lv_obj_get_coords(list, &la);
    uint32_t n = lv_obj_get_child_count(list);
    int count = 0;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(list, i);
        if (!lv_obj_check_type(child, &lv_obj_class)) continue;
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_FLOATING)) continue; /* the scroll catcher, not a message row */
        lv_area_t ra;
        lv_obj_get_coords(child, &ra);
        if (ra.y2 < la.y1 || ra.y1 > la.y2) continue; /* no overlap at all */
        int32_t ov_y1 = ra.y1 > la.y1 ? ra.y1 : la.y1;
        int32_t ov_y2 = ra.y2 < la.y2 ? ra.y2 : la.y2;
        int32_t overlap = ov_y2 - ov_y1 + 1;
        int32_t row_h = ra.y2 - ra.y1 + 1;
        if (overlap * 2 >= row_h) {
            count++;
        }
    }
    return count;
}

static void S24_crew_thread_shows_at_least_4_full_rows_at_rest(void)
{
    ff_app_inbox_t v;
    s24_make_crew_thread_long(&v);
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, 412, 412);
    ff_scr_inbox_build(parent, &v, false);
    lv_obj_update_layout(parent);

    lv_obj_t *list = find_scrollable(parent);
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(4, count_rows_full_visible(list),
                                             "the 12-message CREW thread must show at least 4 FULLY-contained "
                                             "message rows at rest, not scrolled (5 is unreachable without the "
                                             "band extending under the FAB slice, which round 3 of this review "
                                             "explicitly rejected)");
}

static void S24_direct_thread_shows_at_least_4_rows_at_rest(void)
{
    ff_app_inbox_t v;
    s24_make_direct_thread_long(&v);
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, 412, 412);
    ff_scr_inbox_build(parent, &v, false);
    lv_obj_update_layout(parent);

    lv_obj_t *list = find_scrollable(parent);
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(4, count_rows_half_visible(list),
                                             "the long 1:1 thread must show at least 4 message rows (each >= half "
                                             "its own height on-glass) at rest, not scrolled, with the quick-reply "
                                             "chips still present");
    /* "chips present" per the review — this AC must not be satisfiable by
     * silently removing the quick-reply strip to buy the 1:1 thread more
     * room. */
    TEST_ASSERT_NOT_NULL_MESSAGE(find_button_with_label(parent, "OMW"), "1:1 quick-reply chips must stay present");
}

/* =================================================================== */
/* PR #149 review round 3 — "no part of any bubble/age is ever under the */
/* FAB slice": a direct geometric regression guard, independent of      */
/* which of the review's two acceptable shapes a future change picks.   */
/* Circle matches scr_inbox.c's own FF_INBOX_FAB_DECO_X/Y/D (278,    */
/* 280, 240 — private to that file, so mirrored here as literals, same  */
/* convention this file already uses for the FAB hit target's 112x112   */
/* size via find_clickable_by_size). Checked against every message ROW  */
/* (not each label individually) — rows bound their own bubble/age/     */
/* sender-line children, so a row disjoint from the circle guarantees   */
/* everything inside it is too. */
/* ------------------------------------------------------------------- */

#define FAB_DECO_D 240.0f /* FF_INBOX_FAB_DECO_D — private to scr_inbox.c, mirrored here */
#define FAB_DECO_R (FAB_DECO_D / 2.0f)

/* true iff the closest point of rect `a` to the FAB deco circle (center
 * `cx`,`cy`, radius FAB_DECO_R) is farther than the radius — i.e. the
 * rect never paints any pixel the amber slice also claims. */
static bool rect_disjoint_from_fab_deco(lv_area_t const *a, float cx, float cy)
{
    float const nx = (float)a->x1 > cx   ? (float)a->x1
                      : (float)a->x2 < cx ? (float)a->x2
                                          : cx;
    float const ny = (float)a->y1 > cy   ? (float)a->y1
                      : (float)a->y2 < cy ? (float)a->y2
                                          : cy;
    float const dx = nx - cx;
    float const dy = ny - cy;
    return (dx * dx + dy * dy) > (FAB_DECO_R * FAB_DECO_R);
}

/* The FAB deco circle's ABSOLUTE center in THIS test's own render space.
 * Derived at runtime from the FAB hit target (112x112, corner-anchored —
 * already how this file locates the FAB elsewhere, find_clickable_by_size)
 * rather than hardcoding scr_inbox.c's private FF_INBOX_FAB_DECO_X/Y
 * literals: this test's bare `lv_obj_create(lv_screen_active())` parent
 * (unlike the real shell's own stripped container) carries default-theme
 * padding that shifts EVERY absolute coordinate by a constant offset —
 * measured, not assumed, elsewhere in this file's own S24 probes — so a
 * literal circle center would silently compare against the wrong origin.
 * FF_INBOX_FAB_HIT_X/Y are both 300 (scr_inbox.c); the deco circle's
 * center sits at deco (278,280) + D/2 = (398,400) in that SAME space, a
 * constant (+98,+100) offset from the hit rect's own top-left corner that
 * survives whatever the test harness's own translation happens to be. */
static void fab_deco_center(lv_obj_t *parent, float *out_cx, float *out_cy)
{
    lv_obj_t *fab = find_clickable_by_size(parent, 112, 112);
    TEST_ASSERT_NOT_NULL_MESSAGE(fab, "FAB hit target (112x112) not found — can't locate the deco circle");
    lv_area_t fa;
    lv_obj_get_coords(fab, &fa);
    *out_cx = (float)fa.x1 + 98.0f; /* FF_INBOX_FAB_DECO_X(278) - FF_INBOX_FAB_HIT_X(300) + D/2(120) */
    *out_cy = (float)fa.y1 + 100.0f; /* FF_INBOX_FAB_DECO_Y(280) - FF_INBOX_FAB_HIT_Y(300) + D/2(120) */
}

static void S24_crew_thread_no_row_ever_under_the_fab_slice(void)
{
    ff_app_inbox_t v;
    s24_make_crew_thread_long(&v);
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, 412, 412);
    ff_scr_inbox_build(parent, &v, false);
    lv_obj_update_layout(parent);

    lv_obj_t *list = find_scrollable(parent);
    TEST_ASSERT_NOT_NULL(list);

    float cx, cy;
    fab_deco_center(parent, &cx, &cy);

    uint32_t n = lv_obj_get_child_count(list);
    int checked = 0;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(list, i);
        if (!lv_obj_check_type(child, &lv_obj_class)) continue;
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_FLOATING)) continue; /* the scroll catcher, not a message row */
        lv_area_t ra;
        lv_obj_get_coords(child, &ra);
        checked++;
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "message row %u rect=(%d,%d)-(%d,%d) must never overlap the FAB deco circle (center=%.0f,%.0f)",
                 i, (int)ra.x1, (int)ra.y1, (int)ra.x2, (int)ra.y2, (double)cx, (double)cy);
        TEST_ASSERT_TRUE_MESSAGE(rect_disjoint_from_fab_deco(&ra, cx, cy), msg);
    }
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, checked, "no message rows were even found to check");
}

/* =================================================================== */
/* PR #149 review round 2 (FAIL 2 — truncation regression): the message- */
/* body font bump (14px -> FF_THEME_FONT_MSG_BODY, 16px) needs a wider   */
/* FF_INBOX_MSG_MAX_W or text that fit before now ellipsizes. Checks   */
/* the WIDEST message string across every committed fixture (measured,   */
/* not guessed — "sounds good, heading there now" beats the longer-      */
/* looking "grabbing water first, back in 10" by actual rendered pixel   */
/* width at this font) renders in a real message bubble without engaging */
/* LV_LABEL_LONG_MODE_DOTS (inbox_label_clamp's own truncation flag).  */
/* ------------------------------------------------------------------- */
static void S24_widest_fixture_message_does_not_truncate(void)
{
    ff_app_inbox_t v;
    memset(&v, 0, sizeof(v));
    v.subview = FF_INBOX_SUB_THREAD;
    v.thread_node = 0u;
    strncpy(v.thread_name, "CREW", sizeof(v.thread_name) - 1);
    sig_add_conv(&v, FF_CONV_CREW, 0u, NULL, 0, 1);

    static char const *const WIDEST_FIXTURE_TEXT = "sounds good, heading there now"; /* measured widest, see above */
    ff_inbox_msg_t *m = &v.thread.msgs[v.thread.msg_count++];
    memset(m, 0, sizeof(*m));
    m->kind = FEED_TEXT;
    m->dir = FEED_DIR_OUT;
    strncpy(m->text, WIDEST_FIXTURE_TEXT, sizeof(m->text) - 1);
    m->age_ms = 60000u;

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, 412, 412);
    ff_scr_inbox_build(parent, &v, false);

    lv_obj_t *label = find_label_exact(parent, WIDEST_FIXTURE_TEXT);
    TEST_ASSERT_NOT_NULL_MESSAGE(label, "the widest fixture message must render its FULL text verbatim as one "
                                        "findable label — if this fails, the text was truncated (a different, "
                                        "dot-suffixed string) or clipped out of the tree entirely");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(LV_LABEL_LONG_MODE_DOTS, lv_label_get_long_mode(label),
                                  "the widest fixture message must not engage the ellipsis long-mode");
}

/* =================================================================== */
/* Rally WHERE list reachability (maintainer bug report: "I can't scroll */
/* to select all the locations on the send rally screen") — a real       */
/* festpack-shaped 7-row list (On Me + 5 stages + Camp), same real-indev  */
/* drag()/tap_at() harness as the thread scroll tests above.             */
/* =================================================================== */

static void s24_make_rally_seven_rows(ff_app_inbox_t *v)
{
    memset(v, 0, sizeof(*v));
    v->subview = FF_INBOX_SUB_RALLY;
    v->thread_node = 0u;
    strncpy(v->thread_name, "CREW", sizeof(v->thread_name) - 1);
    sig_add_conv(v, FF_CONV_CREW, 0u, NULL, 0, 0);
    v->rally.on_me_ok = true;
    v->rally.sel = 1u;
    v->rally.when = FF_RALLY_WHEN_15;
    strncpy(v->rally.echo_when, "+15m", sizeof(v->rally.echo_when) - 1);
    v->rally.place_count = 6u; /* + On Me = 7 WHERE rows, per the maintainer's real festpack */
    static char const *const places[] = {"Main Stage", "Bass Hollow", "Wompy Woods",
                                         "The Grove",  "Sunrise Stage", "Camp"};
    for (int i = 0; i < 6; i++) {
        strncpy(v->rally.place_names[i], places[i], FF_APP_NAME_LEN - 1);
    }
    strncpy(v->rally.echo_place, "Main Stage", sizeof(v->rally.echo_place) - 1);
    v->rally.can_send = true;
}

static void S24_rally_where_list_scrolls_to_reach_all_rows(void)
{
    ff_app_inbox_t v;
    s24_make_rally_seven_rows(&v);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, 412, 412);
    ff_scr_inbox_build(parent, &v, false);

    lv_obj_t *list = find_scrollable(parent);
    TEST_ASSERT_NOT_NULL(list);
    lv_obj_update_layout(list);
    int32_t const max_scroll = lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list);
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(0, max_scroll, "the 7-row WHERE list must genuinely overflow");

    /* "Camp", the LAST row, is off-screen at open (the list opens
     * scrolled to the top, showing On Me first). */
    lv_obj_t *camp_hit = find_row_hit_by_name(parent, "Camp");
    TEST_ASSERT_NOT_NULL(camp_hit);
    lv_area_t camp_area;
    lv_obj_get_coords(camp_hit, &camp_area);
    lv_area_t list_area;
    lv_obj_get_coords(list, &list_area);
    TEST_ASSERT_GREATER_OR_EQUAL_INT32_MESSAGE(list_area.y2, camp_area.y1,
                                               "Camp must be off-viewport (below) before any scroll");

    /* A drag started on the PLACES divider band — a dead zone with no
     * clickable descendant of its own (the maintainer's actual failure
     * mode: not every drag starts squarely on a row) — must still move
     * the list, and must never be mistaken for a tap (no intent). */
    int32_t const list_top = list_area.y1;
    int32_t const cx = (list_area.x1 + list_area.x2) / 2;
    /* The PLACES divider band sits strictly between the On Me row's hit
     * target and the first landmark row's — its own geometric midpoint,
     * not a private layout constant this test file has no access to (the
     * FF_INBOX_RALLY_* pitch macros are scr_inbox.c-local). */
    lv_obj_t *on_me_hit = find_row_hit_by_name(parent, "On Me");
    lv_obj_t *main_stage_hit = find_row_hit_by_name(parent, "Main Stage");
    TEST_ASSERT_NOT_NULL(on_me_hit);
    TEST_ASSERT_NOT_NULL(main_stage_hit);
    lv_area_t on_me_area, main_stage_area;
    lv_obj_get_coords(on_me_hit, &on_me_area);
    lv_obj_get_coords(main_stage_hit, &main_stage_area);
    int32_t const divider_y = (on_me_area.y2 + main_stage_area.y1) / 2;
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(on_me_area.y2, main_stage_area.y1,
                                           "test setup: expected a real gap between On Me and Main Stage to drag "
                                           "in — the divider band would not exist otherwise");
    drag_v(divider_y, list_top - 260, cx);
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(0, lv_obj_get_scroll_y(list),
                                           "a drag started on the PLACES divider dead zone must still scroll "
                                           "the WHERE list");
    TEST_ASSERT_EQUAL_INT(0, s_spy.count);

    /* Now that the drag brought it into view, Camp is reachable AND still
     * tappable — a real physical tap (press+release, not click()'s direct
     * event injection), same property PR #143's OMW test proved for the
     * thread's own scroll touch target. */
    lv_obj_update_layout(list);
    lv_obj_get_coords(camp_hit, &camp_area);
    int32_t const camp_cy = (camp_area.y1 + camp_area.y2) / 2;
    TEST_ASSERT_TRUE_MESSAGE(camp_cy >= list_area.y1 && camp_cy <= list_area.y2,
                             "Camp must have scrolled into the viewport");
    /* tap_at() is defined further down this file (used by the thread's
     * own real-touch test); a real tap is a press+release with no
     * movement, so drag_v(y, y, x) is exactly that primitive inline. */
    drag_v(camp_cy, camp_cy, (camp_area.x1 + camp_area.x2) / 2);
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_RALLY_SELECT_PLACE, s_spy.last.kind);
    TEST_ASSERT_EQUAL_UINT8(6u, s_spy.last.u.rally_idx); /* On Me=0, ... Camp is the 6th place -> idx 6 */
}

/* ---------------------------------------------------------------------
 * PR #143 review: scroll position must survive the S24 render-key
 * rebuild (an age-bucket crossing legitimately dirties the key —
 * ff_shell.c's own doc comment — which does lv_obj_clean + a fresh
 * inbox_build_thread). Screen-level tests: a rebuild is simulated the
 * same way ff_shell's render loop does it, lv_obj_clean(parent) then a
 * second ff_scr_inbox_build(parent, &v, false) call, since that IS
 * the mechanism (ff_shell decides WHEN to rebuild; this file already
 * tests screens by driving that same primitive directly, same as every
 * other test in it).
 * ------------------------------------------------------------------- */

/* A real vertical drag, then a same-thread/same-count rebuild: the
 * offset must be preserved, not snapped back to newest. The FLOATING
 * touch target must still be there and still reachable afterward (a
 * second drag still moves the list) — the restore must not have
 * disturbed it. */
static void S24_thread_scroll_preserved_across_same_thread_rebuild(void)
{
    ff_app_inbox_t v;
    s24_make_crew_thread_long(&v);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, 412, 412);
    ff_scr_inbox_build(parent, &v, false);

    lv_obj_t *list = find_scrollable(parent);
    TEST_ASSERT_NOT_NULL(list);
    lv_obj_update_layout(list);

    lv_area_t list_area;
    lv_obj_get_coords(list, &list_area);
    int32_t const list_top = list_area.y1;
    int32_t const list_center_x = (list_area.x1 + list_area.x2) / 2;

    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    drag_v(list_top + 90, list_top + 10, list_center_x);
    int32_t const scrolled_y = lv_obj_get_scroll_y(list);
    TEST_ASSERT_GREATER_THAN_INT32(0, scrolled_y);

    /* Simulate the render loop's legitimate age-bucket rebuild: SAME
     * thread (node_id unchanged), SAME message count — the property
     * under test is that this alone preserves the offset. */
    lv_obj_clean(parent);
    ff_scr_inbox_build(parent, &v, false);

    lv_obj_t *list2 = find_scrollable(parent);
    TEST_ASSERT_NOT_NULL(list2);
    lv_obj_update_layout(list2);
    TEST_ASSERT_EQUAL_INT32(scrolled_y, lv_obj_get_scroll_y(list2));

    /* The touch target must still be alive post-restore: drag again and
     * confirm the list keeps scrolling (not stuck, not swallowed). */
    int32_t const before_second_drag = lv_obj_get_scroll_y(list2);
    drag_v(list_top + 90, list_top + 10, list_center_x);
    TEST_ASSERT_GREATER_THAN_INT32(before_second_drag, lv_obj_get_scroll_y(list2));
}

/* A new message arriving (message count changes) must land on newest,
 * even though it's still the SAME thread — a stale offset from a
 * shorter thread would misrepresent where "the end" now is. */
static void S24_thread_scroll_resets_to_newest_on_new_message(void)
{
    ff_app_inbox_t v;
    s24_make_crew_thread_long(&v);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, 412, 412);
    ff_scr_inbox_build(parent, &v, false);

    lv_obj_t *list = find_scrollable(parent);
    TEST_ASSERT_NOT_NULL(list);
    lv_obj_update_layout(list);

    lv_area_t list_area;
    lv_obj_get_coords(list, &list_area);
    int32_t const list_top = list_area.y1;
    int32_t const list_center_x = (list_area.x1 + list_area.x2) / 2;

    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    drag_v(list_top + 90, list_top + 10, list_center_x);
    TEST_ASSERT_GREATER_THAN_INT32(0, lv_obj_get_scroll_y(list));

    /* A 13th message arrives — same thread (node_id unchanged), but the
     * message count differs from what was last built. */
    ff_inbox_msg_t *m = &v.thread.msgs[v.thread.msg_count++];
    memset(m, 0, sizeof(*m));
    m->kind = FEED_TEXT;
    m->dir = FEED_DIR_BROADCAST;
    m->identity_known = true;
    m->node_id = 111u;
    strncpy(m->name, "DANA", sizeof(m->name) - 1);
    m->initial = 'D';
    snprintf(m->text, sizeof(m->text), "just landed");
    m->age_ms = 0u;

    lv_obj_clean(parent);
    ff_scr_inbox_build(parent, &v, false);

    lv_obj_t *list2 = find_scrollable(parent);
    TEST_ASSERT_NOT_NULL(list2);
    lv_obj_update_layout(list2);
    /* Fully scrolled to the end == 0 remaining scroll-bottom distance. */
    TEST_ASSERT_EQUAL_INT32(0, lv_obj_get_scroll_bottom(list2));
    TEST_ASSERT_GREATER_THAN_INT32(0, lv_obj_get_scroll_y(list2));
}

/* Switching to a DIFFERENT thread must land on newest — a node_id
 * mismatch alone forces this regardless of any message-count
 * coincidence, since the restore requires BOTH to match. */
static void S24_thread_scroll_resets_to_newest_on_different_thread(void)
{
    ff_app_inbox_t crew_v;
    s24_make_crew_thread_long(&crew_v);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, 412, 412);
    ff_scr_inbox_build(parent, &crew_v, false);

    lv_obj_t *list = find_scrollable(parent);
    TEST_ASSERT_NOT_NULL(list);
    lv_obj_update_layout(list);

    lv_area_t list_area;
    lv_obj_get_coords(list, &list_area);
    int32_t const list_top = list_area.y1;
    int32_t const list_center_x = (list_area.x1 + list_area.x2) / 2;

    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    drag_v(list_top + 90, list_top + 10, list_center_x);
    TEST_ASSERT_GREATER_THAN_INT32(0, lv_obj_get_scroll_y(list));

    /* Switch to DANA's 1:1 thread — a different node_id entirely. */
    ff_app_inbox_t dana_v;
    s24_make_direct_thread_long(&dana_v);

    lv_obj_clean(parent);
    ff_scr_inbox_build(parent, &dana_v, false);

    lv_obj_t *list2 = find_scrollable(parent);
    TEST_ASSERT_NOT_NULL(list2);
    lv_obj_update_layout(list2);
    TEST_ASSERT_EQUAL_INT32(0, lv_obj_get_scroll_bottom(list2));
    TEST_ASSERT_GREATER_THAN_INT32(0, lv_obj_get_scroll_y(list2));
}

/* tap_at() is defined earlier in this file (right after drag_v, its one
 * dependency) so both the S99 compose tests above and the Signals test
 * below can use it. This Signals test itself proves a genuine physical
 * tap still resolves to a control OUTSIDE `list` (the chip strip is a
 * sibling of `list`, not a descendant) even once the FLOATING scroll
 * touch target is added inside an overflowing thread's list — the
 * touch-safety property the review asked to keep covered by real touch,
 * not just synthetic click(). */
static void S24_omw_chip_real_touch_on_long_overflowing_1to1_thread(void)
{
    ff_app_inbox_t v;
    s24_make_direct_thread_long(&v);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, 412, 412);
    ff_scr_inbox_build(parent, &v, false);

    lv_obj_t *list = find_scrollable(parent);
    TEST_ASSERT_NOT_NULL(list);
    lv_obj_update_layout(list);
    int32_t const max_scroll = lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list);
    TEST_ASSERT_GREATER_THAN_INT32(0, max_scroll); /* confirm this thread genuinely overflows */

    lv_obj_t *omw = find_button_with_label(parent, "OMW");
    TEST_ASSERT_NOT_NULL(omw);
    lv_area_t chip_area;
    lv_obj_get_coords(omw, &chip_area);
    int32_t const cx = (chip_area.x1 + chip_area.x2) / 2;
    int32_t const cy = (chip_area.y1 + chip_area.y2) / 2;

    tap_at(cx, cy);

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_CANNED_REPLY, s_spy.last.kind);
    TEST_ASSERT_EQUAL(FF_WIRING_REPLY_OMW, s_spy.last.u.reply);
}

/* =================================================================== */
/* S24 slice d — action popup + Rally screen emitters                  */
/* =================================================================== */

/* Build the action popup sub-view scoped to a member (DANA). */
static lv_obj_t *build_popup(ff_app_inbox_t *v)
{
    memset(v, 0, sizeof(*v));
    v->subview = FF_INBOX_SUB_POPUP;
    v->thread_node = 111u;
    strncpy(v->thread_name, "DANA", sizeof(v->thread_name) - 1);
    sig_add_conv(v, FF_CONV_CREW, 0u, NULL, 0, 0);
    sig_add_conv(v, FF_CONV_MEMBER, 111u, "DANA", 0, 0);
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_inbox_build(parent, v, false);
    return parent;
}

static void S24d_popup_compose_row_emits_popup_compose(void)
{
    ff_app_inbox_t v;
    lv_obj_t *parent = build_popup(&v);
    click(find_button_with_label(parent, "Compose"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_POPUP_COMPOSE, s_spy.last.kind);
}

static void S24d_popup_rally_row_emits_popup_rally(void)
{
    ff_app_inbox_t v;
    lv_obj_t *parent = build_popup(&v);
    click(find_button_with_label(parent, "Rally"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_POPUP_RALLY, s_spy.last.kind);
}

static void S24d_popup_flare_row_emits_popup_flare(void)
{
    ff_app_inbox_t v;
    lv_obj_t *parent = build_popup(&v);
    click(find_button_with_label(parent, "Flare"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_INBOX_POPUP_FLARE, s_spy.last.kind);
}

static void S24d_popup_close_emits_back(void)
{
    ff_app_inbox_t v;
    lv_obj_t *parent = build_popup(&v);
    click(find_button_with_label(parent, LV_SYMBOL_CLOSE));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_BACK, s_spy.last.kind);
}

/* Build the Rally sub-view with On Me + one landmark, the landmark
 * selected, WHEN=Now, Send enabled. */
static lv_obj_t *build_rally(ff_app_inbox_t *v)
{
    memset(v, 0, sizeof(*v));
    v->subview = FF_INBOX_SUB_RALLY;
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
    ff_scr_inbox_build(parent, v, false);
    return parent;
}

static void S24d_rally_on_me_row_emits_select_place_zero(void)
{
    ff_app_inbox_t v;
    lv_obj_t *parent = build_rally(&v);
    click(find_row_hit_by_name(parent, "On Me"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_RALLY_SELECT_PLACE, s_spy.last.kind);
    TEST_ASSERT_EQUAL_UINT8(0u, s_spy.last.u.rally_idx);
}

static void S24d_rally_landmark_row_emits_select_place_index(void)
{
    ff_app_inbox_t v;
    lv_obj_t *parent = build_rally(&v);
    click(find_row_hit_by_name(parent, "Main Stage"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_RALLY_SELECT_PLACE, s_spy.last.kind);
    TEST_ASSERT_EQUAL_UINT8(1u, s_spy.last.u.rally_idx); /* 0 = On Me, 1 = first landmark */
}

static void S24d_rally_when_chip_emits_cycle_when(void)
{
    ff_app_inbox_t v;
    lv_obj_t *parent = build_rally(&v);
    click(find_button_with_label(parent, "WHEN"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_RALLY_CYCLE_WHEN, s_spy.last.kind);
}

static void S24d_rally_send_button_emits_rally_send(void)
{
    ff_app_inbox_t v;
    lv_obj_t *parent = build_rally(&v);
    click(find_button_with_label(parent, "Send Rally"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_RALLY_SEND, s_spy.last.kind);
}

/* A disabled On Me (no fix) is NOT a tap target — the honest reason shows
 * but the row cannot be picked (never a fabricated position). */
static void S24d_rally_disabled_on_me_is_not_tappable(void)
{
    ff_app_inbox_t v;
    memset(&v, 0, sizeof(v));
    v.subview = FF_INBOX_SUB_RALLY;
    sig_add_conv(&v, FF_CONV_CREW, 0u, NULL, 0, 0);
    v.rally.on_me_ok = false; /* no fix */
    v.rally.place_count = 0;
    v.rally.can_send = false;
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_inbox_build(parent, &v, false);
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
    ff_scr_radar_build(parent, &r, false, false, /*locked=*/false);

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
    ff_scr_radar_build(parent, &r, false, false, /*locked=*/false);
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
    ff_scr_radar_build(parent, &r, false, false, /*locked=*/false);
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
    ff_scr_flare_build_sender_overlay(parent, &disp, /*screen_flip=*/false);

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

/* S21 AC1 — "Settings is a single scrolling list (no page chip); the back
 * button is pinned and always reachable; every prior row plus Calibrate
 * Touch is present."
 *
 * The "back button" clause is now STALE: S26's horizontal-carousel rework
 * (2026-09-01, well before this S21 slice landed) already retired the
 * Settings back button entirely — see scr_settings.c's own header
 * comment ("There is no BACK control any more — the horizontal-carousel
 * rework made Settings a swipe tile you leave by swiping left, not a
 * modal with a back button") and S21's own header-alignment amendment,
 * which independently confirms the back button was already gone before
 * this slice touched the header. Asserting a BACK control here would be
 * testing a phantom, so this test instead pins what's actually true and
 * current: (a) no page chip, (b) every settings row — every prior row
 * plus CALIBRATE TOUCH — is present AND reachable by scrolling the list
 * into its band, and (c) the pinned header (SETTINGS + name) stays put
 * regardless of scroll position, so the equivalent "always reachable"
 * property (a way back OUT is always on screen, just no longer a
 * dedicated button) still holds. The horizontal-drag-leaves-Settings
 * property itself is S26e's, pinned by S26e_AC3_horizontal_drag_*
 * above and the swipe-navigation tests elsewhing in this file.
 *
 * Mutation check (hand-verified before pushing, per docs/review/
 * code-review.md item 6): commenting out the
 * `settings_build_calibrate_row(...)` call in ff_scr_settings_build
 * (scr_settings.c) in a scratch build fails this test on the
 * CALIBRATE TOUCH presence assertion; see the PR body for the exact
 * `ctest` output. */
static void S21_AC1_settings_is_one_scrolling_list_every_row_reachable(void)
{
    ff_scr_settings_reset_scroll(); /* a fresh entry always starts at the top */

    ff_app_settings_t s;
    memset(&s, 0, sizeof(s));

    ff_scr_settings_build(lv_screen_active(), &s);

    /* No page chip: S21 replaced #105's pagination outright — there is
     * no "1/2"-style indicator left anywhere in the tree. */
    TEST_ASSERT_NULL(find_label_exact(lv_screen_active(), "1/2"));
    TEST_ASSERT_NULL(find_label_exact(lv_screen_active(), "2/2"));

    /* Every prior row's caption/label is present SOMEWHERE in the tree
     * (LVGL builds every child regardless of current scroll position —
     * this alone proves "present", not yet "reachable"). */
    static char const *const kRowLabels[] = {
        "BRIGHTNESS", "UNITS",     "CLOCK",       "SCREEN",      "SHARE",
        "HAPTICS",    "SOUNDS",    "UI TICKS",    "GLOW",        "WATER NUDGE",
        "QUIET HOURS", "COLORBLIND", /* S27 sounds adds SOUNDS + UI TICKS, next to HAPTICS */
    };
    for (size_t i = 0; i < sizeof(kRowLabels) / sizeof(kRowLabels[0]); i++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(find_label_exact(lv_screen_active(), kRowLabels[i]), kRowLabels[i]);
    }
    TEST_ASSERT_NOT_NULL(find_button_with_label(lv_screen_active(), "CALIBRATE TOUCH"));

    /* "Reachable by scrolling": scroll the ONE list all the way down
     * (LVGL clamps to the real content range) and prove the LAST row,
     * CALIBRATE TOUCH, actually lands INSIDE the list's own viewport
     * band at that scroll position — not merely present somewhere
     * off-glass in the object tree. */
    lv_obj_t *list = find_scrollable(lv_screen_active());
    TEST_ASSERT_NOT_NULL_MESSAGE(list, "no scrollable settings list container found");
    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, LV_COORD_MAX, LV_ANIM_OFF);
    lv_obj_update_layout(list);

    lv_obj_t *cal = find_button_with_label(lv_screen_active(), "CALIBRATE TOUCH");
    TEST_ASSERT_NOT_NULL(cal);
    lv_area_t cal_area;
    lv_obj_get_coords(cal, &cal_area);
    lv_area_t list_area;
    lv_obj_get_coords(list, &list_area);
    TEST_ASSERT_TRUE_MESSAGE(cal_area.y1 >= list_area.y1 && cal_area.y2 <= list_area.y2,
                             "CALIBRATE TOUCH did not scroll into the list viewport");

    /* The header (SETTINGS + name) is built directly on the puck, never
     * inside the scroll list, so it is unaffected by scrolling the list
     * to its far end — it is still there, pinned, after the scroll
     * above. There is no dedicated BACK control any more (see this
     * test's own header comment). */
    TEST_ASSERT_NOT_NULL(find_label_exact(lv_screen_active(), "SETTINGS"));
}

/* S21 §3 — the "CALIBRATE TOUCH" row emits the shell-owned
 * FF_INTENT_CALIBRATE_TOUCH (the screen only reports the tap; the shell runs
 * the device crosshair flow via its injected hook, a no-op on the sim). The
 * row is far down the scrolling list, but click() injects the event
 * directly, so its scroll position is irrelevant to this seam test. */
static void S21_AC3_settings_calibrate_touch_row_emits_calibrate_intent(void)
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
    state.active_face = FF_APP_FACE_INBOX;
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
/* S26 slice e, amended 2026-09-01 — the launcher's FIVE circles ->      */
/* LAUNCHER_SELECT (Radar joined with no special treatment; see          */
/* ff_route.h's header note for the model amendment)                     */
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

static void S26e_launcher_radar_circle_emits_index_0(void)
{
    S26e_launcher_circle_click_emits_launcher_select(0, 0u);
}

static void S26e_launcher_lineup_circle_emits_index_1(void)
{
    S26e_launcher_circle_click_emits_launcher_select(1, 1u);
}

static void S26e_launcher_inbox_circle_emits_index_2(void)
{
    S26e_launcher_circle_click_emits_launcher_select(2, 2u);
}

static void S26e_launcher_map_circle_emits_index_3(void)
{
    S26e_launcher_circle_click_emits_launcher_select(3, 3u);
}

static void S26e_launcher_settings_circle_emits_index_4(void)
{
    S26e_launcher_circle_click_emits_launcher_select(4, 4u);
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

    click(launcher_circle_at(2)); /* Signals — index 2 as of the amended 5-circle order */
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
}

/* =================================================================== */
/* debt/batt-low-core — the launcher's status row must tint amber on a  */
/* low reading, exactly like scr_radar.c's status bar. Both screens     */
/* call the SAME core classifier (ff_radar_batt_is_low, ff_radar.h) on  */
/* the SAME ff_radar_view_t.batt_pct field — this test proves the       */
/* RENDERED pixel color, not just that the classifier itself is right   */
/* (core/tests/test_radar.c already pins ff_radar_batt_is_low's         */
/* boundary by literal; this is the render-side half that closes PR     */
/* #177's mutation (b) gap, where the whole-frame golden threshold       */
/* (run_goldens.sh, 0.5% of 412x412 pixels) was too coarse to catch a   */
/* small status-row label/icon color regression in isolation).          */
/* =================================================================== */

static void S26_launcher_status_row_tints_amber_when_battery_low(void)
{
    ff_app_state_t state;

    /* Known, low (<= FF_BATT_LOW_PCT) — icon + "NN%" label both amber. */
    memset(&state, 0, sizeof(state));
    state.radar.batt_pct = 10;
    ff_scr_launcher_build(&state);

    lv_obj_t *low_lbl = find_label_exact(lv_screen_active(), "10%");
    TEST_ASSERT_NOT_NULL(low_lbl);
    TEST_ASSERT_TRUE(lv_color_eq(lv_obj_get_style_text_color(low_lbl, LV_PART_MAIN),
                                  lv_color_hex(FF_THEME_COLOR_STALE_AMBER)));

    /* Known, NOT low — normal muted chrome, not amber. */
    lv_obj_clean(lv_screen_active());
    memset(&state, 0, sizeof(state));
    state.radar.batt_pct = 50;
    ff_scr_launcher_build(&state);

    lv_obj_t *ok_lbl = find_label_exact(lv_screen_active(), "50%");
    TEST_ASSERT_NOT_NULL(ok_lbl);
    TEST_ASSERT_TRUE(lv_color_eq(lv_obj_get_style_text_color(ok_lbl, LV_PART_MAIN),
                                  lv_color_hex(FF_THEME_COLOR_MUTED)));

    /* Unknown (-1, no ADC yet) — honestly "--%%", never amber: unknown   */
    /* never escalates to an alarm (CLAUDE.md "honest data over pretty   */
    /* data"). */
    lv_obj_clean(lv_screen_active());
    memset(&state, 0, sizeof(state));
    state.radar.batt_pct = -1;
    ff_scr_launcher_build(&state);

    lv_obj_t *unknown_lbl = find_label_exact(lv_screen_active(), "--%");
    TEST_ASSERT_NOT_NULL(unknown_lbl);
    TEST_ASSERT_TRUE(lv_color_eq(lv_obj_get_style_text_color(unknown_lbl, LV_PART_MAIN),
                                  lv_color_hex(FF_THEME_COLOR_MUTED)));
}

/* =================================================================== */
/* Radar counterpart of the launcher test above. Review finding (PR      */
/* #177): no radar golden fixture has batt_pct <= FF_BATT_LOW_PCT and    */
/* there was no color-level test for scr_radar.c's own status bar, so a  */
/* hardcoded-muted regression there was caught by NOTHING. Same          */
/* rendered-pixel-color proof, same three cases, on ff_scr_radar_build   */
/* instead of ff_scr_launcher_build. */
/* =================================================================== */

static void S06_radar_battery_tints_amber_when_low(void)
{
    ff_radar_view_t r;

    /* Known, low (<= FF_BATT_LOW_PCT) — the "NN%" status-bar label amber. */
    memset(&r, 0, sizeof(r));
    r.mode = RADAR_LIVE;
    r.batt_pct = 10;
    ff_scr_radar_build(lv_obj_create(lv_screen_active()), &r, false, false, /*locked=*/false);

    lv_obj_t *low_lbl = find_label_exact(lv_screen_active(), "10%");
    TEST_ASSERT_NOT_NULL(low_lbl);
    TEST_ASSERT_TRUE(lv_color_eq(lv_obj_get_style_text_color(low_lbl, LV_PART_MAIN),
                                  lv_color_hex(FF_THEME_COLOR_STALE_AMBER)));

    /* Known, NOT low — normal muted chrome, not amber. */
    lv_obj_clean(lv_screen_active());
    memset(&r, 0, sizeof(r));
    r.mode = RADAR_LIVE;
    r.batt_pct = 50;
    ff_scr_radar_build(lv_obj_create(lv_screen_active()), &r, false, false, /*locked=*/false);

    lv_obj_t *ok_lbl = find_label_exact(lv_screen_active(), "50%");
    TEST_ASSERT_NOT_NULL(ok_lbl);
    TEST_ASSERT_TRUE(lv_color_eq(lv_obj_get_style_text_color(ok_lbl, LV_PART_MAIN),
                                  lv_color_hex(FF_THEME_COLOR_MUTED)));

    /* Unknown (-1, no ADC yet) — honestly "--%%", never amber. */
    lv_obj_clean(lv_screen_active());
    memset(&r, 0, sizeof(r));
    r.mode = RADAR_LIVE;
    r.batt_pct = -1;
    ff_scr_radar_build(lv_obj_create(lv_screen_active()), &r, false, false, /*locked=*/false);

    lv_obj_t *unknown_lbl = find_label_exact(lv_screen_active(), "--%");
    TEST_ASSERT_NOT_NULL(unknown_lbl);
    TEST_ASSERT_TRUE(lv_color_eq(lv_obj_get_style_text_color(unknown_lbl, LV_PART_MAIN),
                                  lv_color_hex(FF_THEME_COLOR_MUTED)));
}

/* ff_scr_launcher_satellite_deg (scr_launcher.h [api]): the N-agnostic
 * satellite-angle formula, tested directly rather than through rendered
 * pixel positions — 0deg = top, clockwise, `compass_pos * (360 / n)`.
 * N=4 (today's real satellite count) yields the four cardinal points;
 * N=5 (the design canvas's own pentagon, hypothetical — this app has no
 * 5th routable satellite yet) yields its 72deg spacing. Proves the
 * FORMULA is N-agnostic independent of anything scr_launcher.c actually
 * renders today. */
static void S26e_satellite_deg_is_n_agnostic(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ff_scr_launcher_satellite_deg(0, 4));
    TEST_ASSERT_EQUAL_FLOAT(90.0f, ff_scr_launcher_satellite_deg(1, 4));
    TEST_ASSERT_EQUAL_FLOAT(180.0f, ff_scr_launcher_satellite_deg(2, 4));
    TEST_ASSERT_EQUAL_FLOAT(270.0f, ff_scr_launcher_satellite_deg(3, 4));

    TEST_ASSERT_EQUAL_FLOAT(0.0f, ff_scr_launcher_satellite_deg(0, 5));
    TEST_ASSERT_EQUAL_FLOAT(72.0f, ff_scr_launcher_satellite_deg(1, 5));
    TEST_ASSERT_EQUAL_FLOAT(144.0f, ff_scr_launcher_satellite_deg(2, 5));
    TEST_ASSERT_EQUAL_FLOAT(216.0f, ff_scr_launcher_satellite_deg(3, 5));
    TEST_ASSERT_EQUAL_FLOAT(288.0f, ff_scr_launcher_satellite_deg(4, 5));

    /* n <= 0 is defensive-only (no real caller reaches it — see this
     * function's own doc comment) but must stay defined, never a
     * divide-by-zero. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ff_scr_launcher_satellite_deg(2, 0));
}

/* PR #145 review — PRESS_LOCK regression guard. A real indev drag that
 * PRESSES DOWN on one launcher circle and releases somewhere else must
 * emit NOTHING: LVGL's default LV_OBJ_FLAG_PRESS_LOCK ("keep the object
 * pressed even if the press slid from the object") would otherwise fire
 * LV_EVENT_CLICKED on the ORIGINAL press target regardless of where the
 * finger ends up — exactly what a horizontal sweep across the puck's
 * own midline does today, since the compass ring's left/right
 * satellites sit exactly on that line by construction (see
 * scr_launcher.c's `launcher_make_satellite`, the PRESS_LOCK comment).
 * This uses the SAME shape as targets/sim's real `ctl swipe` command
 * (ctl_loop.c's ctl_loop_swipe): press at (352,206) — inside the
 * Lineup satellite (idx 1, right cardinal point, x 290-378 / y
 * 162-250) — six move steps, release at (60,206) — inside the Map
 * satellite (idx 3, left cardinal point, x 34-122 / y 162-250).
 *
 * Mutation check (per AGENTS.md's proxy-check discipline): re-adding
 * `lv_obj_add_state`-style PRESS_LOCK (i.e. removing this file's
 * `lv_obj_clear_flag(btn, LV_OBJ_FLAG_PRESS_LOCK)` calls in
 * scr_launcher.c) flips this test from PASS to FAIL — verified by hand
 * before landing this test, see the PR body for the exact output. */
static void S26e_launcher_drag_across_satellites_emits_nothing(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    ff_scr_launcher_build(&state);
    lv_obj_update_layout(lv_screen_active()); /* coords are lazily computed — force it before a coordinate-driven drag
                                                * (code review on PR #173: without this, the satellites' real screen
                                                * coords were never committed before drag()'s hardcoded points were
                                                * dispatched, so the initial press could miss its intended target
                                                * entirely — the test then passed vacuously (count==0) regardless of
                                                * PRESS_LOCK, which the reviewer confirmed by mutation: this test
                                                * stayed green even with PRESS_LOCK left uncleared, until this fix). */

    /* (352, 206) -> (60, 206): press inside Lineup (idx 1, real
     * rendered center verified at (333,205) — 352 is still comfortably
     * inside its 88px box), release inside Map (idx 3). Identical shape
     * to targets/sim's real `ctl swipe` command (ctl_loop.c's
     * ctl_loop_swipe: press at width-60, release at 60, y = height/2). */
    drag(352, 60, 206);
    TEST_ASSERT_EQUAL_INT(0, s_spy.count);
}

/* =================================================================== */
/* PL_ — press-lock drag-off, generalized (this PR).                     */
/*                                                                       */
/* #145 (launcher) and #148 (compose) each found and fixed the same      */
/* LV_OBJ_FLAG_PRESS_LOCK bug independently, per-screen — see             */
/* scr_nav.h's ff_scr_button_create doc comment for the full mechanism.  */
/* This section is the generalized proof: every screen's buttons now go  */
/* through that one shared base, so a drag-off is neutralized EVERYWHERE,*/
/* not just in the two files that had already been bitten. Five sites,   */
/* one per screen named in the bug report plus the launcher (whose OWN   */
/* pre-existing S26e_launcher_drag_across_satellites_emits_nothing,      */
/* just above, turned out — code review on PR #173 — to be missing an    */
/* lv_obj_update_layout() before its coordinate-driven drag, so it        */
/* passed regardless of PRESS_LOCK; fixed there, and this section adds    */
/* its own PL_-named counterpart for symmetry with the other four sites): */
/* Radar's FLARE (the primary CTA), a Settings toggle pill, Power-menu's  */
/* Power off (the one destructive action), an Inbox quick-reply chip, and */
/* a launcher satellite. Each drag-off test is paired with a clean-tap    */
/* positive control so the fix is proven to neutralize a SLIDE without    */
/* breaking an ordinary press — for Radar/Settings/Inbox/Launcher those   */
/* positive controls already exist elsewhere in this file                */
/* (S16_c2_radar_flare_button_emits_flare_start,                         */
/* S11b_settings_units_chip_toggles_imperial, S24c_thread_omw_chip_       */
/* emits_canned_reply_omw, S26e_launcher_lineup_circle_emits_index_1);    */
/* Power-menu had no coverage at all before this PR, so both directions   */
/* are added here.                                                       */
/*                                                                        */
/* Mutation check (hand-verified before pushing, per                     */
/* docs/review/code-review.md item 6 / AGENTS.md's standing brief): with  */
/* ff_scr_button_create (scr_nav.c) edited to NOT clear                   */
/* LV_OBJ_FLAG_PRESS_LOCK, a fresh build fails all five PL_*_drag_off_    */
/* emits_nothing tests below PLUS the fixed                               */
/* S26e_launcher_drag_across_satellites_emits_nothing (each now sees      */
/* s_spy.count == 1, the slide wrongly committing the control) while      */
/* every positive-control test (PL_power_off_tap_emits_power_off          */
/* included) still passes — a clean, stationary tap never depended on     */
/* PRESS_LOCK in the first place. Exact output pasted in this PR's body;  */
/* reverted with a targeted edit, not `git checkout` (per the standing    */
/* brief), and re-verified green.                                        */
/* =================================================================== */

/* (0) Launcher satellite — the site #145 originally fixed, now proven
 * with the same PL_-named shape as the other four sites (the launcher's
 * OWN pre-existing S26e_launcher_drag_across_satellites_emits_nothing,
 * above, is fixed separately — this is its symmetric counterpart, not a
 * duplicate: that test drags horizontally ACROSS two satellites at
 * hardcoded screen coordinates, matching the sim's real `ctl swipe`
 * shape; this one presses a SINGLE satellite by object reference
 * (`launcher_circle_at`, this file's own helper) and drags straight off
 * it, the same shape as the other four PL_ tests). Positive control:
 * S26e_launcher_lineup_circle_emits_index_1, already in this file. */
static void PL_launcher_satellite_drag_off_emits_nothing(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    ff_scr_launcher_build(&state);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *btn = launcher_circle_at(1); /* Lineup satellite */
    TEST_ASSERT_NOT_NULL(btn);
    lv_area_t a;
    lv_obj_get_coords(btn, &a);
    int32_t cx = (a.x1 + a.x2) / 2;
    int32_t cy = (a.y1 + a.y2) / 2;

    /* Press on the satellite's own center, drag 150px down (well past
     * its own 88px diameter), release far away, never back on it. */
    drag_v(cy, cy + 150, cx);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_spy.count,
                                   "a slide-off of a launcher satellite must never commit LAUNCHER_SELECT");
}

/* (a) Radar FLARE — the primary CTA on the CLOSE-mode radar face. */
static void PL_radar_flare_drag_off_emits_nothing(void)
{
    ff_radar_view_t r;
    memset(&r, 0, sizeof(r));
    r.mode = RADAR_CLOSE;
    strncpy(r.name, "DANA", sizeof(r.name) - 1);
    strncpy(r.dist_str, "15 m", sizeof(r.dist_str) - 1);

    /* Sized to the puck and cleared non-scrollable, matching production's
     * `content` container (scr_nav.c) — an UN-sized/default-scrollable
     * `lv_obj_create` here would give this test container itself a huge
     * scroll overflow (its default ~100x50 box against FLARE's real,
     * far-larger extent), letting the drag get swallowed by THIS
     * container's own scroll capture — a mechanism entirely separate
     * from PRESS_LOCK — instead of actually exercising PRESS_LOCK. */
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    ff_scr_radar_build(parent, &r, false, false, /*locked=*/false);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *btn = find_button_with_label(parent, "FLARE");
    TEST_ASSERT_NOT_NULL(btn);
    lv_area_t a;
    lv_obj_get_coords(btn, &a);
    int32_t cx = (a.x1 + a.x2) / 2;
    int32_t cy = (a.y1 + a.y2) / 2;

    /* Press on FLARE's own center, drag 150px down (well past its own
     * FF_THEME_FLARE_BTN_H_PX == 48 height), release far away, never
     * back on FLARE. */
    drag_v(cy, cy + 150, cx);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_spy.count, "a slide-off of FLARE must never commit FLARE_START");
}

/* (b) Settings toggle pill — HAPTICS OFF, one of every pill this face
 * builds through settings_make_pill -> ff_scr_pill_create. */
static void PL_settings_toggle_drag_off_emits_nothing(void)
{
    ff_app_settings_t s;
    memset(&s, 0, sizeof(s));
    s.imperial = true; /* renders "FT" — the units row, at rest within the
                         * unscrolled list viewport (unlike a lower row
                         * such as HAPTICS, which sits below
                         * FF_SETTINGS_LIST_H at scroll offset 0 and so is
                         * never reachable by a real, un-scrolled touch —
                         * matches S11b_settings_units_chip_toggles_imperial's
                         * own fixture, the positive control for this same
                         * chip). */

    ff_scr_settings_build(lv_screen_active(), &s);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *btn = find_button_with_label(lv_screen_active(), "FT");
    TEST_ASSERT_NOT_NULL(btn);
    lv_area_t a;
    lv_obj_get_coords(btn, &a);
    int32_t cx = (a.x1 + a.x2) / 2;
    int32_t cy = (a.y1 + a.y2) / 2;

    /* Horizontal drag, not vertical: the pill's row lives inside
     * Settings' own LV_DIR_VER-scrollable list, so a vertical drag would
     * be ambiguous with the list's OWN scroll-gesture detection (a
     * proxy-check trap this repo's AGENTS.md standing brief warns
     * about) — a horizontal slide off the pill exercises PRESS_LOCK
     * alone, since the list never claims a horizontal drag. */
    drag(cx, cx - 150, cy);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_spy.count, "a slide-off of a settings pill must never commit SETTING_SET");
}

/* (c) Power-menu Power off — the one destructive action on this face,
 * and the site with NO prior test coverage at all (both directions
 * added here, not just the drag-off). */
static void PL_power_off_drag_off_emits_nothing(void)
{
    ff_scr_power_menu_build();
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *btn = find_button_with_label(lv_screen_active(), "POWER OFF");
    TEST_ASSERT_NOT_NULL(btn);
    lv_area_t a;
    lv_obj_get_coords(btn, &a);
    int32_t cx = (a.x1 + a.x2) / 2;
    int32_t cy = (a.y1 + a.y2) / 2;

    drag_v(cy, cy + 150, cx);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_spy.count, "a slide-off of POWER OFF must never commit FF_INTENT_POWER_OFF");
}

static void PL_power_off_tap_emits_power_off(void)
{
    ff_scr_power_menu_build();

    click(find_button_with_label(lv_screen_active(), "POWER OFF"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_POWER_OFF, s_spy.last.kind);
}

/* (d) Inbox quick-reply chip (OMW) — the same fixture
 * S24c_thread_omw_chip_emits_canned_reply_omw already uses. */
static void PL_inbox_chip_drag_off_emits_nothing(void)
{
    ff_app_inbox_t v;
    s24c_make_direct_thread(&v);

    /* Sized to the puck and cleared non-scrollable — see the matching
     * comment on PL_radar_flare_drag_off_emits_nothing above for why an
     * un-sized default `lv_obj_create` here would falsely absorb this
     * drag as ITS OWN scroll, never actually reaching PRESS_LOCK. */
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    ff_scr_inbox_build(parent, &v, false);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *btn = find_button_with_label(parent, "OMW");
    TEST_ASSERT_NOT_NULL(btn);
    lv_area_t a;
    lv_obj_get_coords(btn, &a);
    int32_t cx = (a.x1 + a.x2) / 2;
    int32_t cy = (a.y1 + a.y2) / 2;

    drag_v(cy, cy + 150, cx);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_spy.count, "a slide-off of OMW must never commit CANNED_REPLY");
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S26e_AC3_horizontal_drag_on_radar_emits_nothing);
    RUN_TEST(S26e_AC3_horizontal_drag_on_lineup_emits_nothing);
    RUN_TEST(S26e_AC3_horizontal_drag_on_inbox_emits_nothing);
    RUN_TEST(carousel_physical_upward_drag_emits_nothing);
    RUN_TEST(carousel_physical_upward_drag_emits_nothing_from_inbox_too);
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
    RUN_TEST(S99_compose_tap_on_space_center_emits_space_never_send);
    RUN_TEST(S99_compose_send_full_area_tap_emits_send_exactly_once);
    RUN_TEST(S99_compose_space_and_send_clear_the_maintainer_gap_ask);
    RUN_TEST(S99_compose_every_key_has_press_state_feedback);
    RUN_TEST(S99_compose_pred_candidates_have_press_state_feedback);
    RUN_TEST(S99_compose_drag_off_send_emits_nothing);
    RUN_TEST(S99_compose_drag_off_key_emits_nothing);
    RUN_TEST(S99_compose_send_corner_clears_bezel_margin_bar);
    RUN_TEST(S99_compose_to_short_name_renders_in_full_no_dots);
    RUN_TEST(S99_compose_to_every_demo_crew_name_renders_in_full);
    RUN_TEST(S99_compose_to_long_name_ellipsizes_cleanly);
    RUN_TEST(S99_compose_pred_mode_shows_recipient_on_draft_line);
    RUN_TEST(S99_compose_space_del_mode_pinned_dimensions);
    RUN_TEST(S99_compose_mode_header_chip_clears_floor_and_gaps);
    RUN_TEST(S99_compose_pred_strip_drag_scrolls_not_selects);
    RUN_TEST(S99_compose_pred_strip_tap_after_scroll_selects_right_word);
    RUN_TEST(S99_compose_pred_strip_scroll_resets_on_new_prediction_set);
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
    RUN_TEST(S24_crew_thread_overflow_scrolls_on_drag);
    RUN_TEST(S24_direct_thread_overflow_scrolls_on_drag);
    RUN_TEST(S24_thread_message_bubble_not_compressed);
    RUN_TEST(S24_crew_thread_shows_at_least_4_full_rows_at_rest);
    RUN_TEST(S24_direct_thread_shows_at_least_4_rows_at_rest);
    RUN_TEST(S24_crew_thread_no_row_ever_under_the_fab_slice);
    RUN_TEST(S24_widest_fixture_message_does_not_truncate);
    RUN_TEST(S24_rally_where_list_scrolls_to_reach_all_rows);
    RUN_TEST(S24_thread_scroll_preserved_across_same_thread_rebuild);
    RUN_TEST(S24_thread_scroll_resets_to_newest_on_new_message);
    RUN_TEST(S24_thread_scroll_resets_to_newest_on_different_thread);
    RUN_TEST(S24_omw_chip_real_touch_on_long_overflowing_1to1_thread);
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
    RUN_TEST(S21_AC1_settings_is_one_scrolling_list_every_row_reachable);
    RUN_TEST(S21_AC3_settings_calibrate_touch_row_emits_calibrate_intent);
    RUN_TEST(S16_c1_wired_sites_are_noops_while_the_seam_is_unbound);

    RUN_TEST(S26e_launcher_radar_circle_emits_index_0);
    RUN_TEST(S26e_launcher_lineup_circle_emits_index_1);
    RUN_TEST(S26e_launcher_inbox_circle_emits_index_2);
    RUN_TEST(S26e_launcher_map_circle_emits_index_3);
    RUN_TEST(S26e_launcher_settings_circle_emits_index_4);
    RUN_TEST(S26e_launcher_click_emits_exactly_one_intent);
    RUN_TEST(S26e_satellite_deg_is_n_agnostic);
    RUN_TEST(S26e_launcher_drag_across_satellites_emits_nothing);

    RUN_TEST(S26_launcher_status_row_tints_amber_when_battery_low);
    RUN_TEST(S06_radar_battery_tints_amber_when_low);

    RUN_TEST(PL_launcher_satellite_drag_off_emits_nothing);
    RUN_TEST(PL_radar_flare_drag_off_emits_nothing);
    RUN_TEST(PL_settings_toggle_drag_off_emits_nothing);
    RUN_TEST(PL_power_off_drag_off_emits_nothing);
    RUN_TEST(PL_power_off_tap_emits_power_off);
    RUN_TEST(PL_inbox_chip_drag_off_emits_nothing);

    return UNITY_END();
}
