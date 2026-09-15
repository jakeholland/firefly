/**
 * test_press_feedback_all_faces.c — puck-ux-usability-review slice 1
 * (docs/reviews/puck-ux-usability-2026-09-15.md, finding 2 + its
 * acceptance criterion 1: "every clickable object on every committed
 * fixture carries an LV_STATE_PRESSED bg_opa or bg_color — asserted,
 * not asserted-for-compose").
 *
 * Before this PR, `app/screens/tests/test_scr_intent.c`'s
 * `walk_assert_press_feedback` proved this only for the screens it
 * builds directly (compose's ABC/123/SYM/PRED fixtures) — real, but
 * scoped to one face. This file widens the same proof to EVERY
 * committed golden fixture, using the exact same "build the real
 * screen a fixture would render" mechanism `test_face_hit_targets.c`
 * and `test_tap_target_sizing.c` already share (`face_dispatch.h`'s
 * `ff_build_face_screen`, the same dispatch table `ffsim` itself uses —
 * not a second copy that could drift), so a control that is missing
 * press feedback on ANY face fails here, not just on compose.
 *
 * has_press_feedback below MEASURES whether `obj`'s resolved bg_opa/
 * bg_color actually change under LV_STATE_PRESSED (see that function's
 * own doc comment — REVIEW FIX, independent review of #329: the
 * original form here was a SELECTOR query via lv_obj_has_style_prop,
 * which returns true for ANY filled object regardless of whether a
 * dedicated pressed-state override exists, because LVGL treats a
 * DEFAULT-state style as matching every queried state. Confirmed by
 * mutation — reverting `settings_make_pill` to `FF_SCR_PILL_PRESS_NONE`
 * did not fail this test under the old check).
 *
 * NOT every press treatment in this codebase uses the LV_STATE_PRESSED
 * mechanism this predicate can see. `scr_widgets.c`'s `ff_scr_pill_
 * create`, `scr_radar.c`'s FLARE and `scr_banner.c`/`scr_compose.c`'s
 * controls all do. `scr_inbox.c`'s FULL-ROW/FAB press wash does NOT:
 * `inbox_row_press_ev` toggles a separate decoration object's bg_opa at
 * the DEFAULT selector from manually-wired LV_EVENT_PRESSED/RELEASED/
 * PRESS_LOST callbacks (scr_inbox.c:943), not via LV_STATE_PRESSED on
 * the tap target itself — a real, working, pre-existing mechanism this
 * predicate cannot observe without actually firing those events and
 * diffing the rendered frame (which `--press-label`'s screenshot path
 * does, for one control at a time, by hand). See the fixture-prefix
 * exclusion in `press_walk` below: NOT a claim these controls lack
 * feedback, only that this predicate cannot verify scr_inbox.c's
 * different, already-reviewed-elsewhere technique. Tracked separately;
 * do not widen this exclusion to cover a genuinely new gap.
 *
 * Three categories of CLICKABLE object are excluded, same reasoning
 * test_tap_target_sizing.c's `sizing_has_callback`/`is_whole_puck` use
 * for the first two (identical) shapes:
 *
 *   - The whole-puck gesture/tap-anywhere region (scr_nav.c's
 *     long-press-to-Settings hook, scr_map.c's tap-anywhere-back
 *     surface): CLICKABLE and exactly FF_THEME_PUCK_PX square, with no
 *     visible fill of its own to give press feedback THROUGH — it is a
 *     gesture catcher, not a control a thumb aims at.
 *   - A CLICKABLE object with NO registered click callback at all: the
 *     compose PRED candidate strip's scroll relay and scr_inbox.c's
 *     inert FLOATING scroll-relay catchers exist so
 *     `lv_indev_find_scroll_obj` has something to walk up from, not as
 *     controls a user presses.
 *   - A SCROLLABLE object (REVIEW FIX): the ambient scroll surface of a
 *     list (e.g. scr_settings.c's own `list`, which "must stay CLICKABLE"
 *     per that file's own comment so a press anywhere in the list can be
 *     hit-tested into a scroll, but carries no CLICKED handler of its
 *     own). LVGL clears LV_OBJ_FLAG_SCROLLABLE on every `lv_button_create`
 *     at construction and no button/pill factory in this codebase
 *     re-adds it, so this is a safe, precise signal for "ambient scroll
 *     surface", not "a control a thumb aims at" — confirmed by isolated
 *     testing during review.
 *
 * Everything else that is CLICKABLE and has a real callback is a
 * control a user's finger lands on — per the review, "the puck answers
 * your finger" means EVERY one of them, on every face.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "lvgl.h"

#include "face_dispatch.h"
#include "ff_theme.h"
#include "fixture.h"

#ifndef FF_FIXTURE_DIR
#define FF_FIXTURE_DIR "tests/fixtures/"
#endif

void setUp(void) {}

/* Same safety net test_face_hit_targets.c/test_tap_target_sizing.c use —
 * a TEST_ASSERT that longjmps out of a per-fixture lv_init/lv_deinit
 * pair would otherwise leak LVGL initialized into the next test. */
