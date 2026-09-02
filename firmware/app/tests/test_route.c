/**
 * test_route.c — unit tests for app/include/ff_route.h (S16 slice a,
 * extended by the horizontal-carousel rework).
 *
 * Test names mirror the spec's acceptance criteria numbering per
 * AGENTS.md: `S16_ACn_description` for the criteria this module owns
 * (AC1 bounded swipe, AC2 modal suppresses swipe, AC3 takeover overrides
 * without mutating). Tests named without an AC prefix cover the header's
 * contract around those criteria — init, the modal lifecycle, argument
 * rejection, NULL guards — the same way test_ff_layout.c names its
 * non-criteria geometry cases.
 *
 * The horizontal-carousel rework changed the swipe axis from three faces
 * (Radar/Now/Signals) to five (Radar · Now · Signals · Map · Settings),
 * made Compose the sole modal (Map and Settings became swipe faces), and
 * added ff_route_goto (the long-press jump-to-Settings shortcut). The AC1
 * bound/no-wrap and AC2 modal-suppression PROPERTIES are unchanged; only
 * the set they range over grew, so the tests below assert the new order
 * end to end rather than trusting the count.
 *
 * Pure C11, no LVGL, no fixtures: ff_route is plain state machine logic,
 * so every assertion here is a direct check on the real struct rather
 * than a pixel-diff proxy for one.
 *
 * S26 slice e (docs/specs/S26-device-lifecycle.md "(e) Home button +
 * launcher") added `ff_route_home`/`ff_route_launcher_select` — the
 * BOOT-button nav model, tested under `S26e_AC1_*` below — and retired
 * the swipe/goto-based navigation `scr_nav.c` used to drive, WITHOUT
 * touching `ff_route_swipe`/`ff_route_goto` themselves or any test
 * above this comment: both stay dormant pure primitives, still fully
 * covered, with no live caller left anywhere in the app (see
 * ff_route.h's header note on that call).
 */
#include <string.h>

#include "unity.h"

#include "ff_route.h"

void setUp(void) {}
void tearDown(void) {}

/* The swipe axis under test, in order — the tests assert the WHOLE
 * sequence against this so a reordering (or a dropped/added face) fails
 * loudly rather than passing on a count that happens to still match. */
static ff_app_face_t const k_axis[] = {
    FF_APP_FACE_RADAR, FF_APP_FACE_NOW, FF_APP_FACE_SIGNALS, FF_APP_FACE_MAP, FF_APP_FACE_SETTINGS,
};
enum { AXIS_N = (int)(sizeof(k_axis) / sizeof(k_axis[0])) };

/* Helper: a route parked on a given base with no modal. Built through
 * the real API (init + swipes) rather than by poking fields, so the
 * tests below can never assert against a route the module itself would
 * refuse to produce. */
static ff_route_t route_at(ff_app_face_t base)
{
    ff_route_t r;
    ff_route_init(&r);
    /* Bounded, not a bare `while` (PR #36 review, N1): an ff_route_swipe
     * that returned true without advancing would spin here forever, and
     * since every test below routes through this helper, CI would report
     * a ctest timeout instead of naming the broken function. The axis is
     * five faces, so at most four swipes always suffice. */
    for (int guard = 0; r.base != base; guard++) {
        TEST_ASSERT_LESS_THAN_INT_MESSAGE(AXIS_N, guard, "ff_route_swipe is not advancing toward the target base");
        TEST_ASSERT_TRUE(ff_route_swipe(&r, 1));
    }
    return r;
}

/* ------------------------------------------------------------------- */
/* ff_route_init                                                        */
/* ------------------------------------------------------------------- */

static void init_opens_on_radar_with_no_modal(void)
{
    ff_route_t r;
    memset(&r, 0xAA, sizeof(r)); /* garbage in: init must fully define both fields */
    ff_route_init(&r);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, r.base);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, r.modal);
}

static void init_is_null_safe(void)
{
    ff_route_init(NULL); /* must not crash */
}

/* ------------------------------------------------------------------- */
/* AC1 — swipe is bounded, not wrapping                                 */
/* ------------------------------------------------------------------- */

