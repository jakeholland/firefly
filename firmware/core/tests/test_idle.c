/**
 * test_idle.c — S26 slice (c) acceptance criteria for `ff_idle`
 * (docs/specs/S26-device-lifecycle.md, "(c) Inactivity -> dim -> screen
 * off", AC1).
 *
 * Test names follow the spec's numbered ACs: S26c_ACn_description.
 *
 * THE PROXY, stated up front (AGENTS.md standing rule item 6): the easy
 * proxy for "the FSM is time-based" is a single tick landing past a
 * threshold — which is satisfied even by an FSM with an off-by-one
 * boundary, or one that ignores keep_awake entirely and just happens to
 * be re-ticked with a small now_ms in the keep_awake test. So the
 * boundary tests below check ONE MS on both sides of each threshold
 * (14999 vs 15000, 29999 vs 30000), and the keep_awake test ticks PAST
 * both thresholds while keep_awake holds and asserts ACTIVE, then
 * releases it and re-checks that idle time resumed from the release
 * instant (not backdated to whenever keep_awake first became true) —
 * a version that simply skipped the transition check under keep_awake
 * but still advanced ref_ms as if nothing happened would fail that
 * "resumes from release, not from before" assertion, and a version that
 * forgot to re-pin ref_ms each keep_awake tick would slam straight to
 * OFF the instant keep_awake released (past both thresholds at once).
 */
#include <string.h>

#include "unity.h"

#include "ff_idle.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* Transition-time boundaries                                          */
/* ------------------------------------------------------------------- */

static void S26c_AC1_stays_active_below_dim_threshold(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, FF_IDLE_T_DIM_MS - 1, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle));
}

static void S26c_AC1_dims_exactly_at_dim_threshold(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, FF_IDLE_T_DIM_MS, false));
}

static void S26c_AC1_stays_dim_below_off_threshold(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, FF_IDLE_T_DIM_MS, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, FF_IDLE_T_OFF_MS - 1, false));
}

static void S26c_AC1_off_exactly_at_off_threshold(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, FF_IDLE_T_DIM_MS, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, FF_IDLE_T_OFF_MS, false));
}

/* A single large jump (no intervening ticks through DIM's boundary)
 * still lands in OFF at exactly FF_IDLE_T_OFF_MS elapsed — both
 * thresholds are measured from the same reference instant, not
 * "T_OFF_MS after entering DIM". */
static void S26c_AC1_off_reached_directly_without_visiting_dim_first(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 1000);

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, 1000 + FF_IDLE_T_OFF_MS, false));
}

/* ------------------------------------------------------------------- */
/* Reset-on-input from each state                                      */
/* ------------------------------------------------------------------- */

static void S26c_AC1_input_resets_to_active_from_dim(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, FF_IDLE_T_DIM_MS, false));

    ff_idle_input(&idle, FF_IDLE_T_DIM_MS + 500);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle));

    /* The DIM countdown restarts from the input instant, not from the
     * original t=0 — proves ref_ms was actually re-pinned, not just the
     * state flipped back. */
    uint32_t const restarted_dim_at = FF_IDLE_T_DIM_MS + 500 + FF_IDLE_T_DIM_MS;
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, restarted_dim_at - 1, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, restarted_dim_at, false));
}

static void S26c_AC1_input_resets_to_active_from_off(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, FF_IDLE_T_OFF_MS, false));

    ff_idle_input(&idle, FF_IDLE_T_OFF_MS + 42);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle));

    /* And it does not immediately slam back to OFF on the very next
     * tick — proves the natural-tick "OFF is sticky" short-circuit does
     * not also poison a fresh ACTIVE-from-input state. */
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, FF_IDLE_T_OFF_MS + 43, false));
}

static void S26c_AC1_input_resets_to_active_from_active(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, 1000, false));

    ff_idle_input(&idle, 1500);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle));
}

/* ------------------------------------------------------------------- */
/* No transition while keep_awake holds                                */
/* ------------------------------------------------------------------- */