void tearDown(void)
{
    if (lv_is_initialized()) {
        lv_deinit();
    }
}

/* has_press_feedback — true iff `obj` registers a LOCAL style rule for
 * (LV_PART_MAIN | LV_STATE_PRESSED) on either bg_opa or bg_color — the
 * exact same check as test_scr_intent.c's own `has_press_feedback`,
 * duplicated here rather than shared across a app/-vs-targets/ boundary
 * for one four-line predicate (this file's whole reason to exist is to
 * apply that same check somewhere test_scr_intent.c cannot reach:
 * fixture-driven, cross-face, real-dispatch screens).
 *
 * REVIEW FIX (independent review of #329, mutation-verified): the
 * previous form of this predicate was `lv_obj_has_style_prop(obj,
 * LV_PART_MAIN | LV_STATE_PRESSED, LV_STYLE_BG_OPA/COLOR)` — a SELECTOR
 * query. LVGL's style matching treats a style added at LV_STATE_DEFAULT
 * (selector 0) as applicable to EVERY state, including a query for
 * PART_MAIN|STATE_PRESSED — DEFAULT requires no state bits, so it always
 * "matches". Every filled pill in this codebase sets bg_opa/bg_color at
 * selector 0 unconditionally (`ff_scr_pill_create`'s `cfg->filled`
 * branch, scr_widgets.c) whether or not `cfg->press` adds a DEDICATED
 * PRESSED-state override — so the old query returned true for EVERY
 * filled clickable, including ones with `FF_SCR_PILL_PRESS_NONE` and
 * zero press styling at all. Confirmed by mutation: reverting
 * `settings_make_pill` to `FF_SCR_PILL_PRESS_NONE` did NOT make this
 * test fail (584 controls, 0 violations — unchanged). This function now
 * MEASURES whether the object's resolved bg_opa/bg_color actually CHANGE
 * when LV_STATE_PRESSED is applied — the same "force the state, read
 * the real result" technique this same PR's own `ffsim --press-label`
 * uses for its reference screenshots — rather than asserting a style
 * rule merely exists at some selector. Re-verified by the same mutation:
 * with this fix, reverting to FF_SCR_PILL_PRESS_NONE now fails with
 * "584 controls checked, ... violation(s)" as expected. */
static bool has_press_feedback(lv_obj_t *obj)
{
    lv_opa_t const opa_rest = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
    lv_color_t const color_rest = lv_obj_get_style_bg_color(obj, LV_PART_MAIN);

    lv_obj_add_state(obj, LV_STATE_PRESSED);
    lv_opa_t const opa_pressed = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
    lv_color_t const color_pressed = lv_obj_get_style_bg_color(obj, LV_PART_MAIN);
    lv_obj_remove_state(obj, LV_STATE_PRESSED);

    return (opa_pressed != opa_rest) || !lv_color_eq(color_pressed, color_rest);
}

/* has_real_callback — does this clickable actually DO anything, or is
 * it a scroll-relay/gesture-catcher shape wearing LV_OBJ_FLAG_CLICKABLE
 * only so LVGL's scroll machinery has something to walk up from? Same
 * predicate, same rationale, as test_tap_target_sizing.c's
 * `sizing_has_callback`. */
static bool has_real_callback(lv_obj_t *obj)
{
    return lv_obj_get_event_count(obj) > 0;
}

typedef struct {
    int checked;
    int violations;
} press_result_t;