static void S16_AC1_swipe_back_from_radar_is_a_no_op(void)
{
    ff_route_t r = route_at(FF_APP_FACE_RADAR);
    TEST_ASSERT_FALSE(ff_route_swipe(&r, -1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, r.base);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, r.modal);
}

/* The new 5-face order, forward, end to end: RADAR -> NOW -> SIGNALS ->
 * MAP -> SETTINGS, then off the right end is a no-op. This is the
 * horizontal-carousel replacement for the old "three swipes end at
 * SIGNALS" test — Map and Settings are ordinary neighbours now, so the
 * traversal runs all the way to the SETTINGS bound, and modular
 * arithmetic (which would wrap SETTINGS back to RADAR) is ruled out. */
static void S16_AC1_forward_traverses_all_five_then_clamps_at_settings(void)
{
    ff_route_t r;
    ff_route_init(&r);
    for (int i = 1; i < AXIS_N; i++) {
        TEST_ASSERT_TRUE(ff_route_swipe(&r, 1));
        TEST_ASSERT_EQUAL_INT(k_axis[i], r.base);
    }
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_SETTINGS, r.base);
    TEST_ASSERT_FALSE(ff_route_swipe(&r, 1)); /* off the right end: no-op, no wrap */
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_SETTINGS, r.base);
}

static void S16_AC1_swipe_forward_from_settings_is_a_no_op(void)
{
    ff_route_t r = route_at(FF_APP_FACE_SETTINGS);
    TEST_ASSERT_FALSE(ff_route_swipe(&r, 1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_SETTINGS, r.base);
}

static void S16_AC1_swipe_axis_is_symmetric_and_round_trips(void)
{
    /* -1 is toward RADAR and +1 toward SETTINGS — the direction contract
     * the header warns is NOT a finger direction. Asserted as a full
     * round trip across all five faces so an inverted axis cannot pass. */
    ff_route_t r;
    ff_route_init(&r);
    for (int i = 1; i < AXIS_N; i++) {
        TEST_ASSERT_TRUE(ff_route_swipe(&r, 1));
        TEST_ASSERT_EQUAL_INT(k_axis[i], r.base);
    }
    for (int i = AXIS_N - 2; i >= 0; i--) {
        TEST_ASSERT_TRUE(ff_route_swipe(&r, -1));
        TEST_ASSERT_EQUAL_INT(k_axis[i], r.base);
    }
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, r.base);
}

/* Map and Settings are swipe faces now, not modals — pin that they are
 * reached by an ordinary swipe from their neighbour, the exact behaviour
 * the rework is about. Named so a regression reads as "Map/Settings fell
 * off the swipe axis", not folded into the generic traversal above. */
