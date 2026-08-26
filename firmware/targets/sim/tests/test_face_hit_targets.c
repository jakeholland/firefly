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
 * every PAIR of CLICKABLE elements anywhere in the tree, that their
 * hit-rects are >= `FF_HIT_MIN_GAP_PX` apart, EDGE TO EDGE (see that
 * constant's own doc comment in ff_theme.h for the full "why edge-to-edge,
 * why 8px" rationale) — with three, and only three, deliberate exclusions,
 * for the same reason the whole-puck-square exclusion above exists: real
 * categories of "not actually independent", not a loophole for a genuine
 * violation to hide behind.
 *
 * **Exclusion 1 — same logical control.** A pair is skipped when both
 * elements register the SAME click callback function pointer AND the
 * SAME `user_data` — i.e. a tap on either one reports the identical
 * intent (e.g. scr_settings.c's WATER NUDGE row: the dim label and its
 * own value chip are two separate LVGL objects, both wired to
 * `settings_water_cb` with no user_data, because they are two hit-rects
 * for ONE logical control, not two controls a thumb could confuse). This
 * is deliberately a GEOMETRY-and-wiring test, not a per-screen exception
 * list naming "the label next to the water chip" — the same "measure,
 * not reason harder" standing rule (AGENTS.md's proxy check) the rest of
 * this file already follows. A pair where either side registers NO click
 * callback at all is never treated as composite (both sides NULL would
 * otherwise collide) — checked, not silently paired.
 *
 * **Exclusion 2 — ancestor/descendant.** A pair is skipped when one
 * element is an LVGL ancestor of the other (walking `lv_obj_get_parent`
 * from the deeper one; ancestry, not parent-adjacency, so it also catches
 * a control nested several levels under an enclosing clickable region).
 * This is what makes a GLOBAL all-pairs sweep safe: `scr_nav.c`'s
 * long-press region and `scr_map.c`'s tap-anywhere-back puck are both
 * CLICKABLE objects sized to the full puck, and every real control on
 * the active tile is nested — an ancestor, at some depth — underneath
 * them. Without this exclusion, every such control would "violate"
 * against its own enclosing region, which is not a real mis-tap risk: a
 * touch always resolves to the DEEPEST clickable object under it, per
 * LVGL's own hit-testing, never ambiguously to an ancestor AND a
 * descendant at once. This is a STRUCTURAL exclusion (an actual
 * ancestor/descendant relationship in the live object tree), not a
 * size-based heuristic ("looks about puck-sized") — it costs nothing
 * extra to get right and doesn't need updating if some future full-puck
 * region isn't exactly `FF_THEME_PUCK_PX` square.
 *
 * **Exclusion 3 — the whole-puck gesture region itself.** A pair is
 * skipped when EITHER element's hit-rect is an exact
 * `FF_THEME_PUCK_PX` x `FF_THEME_PUCK_PX` match (the SAME test the AC1
 * circle-containment check above already applies, for the same
 * underlying reason: "its 'excess' past the visible circle corresponds
 * to no physically-touchable surface... a different category of control
 * from a sized button a user aims at"). Exclusion 2 alone isn't enough
 * for this: going global surfaced a real case where a whole-puck region
 * is a SIBLING, not an ancestor, of a real control — `scr_nav.c`'s
 * `tileview` (needed CLICKABLE for its own gesture/long-press-fallback
 * handling, so it's deliberately never cleared) sits as a direct child
 * of `puck` right alongside the flare sender overlay's CANCEL button
 * (`flare_takeover`/sending fixtures), not wrapping it — measured gap
 * 0.0px against `flaring_self.json`'s CANCEL before this exclusion was
 * added, an artifact of comparing a screen-spanning gesture catch-all
 * against a real button, not a real "thumb picks the wrong one" risk (a
 * touch anywhere on the puck-minus-CANCEL still resolves to CANCEL's
 * more specific rect first, per LVGL's own deepest-hit-wins search, the
 * exact same reasoning Exclusion 2 relies on — this exclusion just
 * covers the sibling-shaped instance of the identical fact).
 *
 * PR #86 code review (BLOCKING + should-fix): an earlier version of this
 * sweep scoped the adjacency check to elements sharing an immediate LVGL
 * parent instead of this global all-pairs form, reasoning that ancestors
 * and their nested descendants are "essentially never siblings in this
 * codebase's tree shapes" — true, but it also meant two clickable
 * COUSINS (different parents, neither one an ancestor of the other, but
 * visually right next to each other) were structurally invisible to the
 * sweep forever, no matter how close together they sat. The reviewer's
 * concrete instance: `scr_signals.c`'s header "+" button and the first
 * feed row measured exactly 8px apart — the floor itself, zero slack —
 * and neither shares a parent with the other (the "+" is a child of the
 * face's own root container; feed rows are children of the scrollable
 * `list` beneath it), so the old sibling-scoped check could never see it.
 * Switched to the global form above specifically to close that hole —
 * "we enforce a tap-target floor" should mean what it says, not "unless
 * the two controls happen to nest under different parents".
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
    int gap_checked; /* AC2: number of PAIRS the adjacency floor was applied to */
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

/* One CLICKABLE element anywhere in the tree, captured for the AC2 global
 * adjacency pass below — gathered while sweep_walk is already visiting
 * every node for the AC1 size/circle checks, so no second tree traversal
 * is needed. */
typedef struct {
    lv_obj_t *obj;
    ff_layout_rect_t rect; /* EXCLUSIVE far edge, ff_layout.h convention — same as the AC1 circle check uses */
    void *cb;              /* first registered click callback, or NULL if none registered */
    void *user_data;       /* that callback's user_data — together, the "same logical control" signature */
    bool is_whole_puck;    /* exact FF_THEME_PUCK_PX x FF_THEME_PUCK_PX match — see sweep_check_adjacency's
                             * Exclusion 3 for why this needs its own field, distinct from ancestor/descendant */
} sweep_clickable_t;

/* Generous headroom over any single fixture's total clickable-element
 * count — 253 clickable elements were found across ALL 44 pre-#86
 * fixtures combined; no single fixture holds more than a few dozen. */
#define SWEEP_MAX_CLICKABLES 256

typedef struct {
    sweep_clickable_t items[SWEEP_MAX_CLICKABLES];
    int n;
} sweep_clickable_list_t;

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
 * floor", Exclusion 1) for the full rationale. A pair where EITHER side
 * registered no click callback at all is never treated as composite —
 * only a genuine, matching (cb, user_data) pair collapses the gap check.
 *
 * Unit-tested directly below (S17b_AC2_composite_control_detection) —
 * every currently-committed fixture's composite pairs sit at a 24px gap
 * (scr_settings.c's FF_SETTINGS_CHIP_GAP, well above the 8px floor
 * either way), so no fixture's PASS/FAIL outcome depends on this
 * function actually excluding anything; the direct unit test is what
 * proves it does. See that test's own comment for why no fixture can
 * exercise this end-to-end by construction. */
static bool sweep_same_composite_control(sweep_clickable_t const *a, sweep_clickable_t const *b)
{
    if (a->cb == NULL || b->cb == NULL) {
        return false;
    }
    return a->cb == b->cb && a->user_data == b->user_data;
}

/* sweep_is_ancestor — true iff `maybe_ancestor` is a strict LVGL ancestor
 * of `obj` (walks lv_obj_get_parent from `obj` up to the screen root).
 * See this file's header comment ("S17 slice b: the adjacency floor",
 * Exclusion 2) for why the global adjacency pass needs this. */
static bool sweep_is_ancestor(lv_obj_t *maybe_ancestor, lv_obj_t *obj)
{
    lv_obj_t *p = lv_obj_get_parent(obj);
    while (p != NULL) {
        if (p == maybe_ancestor) {
            return true;
        }
        p = lv_obj_get_parent(p);
    }
    return false;
}

static void sweep_walk(lv_obj_t *obj, char const *fixture_name, sweep_result_t *out, sweep_clickable_list_t *list)
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

            /* AC2: record this clickable element for the global adjacency
             * pass below (after the WHOLE tree has been visited). */
            if (list->n < SWEEP_MAX_CLICKABLES) {
                sweep_clickable_t *s = &list->items[list->n++];
                s->obj = child;
                s->rect = r;
                s->is_whole_puck = is_whole_puck_gesture_region;
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

        sweep_walk(child, fixture_name, out, list);
    }
}

/* AC2 — adjacency floor, checked ONCE the whole tree has been walked and
 * every clickable element gathered into `list`: every PAIR must clear
 * FF_HIT_MIN_GAP_PX edge to edge, unless they're the SAME logical control
 * (sweep_same_composite_control, Exclusion 1), one is an LVGL ancestor of
 * the other (sweep_is_ancestor, Exclusion 2), or either side IS the
 * whole-puck gesture region (Exclusion 3) — see this file's header
 * comment ("S17 slice b: the adjacency floor") for why a GLOBAL all-pairs
 * sweep, not one scoped to elements sharing an immediate parent, is what
 * "adjacent" actually has to mean here, and for the full rationale behind
 * all three exclusions. */
static void sweep_check_adjacency(sweep_clickable_list_t const *list, char const *fixture_name, sweep_result_t *out)
{
    for (int i = 0; i < list->n; i++) {
        for (int j = i + 1; j < list->n; j++) {
            sweep_clickable_t const *a = &list->items[i];
            sweep_clickable_t const *b = &list->items[j];

            if (a->is_whole_puck || b->is_whole_puck) {
                continue;
            }
            if (sweep_is_ancestor(a->obj, b->obj) || sweep_is_ancestor(b->obj, a->obj)) {
                continue;
            }
            if (sweep_same_composite_control(a, b)) {
                continue;
            }

            out->gap_checked++;
            float gap = sweep_rect_gap_px(a->rect, b->rect);
            if (gap < (float)FF_HIT_MIN_GAP_PX) {
                out->violations++;
                printf("  HIT-TARGETS-TOO-CLOSE [%s]  rect_a=(%.0f,%.0f)-(%.0f,%.0f)  "
                       "rect_b=(%.0f,%.0f)-(%.0f,%.0f)  gap=%.1fpx  (floor %dpx)\n",
                       fixture_name, (double)a->rect.x1, (double)a->rect.y1, (double)a->rect.x2, (double)a->rect.y2,
                       (double)b->rect.x1, (double)b->rect.y1, (double)b->rect.x2, (double)b->rect.y2, (double)gap,
                       (int)FF_HIT_MIN_GAP_PX);
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
    sweep_clickable_list_t clickables = {.n = 0};
    sweep_walk(lv_screen_active(), name, &result, &clickables);
    sweep_check_adjacency(&clickables, name, &result);

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

    printf("test_face_hit_targets: swept %d fixture(s), checked %d clickable element(s), %d pair(s) for the "
           "adjacency floor, %d violation(s)\n",
           fixtures_swept, total_checked, total_gap_checked, total_violations);

    /* Sanity: the sweep must actually have found fixtures and clickable
     * elements to check — a silently-empty directory or an all-skipped
     * walk would make this test vacuously pass without proving anything
     * (the mutation-test failure mode this whole file exists to avoid). */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, fixtures_swept, "no fixtures found — FF_FIXTURE_DIR misconfigured?");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, total_checked, "swept fixtures but found zero clickable elements to check");
    /* Same sanity, for AC2: at least one fixture must actually exercise two
     * independent elements close enough together to be a meaningful test of
     * the adjacency floor (not just the size floor above) — otherwise a
     * shrunk FF_HIT_MIN_GAP_PX could silently stop mattering and nothing
     * here would notice. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, total_gap_checked,
                                          "swept fixtures but found zero pairs to check the adjacency floor against");

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, total_violations,
                                   "one or more interactive elements are off-glass, under the 44px hit-target floor, "
                                   "or closer than the adjacency floor to a sibling control — see the HIT-* lines "
                                   "printed above for exactly which ones");
}

/* PR #86 code review (should-fix, non-blocking): `sweep_same_composite_
 * control`'s EXCLUSION never actually flips any fixture's PASS/FAIL
 * result today — every currently-committed composite pair (e.g.
 * scr_settings.c's WATER NUDGE label + its own value chip) sits at
 * FF_SETTINGS_CHIP_GAP (24px), already comfortably above FF_HIT_MIN_GAP_PX
 * (8px) whether or not the exclusion fires. Mutating the function to
 * `return false;` unconditionally confirmed this: every fixture still
 * sweeps at 0 violations. No committed fixture CAN exercise the exclusion
 * end-to-end by construction — the label/chip gap is a compile-time
 * layout constant (`FF_SETTINGS_CHIP_GAP`), never fixture data, so no
 * *.json fixture changes that geometry; deliberately shrinking it just to
 * manufacture a fixture-visible failure would mean carrying a real,
 * uncomfortably-tight production gap purely to satisfy this test, which
 * is worse than the gap it would be proving. Direct unit tests of the
 * function itself are the honest way to verify its two branches instead:
 * true only when BOTH the callback pointer and user_data match, false the
 * moment either differs, and false (never a false-positive "composite")
 * when either side never registered a callback at all. */
/* A trivial LV_EVENT_CLICKED handler, only ever registered (never fired)
 * by S17b_AC2_composite_control_detection below — the test compares
 * registered-callback IDENTITY, not behavior, so any real lv_event_cb_t
 * function pointer works. */
static void sweep_test_dummy_click_cb(lv_event_t *e)
{
    (void)e;
}

static void S17b_AC2_composite_control_detection(void)
{
    lv_init();
    lv_tick_set_cb(sweep_tick_cb);

    /* Same minimal display setup sweep_fixture uses above — LVGL needs a
     * real display attached before lv_screen_active()/lv_obj_create() are
     * well-defined; skipping this step (an earlier draft of this test
     * did) hung the process instead of failing cleanly. */
    const int32_t w = FF_THEME_WINDOW_PX;
    const int32_t h = FF_THEME_WINDOW_PX;
    const uint32_t buf_size = (uint32_t)(w * h * 4);
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    TEST_ASSERT_NOT_NULL(buf);
    lv_display_t *disp = lv_display_create(w, h);
    lv_display_set_buffers(disp, buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, sweep_flush_cb);
    lv_display_set_default(disp);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *btn_a = lv_button_create(scr);
    lv_obj_t *btn_b = lv_button_create(scr);
    lv_obj_t *btn_c = lv_button_create(scr);
    int dummy_data_x, dummy_data_y;

    lv_obj_add_event_cb(btn_a, sweep_test_dummy_click_cb, LV_EVENT_CLICKED, &dummy_data_x);
    lv_obj_add_event_cb(btn_b, sweep_test_dummy_click_cb, LV_EVENT_CLICKED, &dummy_data_x);
    lv_obj_add_event_cb(btn_c, sweep_test_dummy_click_cb, LV_EVENT_CLICKED, &dummy_data_y);

    sweep_clickable_t a = {.obj = btn_a,
                            .rect = {0},
                            .cb = (void *)lv_event_dsc_get_cb(lv_obj_get_event_dsc(btn_a, 0)),
                            .user_data = lv_event_dsc_get_user_data(lv_obj_get_event_dsc(btn_a, 0))};
    sweep_clickable_t b = {.obj = btn_b,
                            .rect = {0},
                            .cb = (void *)lv_event_dsc_get_cb(lv_obj_get_event_dsc(btn_b, 0)),
                            .user_data = lv_event_dsc_get_user_data(lv_obj_get_event_dsc(btn_b, 0))};
    sweep_clickable_t c = {.obj = btn_c,
                            .rect = {0},
                            .cb = (void *)lv_event_dsc_get_cb(lv_obj_get_event_dsc(btn_c, 0)),
                            .user_data = lv_event_dsc_get_user_data(lv_obj_get_event_dsc(btn_c, 0))};
    sweep_clickable_t no_cb = {.obj = scr, .rect = {0}, .cb = NULL, .user_data = NULL};

    TEST_ASSERT_TRUE_MESSAGE(sweep_same_composite_control(&a, &b),
                              "same callback + same user_data must be detected as one logical control");
    TEST_ASSERT_FALSE_MESSAGE(sweep_same_composite_control(&a, &c),
                               "same callback but DIFFERENT user_data must NOT be treated as composite");
    TEST_ASSERT_FALSE_MESSAGE(sweep_same_composite_control(&a, &no_cb),
                               "a side with no registered callback must never collapse into a false composite match");

    free(buf);
    lv_deinit();
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S08_hit_targets_every_committed_fixture_fits_the_glass);
    RUN_TEST(S17b_AC2_composite_control_detection);

    return UNITY_END();
}
