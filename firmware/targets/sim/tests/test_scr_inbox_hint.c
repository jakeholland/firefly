/**
 * test_scr_inbox_hint.c — S24 AC9's honest no-crew hint, at the model
 * level (debt/test-harness PR).
 *
 * AC9 ("no crew linked" is said honestly, never a blank face) was
 * previously covered ONLY by golden pixels (tests/golden/inbox_no_crew
 * .png, inbox_quiet.png, inbox_all_stale.png against the fixtures of the
 * same name) — a pixel-diff against itself never fails if the hint text
 * or its visibility condition silently drifts to something else that
 * happens to render the same committed PNG width/position; this is the
 * exact "vacuous golden" gap docs/specs/S14-testing-ci.md's own goldens
 * tooling doesn't close on its own. This adds a direct assertion.
 *
 * WHERE THE DECISION ACTUALLY LIVES (found per this file's own PR brief:
 * scr_inbox.c:~1031's comment points at it). `inbox_build_inbox`
 * (app/screens/scr_inbox.c) computes `n = ff_inbox_conv_count(&v->inbox)`
 * and calls `inbox_build_no_crew_hint` when `n <= 1` (the ever-present
 * CREW row plus zero member conversations) — a plain `if` over core-
 * shaped data, sitting in the SCREEN file rather than in core/shell.
 * That is CLAUDE.md's own house rule ("If you're writing an `if` about
 * domain behavior inside a screen file, it belongs in core") being
 * violated by production code this PR does not touch or fix — flagged
 * here, per this PR's brief, as a finding for the orchestrator rather
 * than a fix: moving the `n <= 1` decision (and the hint strings) into
 * core/shell so it's testable without an LVGL tree walk is a real,
 * separate follow-up.
 *
 * Given that, this test proves the decision the only way available
 * without touching scr_inbox.c: build the REAL screen for each of the
 * three golden fixtures that already exist for this exact behavior
 * (tests/fixtures/inbox_no_crew.json, inbox_quiet.json,
 * inbox_all_stale.json) via the same in-process `ff_build_face_screen`
 * path test_face_hit_targets.c/test_map_circle_containment.c use (no
 * SDL, no ffsim subprocess), then walk the built LVGL tree for the
 * hint's own label text — a screen-builder + label-search technique,
 * same shape as app/screens/tests/test_scr_intent.c's find_button_with_
 * label (duplicated here rather than shared, same "no shared-header
 * dependency for one small helper" reasoning every sibling copy in this
 * suite already gives).
 *
 * THE PROXY THIS CLOSES (AGENTS.md standing rule item 6): "no crew"
 * could easily be mis-implemented as a proxy like "no messages yet" or
 * "no unread" — inbox_quiet.json and inbox_all_stale.json are exactly
 * the fixtures that would trip such a proxy (real paired members with
 * zero item_count / all-LOST presence, respectively) while genuinely
 * HAVING crew linked. Asserting the hint is ABSENT on those two fixtures
 * (not just present on inbox_no_crew) is what actually pins the n<=1
 * condition rather than some correlated-but-wrong stand-in for it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "face_dispatch.h"
#include "ff_app_state.h"
#include "ff_theme.h"
#include "fixture.h"

#ifndef FF_FIXTURE_DIR
#define FF_FIXTURE_DIR "tests/fixtures/"
#endif

void setUp(void) {}

/* P0 harness-hang fix (debt/test-harness PR), same shape as this
 * directory's sibling test files — see tests/test_wakeonly_touch.c's
 * own tearDown comment for the full repro/verification writeup. This
 * file's one test pairs its own lv_init()/lv_deinit() per fixture (a
 * loop, same shape as test_face_hit_targets.c's sweep_fixture), so a
 * failed TEST_ASSERT partway through one fixture's build would longjmp
 * past that fixture's lv_deinit() without this safety net. */
void tearDown(void)
{
    if (lv_is_initialized()) {
        lv_deinit();
    }
}

static uint32_t inbox_hint_tick_cb(void)
{
    return 0; /* frozen clock — same determinism story as run_goldens.sh --mock-clock */
}

static void inbox_hint_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

/* Same recursive lookup as test_scr_intent.c's find_button_with_label,
 * but over EVERY object (not just labels inside a button) — the no-crew
 * hint's headline is a plain lv_label_create child of an lv_obj_create
 * box, never a button. */
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

/* Builds the real INBOX screen for one fixture and asserts whether the
 * no-crew hint's headline (and, when present, its exact sub-line) shows
 * up in the built LVGL tree. */
static void assert_no_crew_hint(char const *fixture_name, bool expect_visible)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s%s.json", FF_FIXTURE_DIR, fixture_name);

    lv_init();
    lv_tick_set_cb(inbox_hint_tick_cb);

    int32_t const w = FF_THEME_WINDOW_PX;
    int32_t const h = FF_THEME_WINDOW_PX;
    uint32_t const buf_size = (uint32_t)(w * h * 4);
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf, fixture_name);

    lv_display_t *disp = lv_display_create(w, h);
    lv_display_set_buffers(disp, buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, inbox_hint_flush_cb);
    lv_display_set_default(disp);

    ff_app_state_t state;
    ff_fixture_result_t fr = ff_fixture_load_file(path, &state);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FF_FIXTURE_OK, fr, path);
    TEST_ASSERT_EQUAL_MESSAGE(FF_APP_FACE_INBOX, state.active_face, fixture_name);

    ff_build_face_screen(&state);
    lv_refr_now(disp);

    lv_obj_t *headline = find_label_with_text(lv_screen_active(), "NO CREW LINKED YET");
    if (expect_visible) {
        TEST_ASSERT_NOT_NULL_MESSAGE(headline, fixture_name);
        lv_obj_t *sub = find_label_with_text(lv_screen_active(), "Paired friends show up here");
        TEST_ASSERT_NOT_NULL_MESSAGE(sub, fixture_name);
    } else {
        TEST_ASSERT_NULL_MESSAGE(headline, fixture_name);
    }

    free(buf);
    lv_deinit();
}

/* S24 AC9 — the honest no-crew hint's exact text and visibility across
 * the three states golden pixels already cover: shown (with its exact
 * headline + sub-line) with no crew linked at all, and absent — not a
 * false positive — the moment even one member conversation exists,
 * whether that conversation is quiet (no messages yet) or all-stale
 * (every member LOST). */
static void S24_AC9_no_crew_hint_text_and_visibility_across_the_three_states(void)
{
    assert_no_crew_hint("inbox_no_crew", true);
    assert_no_crew_hint("inbox_quiet", false);
    assert_no_crew_hint("inbox_all_stale", false);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S24_AC9_no_crew_hint_text_and_visibility_across_the_three_states);
    return UNITY_END();
}