static void carousel_signals_map_settings_are_swipe_neighbours(void)
{
    ff_route_t r = route_at(FF_APP_FACE_SIGNALS);
    TEST_ASSERT_TRUE(ff_route_swipe(&r, 1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_MAP, r.base);
    TEST_ASSERT_TRUE(ff_route_swipe(&r, 1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_SETTINGS, r.base);
    TEST_ASSERT_TRUE(ff_route_swipe(&r, -1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_MAP, r.base);
}

static void swipe_rejects_directions_other_than_plus_or_minus_one(void)
{
    /* A raw gesture delta or a zeroed intent payload must not move the
     * route by an arbitrary number of tiles. This is the route-level half
     * of the "a vertical drag never changes face" rule — the screen half
     * (LV_DIR_TOP/BOTTOM emit nothing) lives in test_scr_intent.c. */
    int8_t const bad_dirs[] = {0, 2, -2, 3, 127, -128};
    for (size_t i = 0; i < sizeof(bad_dirs) / sizeof(bad_dirs[0]); i++) {
        ff_route_t r = route_at(FF_APP_FACE_NOW);
        TEST_ASSERT_FALSE(ff_route_swipe(&r, bad_dirs[i]));
        TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NOW, r.base);
    }
}

static void swipe_on_a_base_off_the_axis_is_a_no_op(void)
{
    /* An uninitialised route (base == NONE, the zero value) stays put
     * rather than being silently "repaired" to Radar mid-gesture. */
    ff_route_t r;
    memset(&r, 0, sizeof(r));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, r.base); /* the zero value is invalid on purpose */
    TEST_ASSERT_FALSE(ff_route_swipe(&r, 1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, r.base);
    TEST_ASSERT_FALSE(ff_route_swipe(&r, -1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, r.base);
}

static void swipe_is_null_safe(void)
{
    TEST_ASSERT_FALSE(ff_route_swipe(NULL, 1));
    TEST_ASSERT_FALSE(ff_route_swipe(NULL, -1));
}

/* ------------------------------------------------------------------- */
/* ff_route_goto — the long-press jump-to-a-face shortcut               */
/* ------------------------------------------------------------------- */

static void goto_jumps_straight_to_a_far_face(void)
{
    /* From Radar to Settings in one call — the nav long-press shortcut,
     * skipping the intermediate swipes ff_route_swipe would take. */
    ff_route_t r = route_at(FF_APP_FACE_RADAR);
    TEST_ASSERT_TRUE(ff_route_goto(&r, FF_APP_FACE_SETTINGS));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_SETTINGS, r.base);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, r.modal);
}

static void goto_reaches_every_swipe_face(void)
{
    for (int i = 0; i < AXIS_N; i++) {
        ff_route_t r = route_at(FF_APP_FACE_NOW); /* mid-axis start */
        if (k_axis[i] == FF_APP_FACE_NOW) {
            TEST_ASSERT_FALSE(ff_route_goto(&r, k_axis[i])); /* already there: no change */
        } else {
            TEST_ASSERT_TRUE(ff_route_goto(&r, k_axis[i]));
        }
        TEST_ASSERT_EQUAL_INT(k_axis[i], r.base);
    }
}

static void goto_off_axis_targets_are_rejected(void)
{
    /* Compose/NONE/FLARE are not swipe faces — a jump to one is a no-op,
     * never a base set off the axis. */
    ff_app_face_t const bad[] = {FF_APP_FACE_NONE, FF_APP_FACE_COMPOSE, FF_APP_FACE_FLARE};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ff_route_t r = route_at(FF_APP_FACE_NOW);
        TEST_ASSERT_FALSE(ff_route_goto(&r, bad[i]));
        TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NOW, r.base);
    }
}

static void goto_is_suppressed_under_a_modal(void)
{
    /* A jump must not slide a half-typed Compose away, exactly as a swipe
     * may not (AC2). */
    ff_route_t r = route_at(FF_APP_FACE_NOW);
    TEST_ASSERT_TRUE(ff_route_push_modal(&r, FF_APP_FACE_COMPOSE));
    TEST_ASSERT_FALSE(ff_route_goto(&r, FF_APP_FACE_SETTINGS));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NOW, r.base);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_COMPOSE, r.modal);
}

static void goto_is_null_safe(void)
{
    TEST_ASSERT_FALSE(ff_route_goto(NULL, FF_APP_FACE_SETTINGS));
}

/* ------------------------------------------------------------------- */
/* AC2 — any modal suppresses swipe entirely                            */
/* ------------------------------------------------------------------- */

static void S16_AC2_compose_modal_suppresses_swipe_in_both_directions(void)
{
    ff_route_t r = route_at(FF_APP_FACE_NOW); /* mid-axis: both directions are otherwise legal */
    TEST_ASSERT_TRUE(ff_route_push_modal(&r, FF_APP_FACE_COMPOSE));

    TEST_ASSERT_FALSE(ff_route_swipe(&r, 1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NOW, r.base);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_COMPOSE, r.modal);

    TEST_ASSERT_FALSE(ff_route_swipe(&r, -1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NOW, r.base);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_COMPOSE, r.modal);
}

