/**
 * test_route.c — unit tests for app/include/ff_route.h (S16 slice a).
 *
 * Test names mirror the spec's acceptance criteria numbering per
 * AGENTS.md: `S16_ACn_description` for the three criteria this slice
 * owns (AC1 bounded swipe, AC2 modal suppresses swipe, AC3 takeover
 * overrides without mutating). Tests named without an AC prefix cover
 * the header's contract around those criteria — init, the modal
 * lifecycle, argument rejection, NULL guards — the same way
 * test_ff_layout.c names its non-criteria geometry cases.
 *
 * Pure C11, no LVGL, no fixtures: ff_route is plain state machine logic,
 * so every assertion here is a direct check on the real struct rather
 * than a pixel-diff proxy for one.
 */
#include <string.h>

#include "unity.h"

#include "ff_route.h"

void setUp(void) {}
void tearDown(void) {}

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
     * three faces, so two swipes always suffice. */
    for (int guard = 0; r.base != base; guard++) {
        TEST_ASSERT_LESS_THAN_INT_MESSAGE(3, guard, "ff_route_swipe is not advancing toward the target base");
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

static void S16_AC1_three_forward_swipes_end_at_signals_not_radar(void)
{
    /* The wrap check, stated exactly as AC1 phrases it: modular
     * arithmetic would land back on RADAR here. */
    ff_route_t r = route_at(FF_APP_FACE_RADAR);

    TEST_ASSERT_TRUE(ff_route_swipe(&r, 1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NOW, r.base);

    TEST_ASSERT_TRUE(ff_route_swipe(&r, 1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_SIGNALS, r.base);

    TEST_ASSERT_FALSE(ff_route_swipe(&r, 1)); /* off the end: no-op */
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_SIGNALS, r.base);
}

static void S16_AC1_swipe_forward_from_signals_is_a_no_op(void)
{
    ff_route_t r = route_at(FF_APP_FACE_SIGNALS);
    TEST_ASSERT_FALSE(ff_route_swipe(&r, 1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_SIGNALS, r.base);
}

static void S16_AC1_swipe_axis_is_symmetric_and_round_trips(void)
{
    /* -1 is toward RADAR and +1 toward SIGNALS — the direction contract
     * the header warns is NOT a finger direction. Asserted as a full
     * round trip so an inverted axis cannot pass. */
    ff_route_t r = route_at(FF_APP_FACE_RADAR);

    TEST_ASSERT_TRUE(ff_route_swipe(&r, 1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NOW, r.base);
    TEST_ASSERT_TRUE(ff_route_swipe(&r, 1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_SIGNALS, r.base);
    TEST_ASSERT_TRUE(ff_route_swipe(&r, -1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NOW, r.base);
    TEST_ASSERT_TRUE(ff_route_swipe(&r, -1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, r.base);
}

static void swipe_rejects_directions_other_than_plus_or_minus_one(void)
{
    /* A raw gesture delta or a zeroed intent payload must not move the
     * route by an arbitrary number of tiles. */
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

static void S16_AC2_settings_modal_suppresses_swipe_in_both_directions(void)
{
    /* AC2 names both modals explicitly: the rule is "a modal is up",
     * not "Compose is up". */
    ff_route_t r = route_at(FF_APP_FACE_NOW);
    TEST_ASSERT_TRUE(ff_route_push_modal(&r, FF_APP_FACE_SETTINGS));

    TEST_ASSERT_FALSE(ff_route_swipe(&r, 1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NOW, r.base);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_SETTINGS, r.modal);

    TEST_ASSERT_FALSE(ff_route_swipe(&r, -1));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NOW, r.base);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_SETTINGS, r.modal);
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

static void visible_is_base_when_no_modal_and_no_takeover(void)
{
    ff_app_face_t const bases[] = {FF_APP_FACE_RADAR, FF_APP_FACE_NOW, FF_APP_FACE_SIGNALS};
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        ff_route_t r = route_at(bases[i]);
        TEST_ASSERT_EQUAL_INT(bases[i], ff_route_visible(&r, false));
    }
}

static void visible_is_the_modal_when_one_is_up(void)
{
    ff_app_face_t const modals[] = {FF_APP_FACE_COMPOSE, FF_APP_FACE_SETTINGS};
    for (size_t i = 0; i < sizeof(modals) / sizeof(modals[0]); i++) {
        ff_route_t r = route_at(FF_APP_FACE_RADAR);
        TEST_ASSERT_TRUE(ff_route_push_modal(&r, modals[i]));
        TEST_ASSERT_EQUAL_INT(modals[i], ff_route_visible(&r, false));
    }
}

static void visible_of_null_is_none(void)
{
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, ff_route_visible(NULL, false));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, ff_route_visible(NULL, true));
}

/* ------------------------------------------------------------------- */
/* modal lifecycle                                                      */
/* ------------------------------------------------------------------- */

static void push_modal_accepts_only_compose_and_settings(void)
{
    /* FLARE is rejected on purpose: the takeover is not routed, it
     * overrides — accepting it as a modal would put ff_flare_t's single
     * fact in a second place. */
    ff_app_face_t const rejected[] = {
        FF_APP_FACE_NONE, FF_APP_FACE_RADAR, FF_APP_FACE_NOW,
        FF_APP_FACE_SIGNALS, FF_APP_FACE_FLARE,
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        ff_route_t r = route_at(FF_APP_FACE_RADAR);
        TEST_ASSERT_FALSE(ff_route_push_modal(&r, rejected[i]));
        TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, r.modal);
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
     * half-typed Compose draft. */
    ff_route_t r = route_at(FF_APP_FACE_SIGNALS);
    TEST_ASSERT_TRUE(ff_route_push_modal(&r, FF_APP_FACE_COMPOSE));
    TEST_ASSERT_FALSE(ff_route_push_modal(&r, FF_APP_FACE_SETTINGS));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_COMPOSE, r.modal);
    /* Even re-pushing the same modal reports no change. */
    TEST_ASSERT_FALSE(ff_route_push_modal(&r, FF_APP_FACE_COMPOSE));
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_COMPOSE, r.modal);
}

static void push_modal_on_a_base_off_the_axis_is_a_no_op(void)
{
    /* The counterpart to swipe_on_a_base_off_the_axis_is_a_no_op, and
     * the sharper of the two: a forgotten ff_route_init must not be
     * MASKED by a modal that behaves perfectly until it is popped
     * (PR #36 review, D1). Both modal faces, since the rule is about
     * the base, not about which modal is being raised. */
    ff_app_face_t const modals[] = {FF_APP_FACE_COMPOSE, FF_APP_FACE_SETTINGS};
    for (size_t i = 0; i < sizeof(modals) / sizeof(modals[0]); i++) {
        ff_route_t r;
        memset(&r, 0, sizeof(r)); /* the deliberately-invalid zero value */
        TEST_ASSERT_FALSE(ff_route_push_modal(&r, modals[i]));
        TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, r.modal);
        /* The point of the guard: `visible` must not report a working
         * face for a route that was never initialised. */
        TEST_ASSERT_EQUAL_INT(FF_APP_FACE_NONE, ff_route_visible(&r, false));
    }
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
    TEST_ASSERT_TRUE(ff_route_push_modal(&r, FF_APP_FACE_SETTINGS));
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

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(init_opens_on_radar_with_no_modal);
    RUN_TEST(init_is_null_safe);

    RUN_TEST(S16_AC1_swipe_back_from_radar_is_a_no_op);
    RUN_TEST(S16_AC1_three_forward_swipes_end_at_signals_not_radar);
    RUN_TEST(S16_AC1_swipe_forward_from_signals_is_a_no_op);
    RUN_TEST(S16_AC1_swipe_axis_is_symmetric_and_round_trips);
    RUN_TEST(swipe_rejects_directions_other_than_plus_or_minus_one);
    RUN_TEST(swipe_on_a_base_off_the_axis_is_a_no_op);
    RUN_TEST(swipe_is_null_safe);

    RUN_TEST(S16_AC2_compose_modal_suppresses_swipe_in_both_directions);
    RUN_TEST(S16_AC2_settings_modal_suppresses_swipe_in_both_directions);
    RUN_TEST(S16_AC2_swipe_resumes_once_the_modal_is_popped);

    RUN_TEST(S16_AC3_takeover_returns_flare_and_leaves_route_byte_identical);
    RUN_TEST(S16_AC3_clearing_takeover_restores_the_prior_modal_not_base);
    RUN_TEST(S16_AC3_takeover_overrides_a_bare_base_face_too);
    RUN_TEST(S16_AC3_takeover_is_not_stored_so_two_reads_can_disagree);

    RUN_TEST(visible_is_base_when_no_modal_and_no_takeover);
    RUN_TEST(visible_is_the_modal_when_one_is_up);
    RUN_TEST(visible_of_null_is_none);

    RUN_TEST(push_modal_accepts_only_compose_and_settings);
    RUN_TEST(push_modal_leaves_base_untouched);
    RUN_TEST(push_modal_over_a_live_modal_is_rejected);
    RUN_TEST(push_modal_on_a_base_off_the_axis_is_a_no_op);
    RUN_TEST(pop_modal_on_a_base_off_the_axis_still_pops);
    RUN_TEST(pop_modal_restores_the_base_face);
    RUN_TEST(pop_modal_with_nothing_up_is_a_no_op);
    RUN_TEST(modal_null_guards_are_no_ops_not_crashes);

    return UNITY_END();
}
