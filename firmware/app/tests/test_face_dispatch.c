/**
 * test_face_dispatch.c — app/ff_face_dispatch.c coverage (debt/shared-
 * face-dispatch).
 *
 * Both targets/sim/face_dispatch.c and targets/esp32s3/main/ff_face.c
 * used to hand-roll this exact mapping twice; this is the test the
 * shared chain never had anywhere (each target's copy was only ever
 * proven indirectly — the sim through goldens, the device not at all).
 * Screen-builder + label-search technique, same shape as targets/sim/
 * tests/test_scr_inbox_hint.c's find_label_with_text: build the REAL
 * screen for a given `ff_app_state_t`, then assert on a label unique to
 * the face that was supposed to render.
 *
 * LVGL init/deinit in real setUp/tearDown (not a per-test-case pairing):
 * setUp() creates a fresh headless display and sets it default; tearDown
 * deinits it, GUARDED by lv_is_initialized() — the harness-hang lesson
 * targets/sim/tests/test_wakeonly_touch.c's own tearDown documents in
 * full (a failed TEST_ASSERT longjmps past any explicit teardown code in
 * the test body itself, but Unity always still runs tearDown() — without
 * the guard, a SECOND lv_deinit() on top of one that already ran cleanly
 * is undefined behavior; with it, tearDown is idempotent either way).
 *
 * MINIMAL STATE, NOT FIXTURES: every case below builds its own
 * `ff_app_state_t = {0}` and sets only the fields the face under test
 * needs, rather than loading a JSON fixture (this directory has no
 * fixture loader — that lives in targets/sim, an app/ dependency this
 * file must not take). Zeroed core sections render each face's genuine
 * EMPTY/unset state, which is exactly what's needed to land on a fixed,
 * deterministic label per face (see per-test comments for why each
 * chosen label is safe against the zeroed state).
 */
#include <string.h>

#include "unity.h"

#include "ff_app_state.h"
#include "ff_face_dispatch.h"
#include "ff_theme.h" /* FF_THEME_WINDOW_PX — setUp's headless display size */
#include "lvgl.h"
#include "scr_settings.h" /* ff_scr_settings_apply_scroll_hint — the sim-shaped scroll hint hook */

/* Frozen tick — nothing here renders a frame or calls lv_refr_now(), but
 * lv_init still wants a tick source (same as test_scr_flare.c). */
static uint32_t test_tick_cb(void)
{
    return 0;
}

void setUp(void)
{
    lv_init();
    lv_tick_set_cb(test_tick_cb);
    /* No buffers/flush callback: nothing here calls lv_refr_now() or
     * lv_timer_handler(); building + lv_obj_update_layout() (which the
     * Settings scroll-reset test needs) never touches the flush path. */
    lv_display_t *disp = lv_display_create(FF_THEME_WINDOW_PX, FF_THEME_WINDOW_PX);
    lv_display_set_default(disp);
}

void tearDown(void)
{
    if (lv_is_initialized()) {
        lv_deinit();
    }
}

/* Same recursive lookup as test_scr_inbox_hint.c's find_label_with_text
 * (duplicated rather than shared — no shared-header dependency for one
 * small helper, same convention every sibling copy in this codebase's
 * test suite already follows). */
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

/* Walks the tree for any object with a nonzero vertical scroll offset —
 * used by the Settings scroll-reset test below, which has no other way
 * to observe scr_settings.c's file-static `s_scroll_y` (there is no
 * getter; `ff_scr_settings_apply_scroll_hint` is the only public seam
 * that touches it, and it's write-only from the caller's point of view).
 * Only the Settings list is ever scrolled in this file's tests, so "any
 * scrolled object" is unambiguous. */