static void S26c_AC1_keep_awake_holds_active_past_both_thresholds(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);

    /* Well past both FF_IDLE_T_DIM_MS and FF_IDLE_T_OFF_MS — a version
     * that ignored keep_awake would report DIM then OFF here. */
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, 20000, true));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, 60000, true));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle));
}

/* Idle time does not accrue while keep_awake holds: once it releases,
 * the DIM countdown starts fresh from the release instant, not
 * backdated to before keep_awake began. */
static void S26c_AC1_keep_awake_does_not_backdate_idle_time_on_release(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, 60000, true)); /* held awake well past OFF */

    /* keep_awake releases at t=60000. If idle time had kept accruing in
     * the background (the bug this test catches), the very next tick
     * would already read >= FF_IDLE_T_OFF_MS and jump straight to OFF. */
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, 60000 + FF_IDLE_T_DIM_MS - 1, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, 60000 + FF_IDLE_T_DIM_MS, false));
}

/* keep_awake also reactivates a state that was already forced OFF — a
 * notification/takeover arriving while the screen is off is itself a
 * wake. */
static void S26c_AC1_keep_awake_reactivates_from_forced_off(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    ff_idle_force_off(&idle);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_state(&idle));

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, 100, true));
}

/* ------------------------------------------------------------------- */
/* ff_idle_force_off                                                    */
/* ------------------------------------------------------------------- */

/* PWR SHORT_PRESS while ACTIVE = go OFF immediately, well short of
 * either elapsed-time threshold. */
static void S26c_AC1_force_off_from_active_ignores_elapsed_time(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, 100, false));

    ff_idle_force_off(&idle);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_state(&idle));
}

/* Forced OFF sticks under natural ticking — a version whose force_off
 * only set a transient flag (rather than the real state) would report
 * ACTIVE again on the next tick, since elapsed time is still far below
 * FF_IDLE_T_DIM_MS. */
static void S26c_AC1_force_off_sticks_under_natural_ticking(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    ff_idle_force_off(&idle);

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, 200, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, FF_IDLE_T_DIM_MS - 1, false));
}

/* Only ff_idle_input wakes a forced-OFF idle — mirrors "PWR SHORT_PRESS
 * while OFF = wake". */
static void S26c_AC1_force_off_then_input_wakes(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    ff_idle_force_off(&idle);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_state(&idle));

    ff_idle_input(&idle, 250);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle));
}

/* ------------------------------------------------------------------- */
/* NULL-safety                                                          */
/* ------------------------------------------------------------------- */

static void S26c_AC1_null_safe(void)
{
    ff_idle_init(NULL);
    ff_idle_input(NULL, 0);
    ff_idle_force_off(NULL);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(NULL, 0, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(NULL));
}

/* ------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S26c_AC1_stays_active_below_dim_threshold);
    RUN_TEST(S26c_AC1_dims_exactly_at_dim_threshold);
    RUN_TEST(S26c_AC1_stays_dim_below_off_threshold);
    RUN_TEST(S26c_AC1_off_exactly_at_off_threshold);
    RUN_TEST(S26c_AC1_off_reached_directly_without_visiting_dim_first);

    RUN_TEST(S26c_AC1_input_resets_to_active_from_dim);
    RUN_TEST(S26c_AC1_input_resets_to_active_from_off);
    RUN_TEST(S26c_AC1_input_resets_to_active_from_active);

    RUN_TEST(S26c_AC1_keep_awake_holds_active_past_both_thresholds);
    RUN_TEST(S26c_AC1_keep_awake_does_not_backdate_idle_time_on_release);
    RUN_TEST(S26c_AC1_keep_awake_reactivates_from_forced_off);

    RUN_TEST(S26c_AC1_force_off_from_active_ignores_elapsed_time);
    RUN_TEST(S26c_AC1_force_off_sticks_under_natural_ticking);
    RUN_TEST(S26c_AC1_force_off_then_input_wakes);

    RUN_TEST(S26c_AC1_null_safe);

    return UNITY_END();
}