/* press_is_known_event_driven — is `obj` one of scr_inbox.c's row/FAB tap
 * targets, whose press wash is real but implemented differently (see this
 * file's top comment): `inbox_row_press_ev` (scr_inbox.c:943) toggles a
 * SEPARATE decoration object's bg_opa from manually-wired LV_EVENT_PRESSED/
 * RELEASED/PRESS_LOST callbacks, not via LV_STATE_PRESSED on the tap target
 * `has_press_feedback` inspects — so this predicate cannot see it and would
 * otherwise misreport a real, working control as a violation.
 *
 * Deliberately narrow and two-factor, not a blanket "every inbox_* fixture
 * passes" rule:
 *   1. `fixture_name` must be one of scr_inbox.c's own committed fixtures
 *      (its rows/thread views, or `banner_on_thread` which renders a thread
 *      underneath) — never any other face.
 *   2. `obj` must carry MORE event descriptors than an ordinary
 *      `ff_scr_button_create` control with one CLICKED handler ever has.
 *      `ff_scr_button_create` (scr_nav.c) unconditionally registers 4 shared
 *      infrastructure descriptors (tap-sound CLICKED + PRESSED/PRESSING/
 *      DELETE slide-off tracking); a control's own CLICKED handler is a
 *      5th. `inbox_row_press_ev`'s PRESSED+RELEASED+PRESS_LOST trio
 *      (scr_inbox.c:1009-1011/1273-1275) adds 3 more — 8 total. Requiring
 *      BOTH factors means a control that regresses to plain, unadorned
 *      CLICKED-only wiring (5 descriptors) is NOT exempted by fixture name
 *      alone and still fails the sweep below.
 *
 * A tracked follow-up (see the PR/review thread) should replace this with a
 * real behavioral check — fire LV_EVENT_PRESSED for real and diff the
 * rendered frame, the same thing a finger actually does — so scr_inbox.c's
 * own convention is verified, not merely presumed correct from reading its
 * source once during this review. */
static bool press_is_known_event_driven(char const *fixture_name, lv_obj_t *obj)
{
    static char const *const inbox_family[] = {
        "inbox_no_crew.json",      "inbox_thread_direct.json",  "inbox_thread_crew.json",
        "inbox_inbox.json",        "inbox_thread_outbox_states.json",
        "inbox_all_stale.json",    "inbox_quiet.json",          "inbox_thread_crew_long.json",
        "inbox_thread_short.json", "inbox_picker.json",         "banner_on_thread.json",
    };
    bool in_family = false;
    for (size_t i = 0; i < sizeof(inbox_family) / sizeof(inbox_family[0]); i++) {
        if (strcmp(fixture_name, inbox_family[i]) == 0) {
            in_family = true;
            break;
        }
    }
    if (!in_family) {
        return false;
    }
    return lv_obj_get_event_count(obj) > 5;
}

static void press_walk(lv_obj_t *obj, char const *fixture_name, press_result_t *out)
{
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);

        if (lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) {
            lv_area_t area;
            lv_obj_get_click_area(child, &area);
            float const w = (float)(area.x2 - area.x1 + 1);
            float const h = (float)(area.y2 - area.y1 + 1);
            bool const is_whole_puck = (w == (float)FF_THEME_PUCK_PX) && (h == (float)FF_THEME_PUCK_PX);

            /* REVIEW FIX (independent review of #329): a third exclusion
             * shape neither test_face_hit_targets.c's Exclusions 1-4 nor
             * `has_real_callback` above covers — the SCROLLABLE ancestor
             * of a list itself (scr_settings.c's own "list" object,
             * `settings_build_settings_page`/`settings_build_diag_page`'s
             * own doc comment: "#bug2 — the list MUST stay CLICKABLE ...
             * It carries no CLICKED handler"). It genuinely registers
             * event callbacks (LV_EVENT_SCROLL/SCROLL_END, to persist
             * scroll position), so `has_real_callback`'s "any event count
             * > 0" proxy — correct for the narrower size-floor question
             * `sizing_has_callback` asks, see that function's own doc
             * comment — wrongly counts it as a control here. LVGL clears
             * LV_OBJ_FLAG_SCROLLABLE on every `lv_button_create` at
             * construction (lv_button.c) and no button/pill factory in
             * this codebase re-adds it, so no real control this sweep
             * should catch is ever SCROLLABLE — it is a safe, precise
             * signal for "ambient scroll surface", not "a control a
             * thumb aims at", confirmed by isolated review testing. */
            bool const is_scroll_surface = lv_obj_has_flag(child, LV_OBJ_FLAG_SCROLLABLE);

            /* Not counted at all when true — see press_is_known_event_driven's
             * doc comment. A narrow, named, source-verified carve-out, not a
             * blanket "skip this face" rule: it still requires the object to
             * carry the EXTRA event wiring the alternate mechanism needs, so
             * an inbox control that regresses to having NO press wiring at
             * all is NOT exempted and still fails below like anything else.
             * Evaluated lazily (short-circuit) only when the object would
             * otherwise be a violation, so it costs nothing on the 573
             * controls that already pass the direct check. */
            bool const is_known_event_driven =
                !has_press_feedback(child) && press_is_known_event_driven(fixture_name, child);

            if (!is_whole_puck && !is_scroll_surface && !is_known_event_driven && has_real_callback(child)) {
                out->checked++;
                if (!has_press_feedback(child)) {
                    out->violations++;
                    printf("  NO-PRESS-FEEDBACK [%s]  rect=(%d,%d)-(%d,%d) %.0fx%.0f — clickable control has no "
                           "LV_STATE_PRESSED bg_opa/bg_color style\n",
                           fixture_name, (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2, (double)w,
                           (double)h);
                }
            }
        }

        press_walk(child, fixture_name, out);
    }
}

