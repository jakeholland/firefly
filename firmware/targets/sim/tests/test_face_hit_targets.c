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
 * ## The one deliberate exclusion, and why it isn't "special-casing away"
 * a finding
 *
 * A hit-rect exactly the size of the puck's own square bounding box
 * (`FF_THEME_PUCK_PX` x `FF_THEME_PUCK_PX` — e.g. scr_nav.c's
 * long-press-anywhere-on-the-puck Settings hook) is excluded from the
 * circle-containment check (but NOT the size-floor check). This is a
 * different category of control from a sized button a user aims at:
 * its "excess" past the visible circle corresponds to no
 * physically-touchable surface on the real round hardware at all
 * (there's nothing to mis-tap — those corners simply don't exist on
 * the device), so it can never make a VISIBLE, sized control
 * unreachable the way an undersized-or-misplaced button can. The
 * exclusion is intentionally narrow (an EXACT full-puck-square size
 * match, not "large" or "near the edge") specifically so a genuinely
 * mis-sized-but-still-a-bug element can't hide behind it.
 *
 * A SECOND exclusion used to live here: a hit-rect entirely outside the
 * WINDOW (not just the round glass) was logged as INFO and excluded from
 * both checks, because `scr_nav.c`'s `ff_scr_nav_build` built all THREE
 * tileview tiles' full content on every render regardless of which one
 * was active — an inactive tile's controls sat at their un-scrolled
 * tileview position, hundreds of pixels outside the window. Issue #29
 * closed that (S16 slice d): `ff_scr_nav_build` now builds content into
 * the ACTIVE tile only, so there is no longer anything off-window to
 * exclude, and this sweep asserts on it like any other finding — exactly
 * the outcome issue #29 promised ("the sweep test's exception should
 * disappear with it").
 *
 * ## S17 slice b (AC2): the adjacency floor
 *
 * The size floor above answers "is this control big enough"; it says
 * nothing about whether two DIFFERENT, individually-fine-sized controls
 * sit close enough together that a real thumb could straddle both and
 * trigger the wrong one (the exact shape of PR #68's "10px Settings pill
 * gap" finding, caught there by eye). This sweep now also asserts, for
 * every pair of CLICKABLE elements that share the same immediate LVGL
 * parent, that their hit-rects are >= `FF_HIT_MIN_GAP_PX` apart, EDGE TO
 * EDGE (see that constant's own doc comment in ff_theme.h for the full
 * "why edge-to-edge, why 8px, why sibling-scoped" rationale) — with one
 * deliberate exclusion, for the identical reason the whole-puck-square
 * exclusion above exists: a real category of "not actually independent",
 * not a loophole for a genuine violation to hide behind.
 *
 * A pair is skipped when both elements register the SAME click callback
 * function pointer AND the SAME `user_data` — i.e. a tap on either one
 * reports the identical intent (e.g. scr_settings.c's WATER NUDGE row:
 * the dim label and its own value chip are two separate LVGL objects,
 * both wired to `settings_water_cb` with no user_data, because they are
 * two hit-rects for ONE logical control, not two controls a thumb could
 * confuse). This is deliberately a GEOMETRY-and-wiring test, not a
 * per-screen exception list naming "the label next to the water chip" —
 * the same "measure, not reason harder" standing rule (AGENTS.md's proxy
 * check) the rest of this file already follows. A pair where either side
 * registers NO click callback at all is never treated as composite (both
 * sides NULL would otherwise collide) — checked, not silently paired.
 *
 * Sibling-scoped (not a global all-pairs sweep) for the same reason the
 * whole-puck-square exclusion above exists: `scr_nav.c`'s long-press
 * region and `scr_map.c`'s tap-anywhere-back puck are both CLICKABLE
 * objects sized to the full puck, with every real control on the active
 * tile nested somewhere underneath them — an all-pairs check would
 * compare every such control against that enclosing full-puck region and
 * find a "violation" for every single one, which is not a real mis-tap
 * risk (a touch always resolves to the DEEPEST clickable object under it,
 * per LVGL's own hit-testing, never ambiguously to an ancestor AND a
 * descendant at once). Restricting comparisons to elements sharing an
 * immediate parent sidesteps that ancestor/descendant case entirely
 * (an ancestor and its nested descendant are essentially never siblings
 * in this codebase's tree shapes) while still catching every real stress
 * case this slice's task brief named: the T9 keypad's keys (all direct
 * children of one keys-container) and the Settings double-chip rows (all
 * direct children of the settings puck).
 */
#include <dirent.h>
#include <math.h> /* sqrtf — AC2's edge-to-edge gap distance */
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
    int gap_checked; /* AC2: number of sibling PAIRS the adjacency floor was applied to */
    int violations;
} sweep_result_t;

/* Puck circle, in the SAME absolute display-pixel space lv_obj_get_click_area
 * reports (this codebase's window/puck geometry, ff_theme.h): the puck is
 * centered in the WINDOW_PX square, so its own center sits
 * (margin + PUCK_RADIUS) from the window's top-left in both axes. */
#define SWEEP_MARGIN_PX ((FF_THEME_WINDOW_PX - FF_THEME_PUCK_PX) / 2)
#define SWEEP_CX ((float)(SWEEP_MARGIN_PX + FF_THEME_PUCK_RADIUS_PX))
#define SWEEP_CY ((float)(SWEEP_MARGIN_PX + FF_THEME_PUCK_RADIUS_PX))
#define SWEEP_RADIUS ((float)FF_THEME_PUCK_RADIUS_PX)

/* One CLICKABLE direct child, captured for the AC2 sibling adjacency pass
 * below — gathered while sweep_walk is already iterating `obj`'s children
 * for the AC1 size/circle checks, so no second tree traversal is needed. */
typedef struct {
    lv_obj_t *obj;
    ff_layout_rect_t rect; /* EXCLUSIVE far edge, ff_layout.h convention — same as the AC1 circle check uses */
    void *cb;              /* first registered click callback, or NULL if none registered */
    void *user_data;       /* that callback's user_data — together, the "same logical control" signature */
} sweep_sibling_t;

#define SWEEP_MAX_SIBLINGS 48 /* generous headroom over this codebase's densest row (T9's 12 keys, Settings' 12 controls) */

/* sweep_rect_gap_px — the shortest EDGE-TO-EDGE distance between two
 * axis-aligned rects (0 if they overlap on either axis) — see
 * FF_HIT_MIN_GAP_PX's doc comment (ff_theme.h) for why this, not
 * centre-to-centre, is the right quantity. */
static float sweep_rect_gap_px(ff_layout_rect_t a, ff_layout_rect_t b)
{
    float dx = 0.0f;
    if (a.x2 <= b.x1) {
        dx = b.x1 - a.x2;
    } else if (b.x2 <= a.x1) {
        dx = a.x1 - b.x2;
    }

    float dy = 0.0f;
    if (a.y2 <= b.y1) {
        dy = b.y1 - a.y2;
    } else if (b.y2 <= a.y1) {
        dy = a.y1 - b.y2;
    }

    if (dx == 0.0f && dy == 0.0f) {
        return 0.0f; /* overlapping (or touching) on both axes */
    }
    return sqrtf(dx * dx + dy * dy);
}

/* sweep_same_composite_control — true iff `a` and `b` are two hit-rects
 * for the SAME logical control (e.g. a row's dim label and its own value
 * chip, both wired to the identical setter) rather than two independent
 * controls — see this file's header comment ("S17 slice b: the adjacency
 * floor") for the full rationale. A pair where EITHER side registered no
 * click callback at all is never treated as composite — only a genuine,
 * matching (cb, user_data) pair collapses the gap check. */
static bool sweep_same_composite_control(sweep_sibling_t const *a, sweep_sibling_t const *b)
{
    if (a->cb == NULL || b->cb == NULL) {
        return false;
    }
    return a->cb == b->cb && a->user_data == b->user_data;
}

static void sweep_walk(lv_obj_t *obj, char const *fixture_name, sweep_result_t *out)
{
    uint32_t n = lv_obj_get_child_count(obj);
    sweep_sibling_t siblings[SWEEP_MAX_SIBLINGS];
    int n_siblings = 0;

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

            ff_layout_rect_t r = {(float)area.x1, (float)area.y1, (float)area.x2 + 1.0f, (float)area.y2 + 1.0f};

            if (!is_whole_puck_gesture_region) {
                if (!ff_layout_rect_in_circle(r, SWEEP_CX, SWEEP_CY, SWEEP_RADIUS)) {
                    out->violations++;
                    printf("  HIT-RECT-OFF-GLASS    [%s]  rect=(%d,%d)-(%d,%d)  circle center=(%.1f,%.1f) r=%.1f\n",
                           fixture_name, (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2, (double)SWEEP_CX,
                           (double)SWEEP_CY, (double)SWEEP_RADIUS);
                }
            }

            /* AC2: record this clickable child for the sibling adjacency
             * pass below (after every child of `obj` has been visited). */
            if (n_siblings < SWEEP_MAX_SIBLINGS) {
                sweep_sibling_t *s = &siblings[n_siblings++];
                s->obj = child;
                s->rect = r;
                uint32_t ec = lv_obj_get_event_count(child);
                if (ec > 0) {
                    lv_event_dsc_t *dsc = lv_obj_get_event_dsc(child, 0);
                    s->cb = (void *)lv_event_dsc_get_cb(dsc);
                    s->user_data = lv_event_dsc_get_user_data(dsc);
                } else {
                    s->cb = NULL;
                    s->user_data = NULL;
                }
            }
        }

        sweep_walk(child, fixture_name, out);
    }

    /* AC2 — adjacency floor: every PAIR of this parent's own clickable
     * children (not composite, not the whole-puck gesture region — see
     * this file's header comment) must clear FF_HIT_MIN_GAP_PX edge to
     * edge. Sibling-scoped, not a global all-pairs sweep — see the same
     * comment for why that's the right scope, not a narrowing of it. */
    for (int i = 0; i < n_siblings; i++) {
        float wi = siblings[i].rect.x2 - siblings[i].rect.x1;
        float hi = siblings[i].rect.y2 - siblings[i].rect.y1;
        bool i_is_whole_puck = (wi == (float)FF_THEME_PUCK_PX) && (hi == (float)FF_THEME_PUCK_PX);
        if (i_is_whole_puck) {
            continue;
        }
        for (int j = i + 1; j < n_siblings; j++) {
            float wj = siblings[j].rect.x2 - siblings[j].rect.x1;
            float hj = siblings[j].rect.y2 - siblings[j].rect.y1;
            bool j_is_whole_puck = (wj == (float)FF_THEME_PUCK_PX) && (hj == (float)FF_THEME_PUCK_PX);
            if (j_is_whole_puck) {
                continue;
            }
            if (sweep_same_composite_control(&siblings[i], &siblings[j])) {
                continue;
            }

            out->gap_checked++;
            float gap = sweep_rect_gap_px(siblings[i].rect, siblings[j].rect);
            if (gap < (float)FF_HIT_MIN_GAP_PX) {
                out->violations++;
                printf("  HIT-TARGETS-TOO-CLOSE [%s]  rect_a=(%.0f,%.0f)-(%.0f,%.0f)  "
                       "rect_b=(%.0f,%.0f)-(%.0f,%.0f)  gap=%.1fpx  (floor %dpx)\n",
                       fixture_name, (double)siblings[i].rect.x1, (double)siblings[i].rect.y1,
                       (double)siblings[i].rect.x2, (double)siblings[i].rect.y2, (double)siblings[j].rect.x1,
                       (double)siblings[j].rect.y1, (double)siblings[j].rect.x2, (double)siblings[j].rect.y2,
                       (double)gap, (int)FF_HIT_MIN_GAP_PX);
            }
        }
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
    int total_gap_checked = 0;
    int total_violations = 0;
    int fixtures_swept = 0;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        size_t nlen = strlen(entry->d_name);
        bool is_json = (nlen > 5) && (strcmp(entry->d_name + nlen - 5, ".json") == 0);
        if (!is_json) {
            continue;
        }

        /* #81 (gcc-14 -Wformat-truncation, fixed here as a trivial
         * one-liner while this file is already touched for S17 slice b —
         * see AGENTS.md's standing brief: "gcc-14 on touched targets").
         * GCC can't bound `entry->d_name`'s runtime length from a plain
         * `char[]` field, so it conservatively assumes it could fill the
         * whole array — sizing each destination to the FULL declared
         * capacity of every source (the macro literal's own compile-time
         * size, `entry->d_name`'s own array size) proves to the compiler
         * that neither snprintf can truncate, the same technique
         * fixture_view.c's own `-Wformat-truncation` fix already
         * established in this codebase. */
        char path[sizeof(FF_FIXTURE_DIR) + sizeof(entry->d_name)];
        snprintf(path, sizeof(path), "%s%s", FF_FIXTURE_DIR, entry->d_name);

        char name[sizeof(entry->d_name)];
        snprintf(name, sizeof(name), "%s", entry->d_name);

        printf("test_face_hit_targets: sweeping %s\n", name);
        sweep_result_t r = sweep_fixture(path, name);
        total_checked += r.checked;
        total_gap_checked += r.gap_checked;
        total_violations += r.violations;
        fixtures_swept++;
    }
    closedir(d);

    printf("test_face_hit_targets: swept %d fixture(s), checked %d clickable element(s), %d sibling pair(s) for the "
           "adjacency floor, %d violation(s)\n",
           fixtures_swept, total_checked, total_gap_checked, total_violations);

    /* Sanity: the sweep must actually have found fixtures and clickable
     * elements to check — a silently-empty directory or an all-skipped
     * walk would make this test vacuously pass without proving anything
     * (the mutation-test failure mode this whole file exists to avoid). */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, fixtures_swept, "no fixtures found — FF_FIXTURE_DIR misconfigured?");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, total_checked, "swept fixtures but found zero clickable elements to check");
    /* Same sanity, for AC2: at least one fixture must actually exercise two
     * independent siblings close enough together to be a meaningful test of
     * the adjacency floor (not just the size floor above) — otherwise a
     * shrunk FF_HIT_MIN_GAP_PX could silently stop mattering and nothing
     * here would notice. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, total_gap_checked,
                                          "swept fixtures but found zero sibling pairs to check the adjacency floor "
                                          "against");

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, total_violations,
                                   "one or more interactive elements are off-glass, under the 44px hit-target floor, "
                                   "or closer than the adjacency floor to a sibling control — see the HIT-* lines "
                                   "printed above for exactly which ones");
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S08_hit_targets_every_committed_fixture_fits_the_glass);

    return UNITY_END();
}