static void S16_AC2_swipe_resumes_once_the_modal_is_popped(void)
{
    ff_route_t r = route_at(FF_APP_FACE_NOW);
    TEST_ASSERT_TRUE(ff_route_push_modal(&r, FF_APP_FACE_COMPOSE));
    TEST_ASSERT_FALSE(ff_route_swipe(&r, 1));
    TEST_ASSERT_TRUE(ff_route_pop_modal(&r));
    TEST_ASSERT_TRUE(ff_route_swipe(&r, 1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_SIGNALS, r.base);
}

/* ------------------------------------------------------------------- */
/* AC3 — takeover overrides what's visible without mutating the route   */
/* ------------------------------------------------------------------- */

static void S16_AC3_takeover_returns_flare_and_leaves_route_byte_identical(void)
{
    ff_route_t r = route_at(FF_APP_FACE_NOW);
    TEST_ASSERT_TRUE(ff_route_push_modal(&r, FF_APP_FACE_COMPOSE));

    ff_route_t before;
    memcpy(&before, &r, sizeof(before));

    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_FLARE, ff_route_visible(&r, true));

    /* "byte-identical", as AC3 words it — not just field-equal. */
    TEST_ASSERT_EQUAL_MEMORY(&before, &r, sizeof(before));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NOW, r.base);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_COMPOSE, r.modal);
}

static void S16_AC3_clearing_takeover_restores_the_prior_modal_not_base(void)
{
    /* The half of AC3 that a "takeover pops the modal" implementation
     * would fail: after the takeover clears, the composer is still
     * there (draft intact, one layer up), NOT the base face. */
    ff_route_t r = route_at(FF_APP_FACE_NOW);
    TEST_ASSERT_TRUE(ff_route_push_modal(&r, FF_APP_FACE_COMPOSE));

    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_FLARE, ff_route_visible(&r, true));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_COMPOSE, ff_route_visible(&r, false));
    TEST_ASSERT_NOT_EQUAL_INT(FF_APP_FACE_NOW, ff_route_visible(&r, false));
}

static void S16_AC3_takeover_overrides_a_bare_base_face_too(void)
{
    ff_route_t r = route_at(FF_APP_FACE_SIGNALS);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_FLARE, ff_route_visible(&r, true));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_SIGNALS, ff_route_visible(&r, false));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_SIGNALS, r.base);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, r.modal);
}

static void S16_AC3_takeover_is_not_stored_so_two_reads_can_disagree(void)
{
    /* The reason `takeover` is a parameter and not a field: ff_flare_tick
     * clears takeover_active on expiry with no route involved, so back-
     * to-back calls with different `takeover` values must give different
     * answers off the SAME unmodified route. A cached copy would make
     * the second call still report FLARE. */
    ff_route_t r = route_at(FF_APP_FACE_RADAR);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_FLARE, ff_route_visible(&r, true));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, ff_route_visible(&r, false));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_FLARE, ff_route_visible(&r, true));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, ff_route_visible(&r, false));
}

/* ------------------------------------------------------------------- */
/* ff_route_visible — the non-takeover precedence                       */
/* ------------------------------------------------------------------- */

static void visible_is_base_across_the_whole_carousel(void)
{
    for (int i = 0; i < AXIS_N; i++) {
        ff_route_t r = route_at(k_axis[i]);
        TEST_ASSERT_EQUAL_INT(k_axis[i], ff_route_visible(&r, false));
    }
}

static void visible_is_the_modal_when_one_is_up(void)
{
    /* Compose is the only modal now. */
    ff_route_t r = route_at(FF_APP_FACE_RADAR);
    TEST_ASSERT_TRUE(ff_route_push_modal(&r, FF_APP_FACE_COMPOSE));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_COMPOSE, ff_route_visible(&r, false));
}

static void visible_of_null_is_none(void)
{
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, ff_route_visible(NULL, false));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, ff_route_visible(NULL, true));
}

/* ------------------------------------------------------------------- */
/* modal lifecycle — Compose and (S26 slice b) Power menu are the two    */
/* modal faces                                                          */
/* ------------------------------------------------------------------- */