static lv_obj_t *find_scrolled_object(lv_obj_t *root)
{
    if (lv_obj_get_scroll_y(root) != 0) {
        return root;
    }
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *found = find_scrolled_object(lv_obj_get_child(root, i));
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

/* The sim's own hook (targets/sim/face_dispatch.c) forwards straight to
 * scr_settings.h's golden-harness scroll hint — reused verbatim here so
 * this test drives the exact real seam a fixture-driven golden does,
 * not a stand-in for it. */
static void settings_scroll_hint_hook(void *user_data, int32_t y)
{
    (void)user_data;
    ff_scr_settings_apply_scroll_hint(y);
}

/* Spy for the unknown-face hook: records whether it fired and with what
 * state pointer, WITHOUT building anything (mirrors the device's real
 * ESP_LOGW tail — a log line, not a screen). */
static int s_unknown_face_calls;
static ff_app_state_t const *s_unknown_face_last_state;

static void unknown_face_spy(void *user_data, ff_app_state_t const *state)
{
    (void)user_data;
    s_unknown_face_calls++;
    s_unknown_face_last_state = state;
}

/* ---------------------------------------------------------------------
 * Each active_face value builds the expected screen.
 *
 * Every label chosen below is what the named face renders from a fully
 * ZEROED `ff_app_state_t` (no crew paired, no fix, no draft typed, ...)
 * — the honest "nothing selected/known yet" state every face already
 * has real, deterministic content for (CLAUDE.md: "unknown = explicitly
 * unknown"), so no fixture JSON is needed to pin it. See each case's own
 * comment for the exact screen-builder line the label comes from.
 * ------------------------------------------------------------------- */

/* `*state` is caller-populated (active_face plus whatever section fields
 * that face's zeroed-default label needs — see each call site's
 * comment); this helper only runs the build and the assertion. */
static void assert_state_builds_label(ff_app_state_t const *state, char const *label, char const *case_name)
{
    lv_obj_clean(lv_screen_active()); /* same lv_obj_clean + rebuild the real render loop uses (app_main.c) */

    ff_face_dispatch_ctx_t ctx = FF_FACE_DISPATCH_CTX_INIT;
    ff_face_dispatch_build(state, &ctx, NULL);

    lv_obj_t *found = find_label_with_text(lv_screen_active(), label);
    TEST_ASSERT_NOT_NULL_MESSAGE(found, case_name);
}

static void assert_face_builds_label(ff_app_face_t face, char const *label, char const *case_name)
{
    ff_app_state_t state = {0};
    state.active_face = face;
    assert_state_builds_label(&state, label, case_name);
}

static void S_face_dispatch_each_active_face_builds_the_expected_screen(void)
{
    /* RADAR: scr_radar.c's radar_render_nosel. `radar.mode` must be set
     * explicitly — RADAR_LIVE is enum value 0 (core/include/ff_radar.h),
     * so a bare zeroed `ff_radar_view_t` would take the LIVE path, not
     * NOSEL; RADAR_NOSEL ("no member paired") is the honest zero-state
     * this test actually wants, set the same way ff_radar_compute()
     * itself would for "no crew selected". */
    {
        ff_app_state_t radar_case = {0};
        radar_case.active_face = FF_APP_FACE_RADAR;
        radar_case.radar.mode = RADAR_NOSEL;
        assert_state_builds_label(&radar_case, "NO CREW SELECTED", "RADAR");
    }

    /* LINEUP (Now): scr_lineup.c's lineup_render_no_pack — `now.state`
     * defaults to NOW_NO_PACK (enum value 0, ff_app_state.h), the
     * honest "no festpack loaded at all" zero-state. */
    assert_face_builds_label(FF_APP_FACE_LINEUP, "NO FESTIVAL LOADED", "LINEUP");

    /* INBOX (Signals): scr_inbox.c's honest no-crew hint (S24 AC9) —
     * n<=1 (the ever-present CREW row alone) with a zeroed inbox. */
    assert_face_builds_label(FF_APP_FACE_INBOX, "NO CREW LINKED YET", "INBOX");

    /* MAP: scr_map.c's map_draw_you — no position fix (zeroed
     * you_has_pos) always draws the fixed "NO FIX" status chip. */
    assert_face_builds_label(FF_APP_FACE_MAP, "NO FIX", "MAP");

    /* SETTINGS: scr_settings.c's brightness section caption — drawn
     * unconditionally regardless of the settings values. */
    assert_face_builds_label(FF_APP_FACE_SETTINGS, "BRIGHTNESS", "SETTINGS");

    /* COMPOSE: scr_compose.c's placeholder — an empty draft (zeroed
     * compose.text, no pending char) renders "Type a message...". */
    assert_face_builds_label(FF_APP_FACE_COMPOSE, "Type a message...", "COMPOSE");

    /* POWER_MENU: scr_power_menu.c's fixed headline — no state
     * parameter, always the same content. */
    assert_face_builds_label(FF_APP_FACE_POWER_MENU, "POWER", "POWER_MENU");

    /* LAUNCHER: scr_launcher.c's Radar hub caption — fixed text, always
     * drawn. */
    assert_face_builds_label(FF_APP_FACE_LAUNCHER, "RADAR", "LAUNCHER");
}

/* ---------------------------------------------------------------------
 * Takeover wins over ANY active_face (S10 slice b — "full-screen
 * takeover regardless of current face"). RADAR is an arbitrary non-FLARE
 * active_face chosen to prove the takeover check runs FIRST, before
 * active_face is even consulted: RADAR's own "NO CREW SELECTED" content
 * must NOT appear once a takeover is pending.
 * ------------------------------------------------------------------- */
static void S_face_dispatch_takeover_wins_over_any_active_face(void)
{
    ff_app_state_t state = {0};
    state.active_face = FF_APP_FACE_RADAR;
    state.radar.mode = RADAR_NOSEL; /* would genuinely render "NO CREW SELECTED" if takeover did NOT win —
                                      * the proxy-check rule (AGENTS.md item 6): RADAR_LIVE (enum 0, a bare
                                      * zeroed radar view) would never show that label regardless of
                                      * takeover, making the negative assertion below vacuous. */
    state.flare.takeover_active = true;
    /* takeover_bearing_valid stays false (zeroed) and takeover_dist_str
     * stays empty -> ff_scr_flare_build_takeover's honest fallback line,
     * a deterministic label with no fixture needed (see that function's
     * own comment on why 0.0 can't stand in for "unknown"). */

    ff_face_dispatch_ctx_t ctx = FF_FACE_DISPATCH_CTX_INIT;
    ff_face_dispatch_build(&state, &ctx, NULL);

    lv_obj_t *takeover_line = find_label_with_text(lv_screen_active(), "bearing unknown - -- m");
    TEST_ASSERT_NOT_NULL_MESSAGE(takeover_line, "takeover bearing/distance line must render");

    lv_obj_t *radar_line = find_label_with_text(lv_screen_active(), "NO CREW SELECTED");
    TEST_ASSERT_NULL_MESSAGE(radar_line, "RADAR's own content must NOT render under a takeover");
}

/* ---------------------------------------------------------------------
 * #bug4 — the Settings scroll reset fires ONLY on a fresh entry (a
 * not-Settings -> Settings transition through the SAME ctx), never on a
 * same-face rebuild. Proven against the REAL scroll state (via the
 * sim's own golden-harness hook, `ff_scr_settings_apply_scroll_hint`),
 * not just by inspecting which internal function got called — this is
 * exactly the class of test AGENTS.md's proxy-check rule asks for:
 * "what input satisfies the proxy and violates the property?" A test
 * that only checked "was ff_scr_settings_reset_scroll called" could
 * still pass if the SUBSEQUENT build ignored s_scroll_y; this checks
 * the actual rendered scroll offset instead.
 * ------------------------------------------------------------------- */
static void S_face_dispatch_settings_scroll_resets_on_fresh_entry_not_on_rerender(void)
{
    ff_face_dispatch_ctx_t ctx = FF_FACE_DISPATCH_CTX_INIT;
    ff_face_dispatch_hooks_t const hooks = {
        .settings_scroll_hint = settings_scroll_hint_hook,
        .unknown_face = NULL,
        .user_data = NULL,
    };

    /* 1) Fresh entry into Settings (prev_face starts at NONE) — reset is
     * a no-op here (already at 0), then the fixture-only hint scrolls
     * the list to a real, nonzero offset (mirrors a scrolled golden
     * fixture's ui_settings_scroll_y — see tests/fixtures/settings_
     * scrolled_mid.json). */
    ff_app_state_t state = {0};
    state.active_face = FF_APP_FACE_SETTINGS;
    state.ui_settings_scroll_y = 200;
    ff_face_dispatch_build(&state, &ctx, &hooks);

    lv_obj_t *scrolled = find_scrolled_object(lv_screen_active());
    TEST_ASSERT_NOT_NULL_MESSAGE(scrolled, "the scroll hint must have moved the list");

    /* 2) Same-face rebuild (a settings toggle would trigger exactly this
     * — same ctx, active_face still SETTINGS, no fresh transition). The
     * live shell always passes ui_settings_scroll_y == 0 on a real
     * rebuild (only the fixture harness path passes a hint) — the
     * offset must survive from the file-static s_scroll_y the first
     * build left behind, NOT from this hint. */
    lv_obj_clean(lv_screen_active()); /* same lv_obj_clean + rebuild the real render loop uses (app_main.c) */
    state.ui_settings_scroll_y = 0;
    ff_face_dispatch_build(&state, &ctx, &hooks);

    lv_obj_t *still_scrolled = find_scrolled_object(lv_screen_active());
    TEST_ASSERT_NOT_NULL_MESSAGE(still_scrolled, "a same-face rebuild must PRESERVE the scroll offset");

    /* 3) Leave Settings, then re-enter — a genuine fresh transition
     * through the SAME ctx must reset to the top, even though nothing
     * passes a scroll hint this time. */
    lv_obj_clean(lv_screen_active());
    state.active_face = FF_APP_FACE_RADAR;
    ff_face_dispatch_build(&state, &ctx, &hooks);

    lv_obj_clean(lv_screen_active());
    state.active_face = FF_APP_FACE_SETTINGS;
    state.ui_settings_scroll_y = 0;
    ff_face_dispatch_build(&state, &ctx, &hooks);

    lv_obj_t *reset_again = find_scrolled_object(lv_screen_active());
    TEST_ASSERT_NULL_MESSAGE(reset_again, "a fresh not-Settings -> Settings entry must reset to the top");
}

/* ---------------------------------------------------------------------
 * An unknown face (one this dispatcher's chain does not recognize)
 * invokes the unknown_face hook and builds nothing else — matching the
 * device adapter's real behavior (ESP_LOGW, no screen). FF_APP_FACE_NONE
 * is used as the unknown value: it is a legitimate enum member but not
 * one the dispatch chain ever matches (see ff_app_state.h's own doc
 * comment: a failed fixture load leaves active_face at NONE, "a state no
 * caller may render" — exactly the "unmapped" case this hook exists
 * for).
 * ------------------------------------------------------------------- */
static void S_face_dispatch_unknown_face_invokes_the_warn_hook_and_builds_nothing(void)
{
    s_unknown_face_calls = 0;
    s_unknown_face_last_state = NULL;

    ff_app_state_t state = {0};
    state.active_face = FF_APP_FACE_NONE;

    ff_face_dispatch_ctx_t ctx = FF_FACE_DISPATCH_CTX_INIT;
    ff_face_dispatch_hooks_t const hooks = {
        .settings_scroll_hint = NULL,
        .unknown_face = unknown_face_spy,
        .user_data = NULL,
    };
    ff_face_dispatch_build(&state, &ctx, &hooks);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_unknown_face_calls, "unknown_face must fire exactly once");
    TEST_ASSERT_EQUAL_PTR_MESSAGE(&state, s_unknown_face_last_state, "unknown_face must receive the same state pointer");

    /* Nothing else should have been built — no label from any real
     * screen should exist under a fresh, empty screen. RADAR's fallback
     * label is a safe negative probe: it is the one thing scr_nav.c's
     * OWN internal `default` case would draw if this dispatcher ever
     * fell through to ff_scr_nav_build by mistake for an unmapped face. */
    lv_obj_t *stray = find_label_with_text(lv_screen_active(), "NO CREW SELECTED");
    TEST_ASSERT_NULL_MESSAGE(stray, "an unknown face must not fall back to any real screen build");
}

