/**
 * test_map_circle_containment.c — issue #75/#77 regression: proves BOTH
 * halves of the fix against `tests/fixtures/map_clip_stress.json` (a
 * feature whose own vertices legitimately extend past the fitted circle,
 * a HIGH-priority label long enough to cross the round glass on its own,
 * and a LOW-priority label doing the same):
 *
 *   1. The headless build COMPLETES, and fast — the actual regression for
 *      issue #75 (`lv_obj_set_style_clip_corner` reliably hung `ffsim
 *      --headless` at this face's draw-object count; see scr_map.c's
 *      header comment). Same in-process build path
 *      test_face_hit_targets.c already uses (`ff_build_face_screen` +
 *      `lv_refr_now`, no SDL) — if the old clip_corner approach were
 *      reintroduced, this test would hang instead of failing fast, which
 *      is exactly why the wall-clock assertion below exists: a bare
 *      "did it return" check would still eventually pass under a test
 *      runner with no timeout, but a human (or CI) watching it hang is
 *      the whole bug.
 *   2. A LAYOUT-LEVEL bounds check: every LABEL object actually built
 *      (map_make_label's real `lv_label_create` widgets — not the
 *      full-puck-sized triangle/segment proxy objects scr_map.c's header
 *      comment explains can't be checked this way) has its real,
 *      on-screen bounding box entirely inside the round glass —
 *      `ff_layout_rect_in_circle` (app/screens/ff_layout.h), the same
 *      primitive test_face_hit_targets.c uses for hit-targets. This is
 *      the end-to-end confirmation that `ff_map_place_labels`'s
 *      circle-bounds rejection (core/include/ff_map.h) actually reaches
 *      the rendered screen, on top of that function's own direct unit
 *      tests in core/tests/test_map.c.
 *
 * The polygon/line vertex-clamp half of the fix (`ff_map_clip_point_to_
 * circle`) is NOT checked here — scr_map.c's triangle/segment draw
 * objects are plain `lv_obj_t`s sized to the WHOLE puck as a vehicle for
 * a custom `LV_EVENT_DRAW_MAIN` callback (see that file's header
 * comment), so `lv_obj_get_coords` on one of them reports the full puck
 * box regardless of where the clamped geometry actually draws — an LVGL
 * object-tree walk is structurally unable to see it. That half is
 * covered directly, at the geometry level, by core/tests/test_map.c's
 * `S75_clip_point_*` tests instead.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "unity.h"

#include "lvgl.h"

#include "face_dispatch.h"
#include "ff_layout.h"
#include "ff_theme.h"
#include "fixture.h"

#ifndef FF_FIXTURE_DIR
#define FF_FIXTURE_DIR "tests/fixtures/"
#endif

/* S09 spec's own literal ("the 412 circle") — scr_map.c's
 * FF_MAP_CIRCLE_RADIUS_PX is a private #define, so this is the same
 * documented duplication core/tests/test_map.c's FF_TEST_RADIUS_PX
 * already carries, not a fresh magic number. */
#define MAP_CIRCLE_RADIUS_PX 206.0f

void setUp(void) {}
void tearDown(void) {}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

static uint32_t tick_cb(void)
{
    return 0; /* frozen clock — same determinism story as run_goldens.sh --mock-clock */
}

typedef struct {
    int labels_checked;
    int off_circle;
} sweep_result_t;

static void sweep_labels(lv_obj_t *obj, float cx, float cy, sweep_result_t *out)
{
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);

        if (lv_obj_check_type(child, &lv_label_class)) {
            lv_area_t area;
            lv_obj_get_coords(child, &area);
            /* lv_area_t's x2/y2 are inclusive; ff_layout_rect_t's are
             * exclusive — +1 converts (same conversion
             * test_face_hit_targets.c documents and applies). */
            ff_layout_rect_t r = {(float)area.x1, (float)area.y1, (float)area.x2 + 1.0f, (float)area.y2 + 1.0f};

            out->labels_checked++;
            if (!ff_layout_rect_in_circle(r, cx, cy, MAP_CIRCLE_RADIUS_PX)) {
                out->off_circle++;
                printf("  LABEL-OFF-CIRCLE  rect=(%d,%d)-(%d,%d)  circle center=(%.1f,%.1f) r=%.1f\n", (int)area.x1,
                       (int)area.y1, (int)area.x2, (int)area.y2, (double)cx, (double)cy,
                       (double)MAP_CIRCLE_RADIUS_PX);
            }
        }

        sweep_labels(child, cx, cy, out);
    }
}

static void S75_S77_map_clip_stress_headless_build_completes_fast_and_labels_stay_inside_circle(void)
{
    char path[1024];
    snprintf(path, sizeof(path), "%smap_clip_stress.json", FF_FIXTURE_DIR);

    lv_init();
    lv_tick_set_cb(tick_cb);

    const int32_t w = FF_THEME_WINDOW_PX;
    const int32_t h = FF_THEME_WINDOW_PX;
    const uint32_t buf_size = (uint32_t)(w * h * 4);
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    TEST_ASSERT_NOT_NULL(buf);

    lv_display_t *disp = lv_display_create(w, h);
    lv_display_set_buffers(disp, buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_default(disp);

    ff_app_state_t state;
    ff_fixture_result_t fr = ff_fixture_load_file(path, &state);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FF_FIXTURE_OK, fr, path);

    /* --- (1) headless build completes, and fast. --- */
    clock_t const t0 = clock();
    ff_build_face_screen(&state);
    lv_refr_now(disp);
    clock_t const t1 = clock();

    double const elapsed_s = (double)(t1 - t0) / (double)CLOCKS_PER_SEC;
    /* Generous on purpose (this build takes low-single-digit milliseconds
     * in practice) — this bound exists to fail LOUD on a reintroduced
     * hang, not to police performance. */
    TEST_ASSERT_TRUE_MESSAGE(elapsed_s < 2.0, "map_clip_stress build+render took >= 2s — possible hang regression "
                                               "(issue #75)");

    /* --- (2) every real label object stays inside the round glass. --- */
    float const cx = (float)(FF_THEME_WINDOW_PX / 2);
    float const cy = (float)(FF_THEME_WINDOW_PX / 2);
    sweep_result_t result = {0, 0};
    sweep_labels(lv_screen_active(), cx, cy, &result);

    free(buf);
    lv_deinit();

    printf("test_map_circle_containment: checked %d label(s), %d off-circle\n", result.labels_checked,
           result.off_circle);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, result.labels_checked,
                                          "swept the stress fixture but found zero label objects to check");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, result.off_circle,
                                   "one or more label objects render outside the round glass — see the "
                                   "LABEL-OFF-CIRCLE line(s) above");
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S75_S77_map_clip_stress_headless_build_completes_fast_and_labels_stay_inside_circle);

    return UNITY_END();
}