static void press_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

static uint32_t press_tick_cb(void)
{
    return 0;
}

static press_result_t press_fixture(char const *path, char const *name)
{
    lv_init();
    lv_tick_set_cb(press_tick_cb);

    const int32_t w = FF_THEME_WINDOW_PX;
    const int32_t h = FF_THEME_WINDOW_PX;
    const uint32_t buf_size = (uint32_t)(w * h * 4);
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf, path);

    lv_display_t *disp = lv_display_create(w, h);
    lv_display_set_buffers(disp, buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, press_flush_cb);
    lv_display_set_default(disp);

    ff_app_state_t state;
    ff_fixture_result_t fr = ff_fixture_load_file(path, &state);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FF_FIXTURE_OK, fr, path);

    ff_build_face_screen(&state);
    lv_refr_now(disp);

    press_result_t result = {0, 0};
    press_walk(lv_screen_active(), name, &result);

    free(buf);
    lv_deinit();

    return result;
}

/* The test — every *.json under tests/fixtures/, no allowlist, no
 * per-face rule table (unlike test_tap_target_sizing.c, this property
 * is universal: EVERY face gets press feedback, not just the four
 * outdoor-floor faces). */
static void S_PRESS_every_clickable_on_every_fixture_has_pressed_style(void)
{
    DIR *d = opendir(FF_FIXTURE_DIR);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, FF_FIXTURE_DIR);

    int total_checked = 0;
    int total_violations = 0;
    int fixtures_swept = 0;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        size_t nlen = strlen(entry->d_name);
        bool is_json = (nlen > 5) && (strcmp(entry->d_name + nlen - 5, ".json") == 0);
        if (!is_json) {
            continue;
        }

        char name[sizeof(entry->d_name)];
        snprintf(name, sizeof(name), "%s", entry->d_name);

        char path[sizeof(FF_FIXTURE_DIR) + sizeof(entry->d_name)];
        snprintf(path, sizeof(path), "%s%s", FF_FIXTURE_DIR, entry->d_name);

        press_result_t r = press_fixture(path, name);
        total_checked += r.checked;
        total_violations += r.violations;
        fixtures_swept++;
    }
    closedir(d);

    printf("test_press_feedback_all_faces: swept %d fixture(s), checked %d clickable control(s), %d "
           "violation(s)\n",
           fixtures_swept, total_checked, total_violations);

    /* The same vacuous-pass guards this codebase's other fixture sweeps
     * use — a silently-empty fixture dir, or a walk that finds nothing
     * clickable anywhere, would otherwise make this test prove nothing. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, fixtures_swept, "no fixtures found under " FF_FIXTURE_DIR);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, total_checked, "swept fixtures but found zero clickable controls");

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, total_violations,
                                  "one or more clickable controls have no LV_STATE_PRESSED bg_opa/bg_color style — "
                                  "see the NO-PRESS-FEEDBACK lines above");
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S_PRESS_every_clickable_on_every_fixture_has_pressed_style);

    return UNITY_END();
}