/* ---------------------------------------------------------------------
 * hooks == NULL is safe for every branch that would otherwise consult a
 * hook: a Settings build (scroll hint slot) and an unmapped face
 * (unknown_face slot). Passing NULL and completing without crashing —
 * plus the base-face assertions in the earlier test already build every
 * face with hooks == NULL and pass — is the whole proof.
 * ------------------------------------------------------------------- */
static void S_face_dispatch_hooks_null_is_safe(void)
{
    ff_face_dispatch_ctx_t ctx = FF_FACE_DISPATCH_CTX_INIT;

    ff_app_state_t settings_state = {0};
    settings_state.active_face = FF_APP_FACE_SETTINGS;
    settings_state.ui_settings_scroll_y = 100; /* would call the (absent) hook if not guarded */
    ff_face_dispatch_build(&settings_state, &ctx, NULL);
    lv_obj_t *brightness = find_label_with_text(lv_screen_active(), "BRIGHTNESS");
    TEST_ASSERT_NOT_NULL_MESSAGE(brightness, "Settings must still build with hooks == NULL");

    lv_obj_clean(lv_screen_active());
    ff_app_state_t unknown_state = {0};
    unknown_state.active_face = FF_APP_FACE_NONE;
    ff_face_dispatch_build(&unknown_state, &ctx, NULL); /* must not crash */

    /* ctx == NULL is documented safe too (degrades to "always fresh") —
     * exercised here for completeness alongside hooks == NULL. */
    lv_obj_clean(lv_screen_active());
    ff_app_state_t radar_state = {0};
    radar_state.active_face = FF_APP_FACE_RADAR;
    radar_state.radar.mode = RADAR_NOSEL; /* see the RADAR case above for why this must be explicit */
    ff_face_dispatch_build(&radar_state, NULL, NULL);
    lv_obj_t *nosel = find_label_with_text(lv_screen_active(), "NO CREW SELECTED");
    TEST_ASSERT_NOT_NULL_MESSAGE(nosel, "ctx == NULL must still build the face");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S_face_dispatch_each_active_face_builds_the_expected_screen);
    RUN_TEST(S_face_dispatch_takeover_wins_over_any_active_face);
    RUN_TEST(S_face_dispatch_settings_scroll_resets_on_fresh_entry_not_on_rerender);
    RUN_TEST(S_face_dispatch_unknown_face_invokes_the_warn_hook_and_builds_nothing);
    RUN_TEST(S_face_dispatch_hooks_null_is_safe);
    return UNITY_END();
}