static void push_modal_accepts_only_compose_power_menu_and_launcher(void)
{
    /* Everything but Compose/Power menu/Launcher is rejected. Map and
     * Settings in particular are swipe faces now — accepting either as
     * a modal would put a fact that lives on the swipe axis in a second
     * place. FLARE is rejected because the takeover overrides, it is
     * not routed. */
    ff_app_face_t const rejected[] = {
        FF_APP_FACE_NONE,     FF_APP_FACE_RADAR, FF_APP_FACE_NOW, FF_APP_FACE_SIGNALS,
        FF_APP_FACE_SETTINGS, FF_APP_FACE_MAP,   FF_APP_FACE_FLARE,
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        ff_route_t r = route_at(FF_APP_FACE_RADAR);
        TEST_ASSERT_FALSE(ff_route_push_modal(&r, rejected[i]));
        TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, r.modal);
    }

    ff_route_t r = route_at(FF_APP_FACE_RADAR);
    TEST_ASSERT_TRUE(ff_route_push_modal(&r, FF_APP_FACE_COMPOSE));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_COMPOSE, r.modal);

    ff_route_t r2 = route_at(FF_APP_FACE_RADAR);
    TEST_ASSERT_TRUE(ff_route_push_modal(&r2, FF_APP_FACE_POWER_MENU));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_POWER_MENU, r2.modal);

    ff_route_t r3 = route_at(FF_APP_FACE_RADAR);
    TEST_ASSERT_TRUE(ff_route_push_modal(&r3, FF_APP_FACE_LAUNCHER));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, r3.modal);
}

/* S26 slice b: the PWR long-press must not be able to open a second
 * modal over a live one either — a long-press while Compose is open (a
 * draft in progress) must leave the draft exactly as untouched as any
 * other modal-suppressed action does, and the reverse (Compose reached
 * while the power menu is up) is equally rejected — one slot, either
 * direction. */
static void push_modal_power_menu_over_compose_is_rejected_and_vice_versa(void)
{
    ff_route_t r = route_at(FF_APP_FACE_SIGNALS);
    TEST_ASSERT_TRUE(ff_route_push_modal(&r, FF_APP_FACE_COMPOSE));
    TEST_ASSERT_FALSE(ff_route_push_modal(&r, FF_APP_FACE_POWER_MENU));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_COMPOSE, r.modal);

    ff_route_t r2 = route_at(FF_APP_FACE_SIGNALS);
    TEST_ASSERT_TRUE(ff_route_push_modal(&r2, FF_APP_FACE_POWER_MENU));
    TEST_ASSERT_FALSE(ff_route_push_modal(&r2, FF_APP_FACE_COMPOSE));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_POWER_MENU, r2.modal);
}

/* Same swipe-suppression and visible() rules Compose already has,
 * pinned for Power menu too — a future change that special-cased Compose
 * in either function would leave Power menu wrongly swipeable/invisible
 * without this failing. */
static void power_menu_modal_suppresses_swipe_and_is_the_visible_face(void)
{
    ff_route_t r = route_at(FF_APP_FACE_RADAR);
    TEST_ASSERT_TRUE(ff_route_push_modal(&r, FF_APP_FACE_POWER_MENU));

    TEST_ASSERT_FALSE(ff_route_swipe(&r, 1));
    TEST_ASSERT_FALSE(ff_route_swipe(&r, -1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, r.base);

    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_POWER_MENU, ff_route_visible(&r, false));

    TEST_ASSERT_TRUE(ff_route_pop_modal(&r));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, ff_route_visible(&r, false));
}

/* Map and Settings are no longer modals — pin that push_modal refuses
 * them explicitly, so a future change that tried to re-add them as modals
 * (re-opening the scroll-vs-swipe bug) fails here rather than silently. */
static void push_modal_rejects_map_and_settings(void)
{
    ff_app_face_t const not_modals[] = {FF_APP_FACE_MAP, FF_APP_FACE_SETTINGS};
    for (size_t i = 0; i < sizeof(not_modals) / sizeof(not_modals[0]); i++) {
        ff_route_t r = route_at(FF_APP_FACE_RADAR);
        TEST_ASSERT_FALSE(ff_route_push_modal(&r, not_modals[i]));
        TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, r.modal);
        TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, r.base);
    }
}

static void push_modal_leaves_base_untouched(void)
{
    ff_route_t r = route_at(FF_APP_FACE_SIGNALS);
    TEST_ASSERT_TRUE(ff_route_push_modal(&r, FF_APP_FACE_COMPOSE));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_SIGNALS, r.base);
}

static void push_modal_over_a_live_modal_is_rejected(void)
{
    /* One slot, not a stack: replacing would silently discard a
     * half-typed Compose draft. Compose is the only modal, so the
     * second push is Compose again — re-pushing the live modal reports
     * no change. */
    ff_route_t r = route_at(FF_APP_FACE_SIGNALS);
    TEST_ASSERT_TRUE(ff_route_push_modal(&r, FF_APP_FACE_COMPOSE));
    TEST_ASSERT_FALSE(ff_route_push_modal(&r, FF_APP_FACE_COMPOSE));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_COMPOSE, r.modal);
}

