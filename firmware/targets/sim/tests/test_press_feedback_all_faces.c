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
 * has_press_feedback below is the identical check test_scr_intent.c's
 * own `has_press_feedback` uses (a SELECTOR query via
 * lv_obj_has_style_prop, not a "what does this look like right now"
 * query) — every press treatment in this codebase (scr_widgets.c's
 * `ff_scr_pill_create`, scr_radar.c's FLARE, scr_launcher.c's tiles,
 * scr_inbox.c's rows/chips, scr_banner.c, scr_compose.c) sets bg_opa
 * and/or bg_color at LV_STATE_PRESSED — see this codebase's own grep of
 * `LV_STATE_PRESSED` across every screen source file: every hit is one of those
 * two properties.
 *
 * Two categories of CLICKABLE object are excluded, same reasoning
 * test_tap_target_sizing.c's `sizing_has_callback`/`is_whole_puck` use
 * for the identical shapes:
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
 * fixture-driven, cross-face, real-dispatch screens). */
static bool has_press_feedback(lv_obj_t *obj)
{
    return lv_obj_has_style_prop(obj, LV_PART_MAIN | LV_STATE_PRESSED, LV_STYLE_BG_OPA) ||
           lv_obj_has_style_prop(obj, LV_PART_MAIN | LV_STATE_PRESSED, LV_STYLE_BG_COLOR);
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

            if (!is_whole_puck && has_real_callback(child)) {
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
