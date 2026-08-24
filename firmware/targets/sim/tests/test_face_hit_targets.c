/**
 * test_face_hit_targets.c — S08 (PR #25 UX review follow-up): the
 * automated assertion that makes "a control sitting off the round glass"
 * impossible to ship silently again.
 *
 * For every *.json under tests/fixtures/, this builds the EXACT real
 * screen ffsim itself would (via face_dispatch.h's ff_build_face_screen —
 * the same dispatch table main.c uses, not a second copy that could
 * drift), then walks the resulting LVGL object tree and asserts, for
 * every CLICKABLE element:
 *   1. its on-screen hit-rect is >= FF_THEME_MIN_HIT_PX in BOTH
 *      dimensions (the ux-raver fat-thumb floor), and
 *   2. its hit-rect lies entirely within the round glass — the circle
 *      `ff_theme.h`'s own puck geometry defines (center at
 *      (margin + PUCK_RADIUS, margin + PUCK_RADIUS), radius PUCK_RADIUS,
 *      where margin = (WINDOW_PX - PUCK_PX) / 2) — via ff_layout.h's
 *      ff_layout_rect_in_circle.
 *
 * This is assertion-level, not visual (same rationale as
 * app/screens/tests/test_radar_layout.c's header comment): a golden PNG
 * is a pixel-diff against ITSELF, so a control that was wrong-but-stable
 * in every single render would pass every golden check forever — which
 * is exactly how PR #25's original Compose layout (back button ~42px
 * off-glass, mode chip ~35px off-glass) shipped undetected by its own
 * goldens in the first place.
 *
 * ## The two deliberate exclusions, and why they aren't "special-casing
 * away" a finding
 *
 * 1. A hit-rect exactly the size of the puck's own square bounding box
 *    (`FF_THEME_PUCK_PX` x `FF_THEME_PUCK_PX` — e.g. scr_nav.c's
 *    long-press-anywhere-on-the-puck Settings hook) is excluded from the
 *    circle-containment check (but NOT the size-floor check). This is a
 *    different category of control from a sized button a user aims at:
 *    its "excess" past the visible circle corresponds to no
 *    physically-touchable surface on the real round hardware at all
 *    (there's nothing to mis-tap — those corners simply don't exist on
 *    the device), so it can never make a VISIBLE, sized control
 *    unreachable the way an undersized-or-misplaced button can. The
 *    exclusion is intentionally narrow (an EXACT full-puck-square size
 *    match, not "large" or "near the edge") specifically so a genuinely
 *    mis-sized-but-still-a-bug element can't hide behind it.
 *
 * 2. A hit-rect entirely outside the WINDOW (`FF_THEME_WINDOW_PX`
 *    square, not just the round glass — e.g. `x1 >= FF_THEME_WINDOW_PX`)
 *    is excluded from both checks, logged as INFO rather than a
 *    violation. **This is a real finding, not a test artifact**: it
 *    surfaces that `scr_nav.c`'s `ff_scr_nav_build` builds all THREE
 *    tileview tiles' full content on every render regardless of which
 *    one is active (`ff_scr_radar_build` + `nav_build_todo_pane` +
 *    `ff_scr_signals_build` all run unconditionally) — an inactive
 *    tile's controls (e.g. Signals' "+" button and reply row, when Radar
 *    is the active face) end up sitting at their un-scrolled tileview
 *    position, which can be hundreds of pixels outside the 456px window
 *    entirely, not merely outside the circle. Reported here, not fixed:
 *    on real hardware nothing at those coordinates is reachable (there
 *    is no display surface out there at all, an even stronger case than
 *    exclusion #1's "no touch surface in the corner"), and rearchitecting
 *    `scr_nav.c` to only build the active tile is a materially different,
 *    larger change (touching shared shell code three parallel face PRs
 *    depend on, and overlapping issue #17's already-tracked
 *    teardown/rebuild concerns) than "fit Compose inside the circle" —
 *    out of scope for this PR. Every OTHER finding this sweep produces,
 *    on every face, is reported and asserted on, not filtered.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "lvgl.h"

#include "face_dispatch.h"
#include "ff_layout.h"
#include "ff_theme.h"
#include "fixture.h"

#ifndef FF_FIXTURE_DIR
#define FF_FIXTURE_DIR "tests/fixtures/"
#endif

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------------------
 * Headless LVGL setup — same shape as targets/sim/main.c's
 * ff_run_headless (full-frame software buffer, no SDL), reduced to just
 * what's needed to force a layout pass and read object coordinates back
 * (no PNG export here — this test never looks at pixels, only geometry).
 * ------------------------------------------------------------------- */