static void push_modal_on_a_base_off_the_axis_is_a_no_op(void)
{
    /* The counterpart to swipe_on_a_base_off_the_axis_is_a_no_op, and
     * the sharper of the two: a forgotten ff_route_init must not be
     * MASKED by a modal that behaves perfectly until it is popped
     * (PR #36 review, D1). */
    ff_route_t r;
    memset(&r, 0, sizeof(r)); /* the deliberately-invalid zero value */
    TEST_ASSERT_FALSE(ff_route_push_modal(&r, FF_APP_FACE_COMPOSE));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, r.modal);
    /* The point of the guard: `visible` must not report a working face
     * for a route that was never initialised. */
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, ff_route_visible(&r, false));
}

static void pop_modal_on_a_base_off_the_axis_still_pops(void)
{
    /* Pins the DELIBERATE asymmetry with push_modal above: pop drains
     * state and moves an invalid route toward the visible NONE, so
     * guarding it would strand a caller inside a modal with no way out.
     * Fields are set directly because ff_route_push_modal can no longer
     * produce this shape — which is exactly what D1's fix accomplished,
     * and why this test has to build it by hand to keep testing it. */
    ff_route_t r;
    memset(&r, 0, sizeof(r));
    r.modal = FF_APP_FACE_COMPOSE;

    TEST_ASSERT_TRUE(ff_route_pop_modal(&r));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, r.modal);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, r.base); /* still invalid, now visibly so */
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, ff_route_visible(&r, false));
}

static void pop_modal_restores_the_base_face(void)
{
    ff_route_t r = route_at(FF_APP_FACE_NOW);
    TEST_ASSERT_TRUE(ff_route_push_modal(&r, FF_APP_FACE_COMPOSE));
    TEST_ASSERT_TRUE(ff_route_pop_modal(&r));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, r.modal);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NOW, r.base);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NOW, ff_route_visible(&r, false));
}

static void pop_modal_with_nothing_up_is_a_no_op(void)
{
    /* So a stray BACK on a bare face cannot be mistaken for a state
     * change by a caller keying off the return value. */
    ff_route_t r = route_at(FF_APP_FACE_RADAR);
    TEST_ASSERT_FALSE(ff_route_pop_modal(&r));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, r.base);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, r.modal);
}

static void modal_null_guards_are_no_ops_not_crashes(void)
{
    TEST_ASSERT_FALSE(ff_route_push_modal(NULL, FF_APP_FACE_COMPOSE));
    TEST_ASSERT_FALSE(ff_route_pop_modal(NULL));
}

/* ------------------------------------------------------------------- */
/* S26 slice e — ff_route_home (the BOOT-button nav model) + the        */
/* launcher's own ff_route_launcher_select                              */
/* ------------------------------------------------------------------- */

/* S26 AC1: BOOT from Radar -> launcher. */
static void S26e_AC1_home_from_radar_opens_the_launcher(void)
{
    ff_route_t r = route_at(FF_APP_FACE_RADAR);
    TEST_ASSERT_TRUE(ff_route_home(&r));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, r.base);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, r.modal);
}

/* S26 AC1: from the launcher -> Radar. */
static void S26e_AC1_home_from_the_launcher_returns_to_radar(void)
{
    ff_route_t r = route_at(FF_APP_FACE_RADAR);
    TEST_ASSERT_TRUE(ff_route_home(&r));
    TEST_ASSERT_TRUE(ff_route_home(&r));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, r.base);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, r.modal);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, ff_route_visible(&r, false));
}

/* S26 AC1: from EACH app (Now/Signals/Map/Settings) -> Radar, in one
 * press — not via the launcher. */
static void S26e_AC1_home_from_each_app_returns_straight_to_radar(void)
{
    ff_app_face_t const apps[] = {FF_APP_FACE_NOW, FF_APP_FACE_SIGNALS, FF_APP_FACE_MAP, FF_APP_FACE_SETTINGS};
    for (size_t i = 0; i < sizeof(apps) / sizeof(apps[0]); i++) {
        ff_route_t r = route_at(apps[i]);
        TEST_ASSERT_TRUE(ff_route_home(&r));
        TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, r.base);
        TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, r.modal);
    }
}

/* S26 AC1: Compose/Power-menu modal suppresses HOME, exactly like swipe. */
static void S26e_AC1_home_is_suppressed_by_compose(void)
{
    ff_route_t r = route_at(FF_APP_FACE_SIGNALS);
    TEST_ASSERT_TRUE(ff_route_push_modal(&r, FF_APP_FACE_COMPOSE));
    TEST_ASSERT_FALSE(ff_route_home(&r));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_SIGNALS, r.base);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_COMPOSE, r.modal); /* draft untouched */
}

static void S26e_AC1_home_is_suppressed_by_the_power_menu(void)
{
    ff_route_t r = route_at(FF_APP_FACE_RADAR);
    TEST_ASSERT_TRUE(ff_route_push_modal(&r, FF_APP_FACE_POWER_MENU));
    TEST_ASSERT_FALSE(ff_route_home(&r));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, r.base);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_POWER_MENU, r.modal);
}

/* S26 AC1 (via ff_route.h's own house rule): swipe no longer moves
 * base — ff_route_swipe is retired from every LIVE call path (no screen
 * emits FF_INTENT_SWIPE any more, and ff_shell.c's handler is a no-op),
 * which this test pins one layer down: while the launcher modal is up
 * (the state BOOT now puts the route in), a swipe is rejected by the
 * SAME generic modal-suppression rule every other modal already gets —
 * no LAUNCHER-specific carve-out was needed, or written. */
static void S26e_AC1_swipe_is_suppressed_while_the_launcher_is_open(void)
{
    ff_route_t r = route_at(FF_APP_FACE_RADAR);
    TEST_ASSERT_TRUE(ff_route_home(&r));
    TEST_ASSERT_FALSE(ff_route_swipe(&r, 1));
    TEST_ASSERT_FALSE(ff_route_swipe(&r, -1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, r.modal);
}

static void home_is_null_safe(void)
{
    TEST_ASSERT_FALSE(ff_route_home(NULL));
}

/* ff_route_launcher_select — the four app circles. */

static void launcher_select_reaches_every_circle(void)
{
    ff_app_face_t const circles[] = {FF_APP_FACE_NOW, FF_APP_FACE_SIGNALS, FF_APP_FACE_MAP, FF_APP_FACE_SETTINGS};
    for (size_t i = 0; i < sizeof(circles) / sizeof(circles[0]); i++) {
        ff_route_t r = route_at(FF_APP_FACE_RADAR);
        TEST_ASSERT_TRUE(ff_route_home(&r)); /* opens the launcher */
        TEST_ASSERT_TRUE(ff_route_launcher_select(&r, circles[i]));
        TEST_ASSERT_EQUAL_INT(circles[i], r.base);
        TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, r.modal);
        TEST_ASSERT_EQUAL_INT(circles[i], ff_route_visible(&r, false));
    }
}

/* Radar is home, not a circle — the launcher never offers it. */
static void launcher_select_rejects_radar(void)
{
    ff_route_t r = route_at(FF_APP_FACE_RADAR);
    TEST_ASSERT_TRUE(ff_route_home(&r));
    TEST_ASSERT_FALSE(ff_route_launcher_select(&r, FF_APP_FACE_RADAR));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, r.modal); /* still open */
}

static void launcher_select_rejects_non_swipe_faces(void)
{
    ff_app_face_t const bad[] = {
        FF_APP_FACE_NONE, FF_APP_FACE_COMPOSE, FF_APP_FACE_FLARE, FF_APP_FACE_POWER_MENU, FF_APP_FACE_LAUNCHER,
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ff_route_t r = route_at(FF_APP_FACE_RADAR);
        TEST_ASSERT_TRUE(ff_route_home(&r));
        TEST_ASSERT_FALSE(ff_route_launcher_select(&r, bad[i]));
        TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, r.modal);
        TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, r.base);
    }
}

/* A tap with no launcher open (a stray/late event) must not resurrect
 * it or move base out from under whatever IS actually showing. */