static void sweep_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

static uint32_t sweep_tick_cb(void)
{
    return 0; /* frozen clock — matches run_goldens.sh's --mock-clock determinism story */
}

/* ---------------------------------------------------------------------
 * The sweep itself.
 * ------------------------------------------------------------------- */

typedef struct {
    int checked;
    int violations;
    int off_window_inactive_tile; /* see this file's header comment, exclusion #2 */
} sweep_result_t;

/* Puck circle, in the SAME absolute display-pixel space lv_obj_get_click_area
 * reports (this codebase's window/puck geometry, ff_theme.h): the puck is
 * centered in the WINDOW_PX square, so its own center sits
 * (margin + PUCK_RADIUS) from the window's top-left in both axes. */
#define SWEEP_MARGIN_PX ((FF_THEME_WINDOW_PX - FF_THEME_PUCK_PX) / 2)
#define SWEEP_CX ((float)(SWEEP_MARGIN_PX + FF_THEME_PUCK_RADIUS_PX))
#define SWEEP_CY ((float)(SWEEP_MARGIN_PX + FF_THEME_PUCK_RADIUS_PX))
#define SWEEP_RADIUS ((float)FF_THEME_PUCK_RADIUS_PX)

static void sweep_walk(lv_obj_t *obj, char const *fixture_name, sweep_result_t *out)
{
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);

        if (lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) {
            lv_area_t area;
            /* lv_obj_get_click_area (not lv_obj_get_coords): the object's
             * normal box PLUS its ext_click_area, i.e. the SAME effective
             * hit-rect lv_obj_hit_test itself checks a real touch against
             * (LVGL nit, S08 PR #25 code review, finding #4) — a future
             * face that shrinks a button's visible box while widening its
             * tap target via lv_obj_set_ext_click_area would otherwise
             * sail past this sweep's off-glass/too-small checks even
             * though nothing here currently calls that API (confirmed:
             * `grep -rn ext_click_area app/ core/ targets/` has no
             * call sites as of this PR — this is a forward-looking
             * correctness fix, not a regression fix). */
            lv_obj_get_click_area(child, &area);

            /* Exclusion #2 (see header comment): an inactive tileview
             * tile's content sits at its un-scrolled position, which can
             * land entirely outside the window — not reachable by any
             * real touch, and not what this sweep exists to catch.
             * Logged as INFO (visible, not silently dropped), excluded
             * from both checks below. */
            bool is_off_window = (area.x2 < 0) || (area.x1 >= FF_THEME_WINDOW_PX) || (area.y2 < 0) ||
                                  (area.y1 >= FF_THEME_WINDOW_PX);
            if (is_off_window) {
                out->off_window_inactive_tile++;
                printf("  INFO off-window (inactive tile, unreachable) [%s]  rect=(%d,%d)-(%d,%d)\n", fixture_name,
                       (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2);
                sweep_walk(child, fixture_name, out);
                continue;
            }

            /* lv_area_t's x2/y2 are the INCLUSIVE last pixel; ff_layout_rect_t's
             * x2/y2 are the EXCLUSIVE far edge (ff_layout.h's documented
             * convention) — +1 converts between them. */
            float w = (float)(area.x2 - area.x1 + 1);
            float h = (float)(area.y2 - area.y1 + 1);

            bool is_whole_puck_gesture_region = (w == (float)FF_THEME_PUCK_PX) && (h == (float)FF_THEME_PUCK_PX);

            out->checked++;

            if (w < (float)FF_THEME_MIN_HIT_PX || h < (float)FF_THEME_MIN_HIT_PX) {
                out->violations++;
                printf("  HIT-TARGET-TOO-SMALL  [%s]  rect=(%d,%d)-(%d,%d)  %.0fx%.0f  (floor %dpx)\n", fixture_name,
                       (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2, (double)w, (double)h,
                       (int)FF_THEME_MIN_HIT_PX);
            }

            if (!is_whole_puck_gesture_region) {
                ff_layout_rect_t r = {(float)area.x1, (float)area.y1, (float)area.x2 + 1.0f, (float)area.y2 + 1.0f};
                if (!ff_layout_rect_in_circle(r, SWEEP_CX, SWEEP_CY, SWEEP_RADIUS)) {
                    out->violations++;
                    printf("  HIT-RECT-OFF-GLASS    [%s]  rect=(%d,%d)-(%d,%d)  circle center=(%.1f,%.1f) r=%.1f\n",
                           fixture_name, (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2, (double)SWEEP_CX,
                           (double)SWEEP_CY, (double)SWEEP_RADIUS);
                }
            }
        }

        sweep_walk(child, fixture_name, out);
    }
}

static sweep_result_t sweep_fixture(char const *path, char const *name)
{
    lv_init();
    lv_tick_set_cb(sweep_tick_cb);

    const int32_t w = FF_THEME_WINDOW_PX;
    const int32_t h = FF_THEME_WINDOW_PX;
    const uint32_t buf_size = (uint32_t)(w * h * 4);
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf, path);

    lv_display_t *disp = lv_display_create(w, h);
    lv_display_set_buffers(disp, buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, sweep_flush_cb);
    lv_display_set_default(disp);

    ff_app_state_t state;
    ff_fixture_result_t fr = ff_fixture_load_file(path, &state);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FF_FIXTURE_OK, fr, path);

    /* S16 slice c2 dropped ff_build_face_screen's ff_flare_t* parameter:
     * every S10 button now emits an intent through the seam instead
     * (see face_dispatch.h's doc comment), and this geometry sweep never
     * fires a click at all. */
    ff_build_face_screen(&state);
    lv_refr_now(disp);

    sweep_result_t result = {0, 0, 0};
    sweep_walk(lv_screen_active(), name, &result);

    free(buf);
    lv_deinit();

    return result;
}