static void launcher_select_is_a_no_op_when_the_launcher_is_not_open(void)
{
    ff_route_t r = route_at(FF_APP_FACE_SIGNALS);
    TEST_ASSERT_FALSE(ff_route_launcher_select(&r, FF_APP_FACE_NOW));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_SIGNALS, r.base);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, r.modal);
}

static void launcher_select_is_null_safe(void)
{
    TEST_ASSERT_FALSE(ff_route_launcher_select(NULL, FF_APP_FACE_NOW));
}

/* ------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(init_opens_on_radar_with_no_modal);
    RUN_TEST(init_is_null_safe);

    RUN_TEST(S16_AC1_swipe_back_from_radar_is_a_no_op);
    RUN_TEST(S16_AC1_forward_traverses_all_five_then_clamps_at_settings);
    RUN_TEST(S16_AC1_swipe_forward_from_settings_is_a_no_op);
    RUN_TEST(S16_AC1_swipe_axis_is_symmetric_and_round_trips);
    RUN_TEST(carousel_signals_map_settings_are_swipe_neighbours);
    RUN_TEST(swipe_rejects_directions_other_than_plus_or_minus_one);
    RUN_TEST(swipe_on_a_base_off_the_axis_is_a_no_op);
    RUN_TEST(swipe_is_null_safe);

    RUN_TEST(goto_jumps_straight_to_a_far_face);
    RUN_TEST(goto_reaches_every_swipe_face);
    RUN_TEST(goto_off_axis_targets_are_rejected);
    RUN_TEST(goto_is_suppressed_under_a_modal);
    RUN_TEST(goto_is_null_safe);

    RUN_TEST(S16_AC2_compose_modal_suppresses_swipe_in_both_directions);
    RUN_TEST(S16_AC2_swipe_resumes_once_the_modal_is_popped);

    RUN_TEST(S16_AC3_takeover_returns_flare_and_leaves_route_byte_identical);
    RUN_TEST(S16_AC3_clearing_takeover_restores_the_prior_modal_not_base);
    RUN_TEST(S16_AC3_takeover_overrides_a_bare_base_face_too);
    RUN_TEST(S16_AC3_takeover_is_not_stored_so_two_reads_can_disagree);

    RUN_TEST(visible_is_base_across_the_whole_carousel);
    RUN_TEST(visible_is_the_modal_when_one_is_up);
    RUN_TEST(visible_of_null_is_none);

    RUN_TEST(push_modal_accepts_only_compose_power_menu_and_launcher);
    RUN_TEST(push_modal_power_menu_over_compose_is_rejected_and_vice_versa);
    RUN_TEST(power_menu_modal_suppresses_swipe_and_is_the_visible_face);
    RUN_TEST(push_modal_rejects_map_and_settings);
    RUN_TEST(push_modal_leaves_base_untouched);
    RUN_TEST(push_modal_over_a_live_modal_is_rejected);
    RUN_TEST(push_modal_on_a_base_off_the_axis_is_a_no_op);
    RUN_TEST(pop_modal_on_a_base_off_the_axis_still_pops);
    RUN_TEST(pop_modal_restores_the_base_face);
    RUN_TEST(pop_modal_with_nothing_up_is_a_no_op);
    RUN_TEST(modal_null_guards_are_no_ops_not_crashes);

    RUN_TEST(S26e_AC1_home_from_radar_opens_the_launcher);
    RUN_TEST(S26e_AC1_home_from_the_launcher_returns_to_radar);
    RUN_TEST(S26e_AC1_home_from_each_app_returns_straight_to_radar);
    RUN_TEST(S26e_AC1_home_is_suppressed_by_compose);
    RUN_TEST(S26e_AC1_home_is_suppressed_by_the_power_menu);
    RUN_TEST(S26e_AC1_swipe_is_suppressed_while_the_launcher_is_open);
    RUN_TEST(home_is_null_safe);

    RUN_TEST(launcher_select_reaches_every_circle);
    RUN_TEST(launcher_select_rejects_radar);
    RUN_TEST(launcher_select_rejects_non_swipe_faces);
    RUN_TEST(launcher_select_is_a_no_op_when_the_launcher_is_not_open);
    RUN_TEST(launcher_select_is_null_safe);

    return UNITY_END();
}