/* ---------------------------------------------------------------------
 * Directory listing (same opendir/readdir/closedir shape as
 * meshclient/tests/test_meshclient.c's scan_dir_for_forbidden_includes).
 * ------------------------------------------------------------------- */

static void S08_hit_targets_every_committed_fixture_fits_the_glass(void)
{
    DIR *d = opendir(FF_FIXTURE_DIR);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, FF_FIXTURE_DIR);

    int total_checked = 0;
    int total_violations = 0;
    int total_off_window = 0;
    int fixtures_swept = 0;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        size_t nlen = strlen(entry->d_name);
        bool is_json = (nlen > 5) && (strcmp(entry->d_name + nlen - 5, ".json") == 0);
        if (!is_json) {
            continue;
        }

        char path[1024];
        snprintf(path, sizeof(path), "%s%s", FF_FIXTURE_DIR, entry->d_name);

        char name[256];
        snprintf(name, sizeof(name), "%s", entry->d_name);

        printf("test_face_hit_targets: sweeping %s\n", name);
        sweep_result_t r = sweep_fixture(path, name);
        total_checked += r.checked;
        total_violations += r.violations;
        total_off_window += r.off_window_inactive_tile;
        fixtures_swept++;
    }
    closedir(d);

    printf("test_face_hit_targets: swept %d fixture(s), checked %d clickable element(s), %d violation(s), "
           "%d off-window (inactive-tile, unreachable — see header comment exclusion #2)\n",
           fixtures_swept, total_checked, total_violations, total_off_window);

    /* Sanity: the sweep must actually have found fixtures and clickable
     * elements to check — a silently-empty directory or an all-skipped
     * walk would make this test vacuously pass without proving anything
     * (the mutation-test failure mode this whole file exists to avoid). */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, fixtures_swept, "no fixtures found — FF_FIXTURE_DIR misconfigured?");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, total_checked, "swept fixtures but found zero clickable elements to check");

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, total_violations,
                                   "one or more interactive elements are off-glass or under the 44px hit-target "
                                   "floor — see the HIT-* lines printed above for exactly which ones");
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S08_hit_targets_every_committed_fixture_fits_the_glass);

    return UNITY_END();
}
