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
#include "ff_settings.h" /* FF_BRIGHTNESS_MIN_PCT — ff_idle_brightness_pct DIM expectation */

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

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, FF_IDLE_T_DIM_MS - 1, false, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle));
}

static void S26c_AC1_dims_exactly_at_dim_threshold(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, FF_IDLE_T_DIM_MS, false, false));
}

static void S26c_AC1_stays_dim_below_off_threshold(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, FF_IDLE_T_DIM_MS, false, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, FF_IDLE_T_OFF_MS - 1, false, false));
}

static void S26c_AC1_off_exactly_at_off_threshold(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, FF_IDLE_T_DIM_MS, false, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, FF_IDLE_T_OFF_MS, false, false));
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

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, 1000 + FF_IDLE_T_OFF_MS, false, false));
}

/* ------------------------------------------------------------------- */
/* Reset-on-input from each state                                      */
/* ------------------------------------------------------------------- */

static void S26c_AC1_input_resets_to_active_from_dim(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, FF_IDLE_T_DIM_MS, false, false));

    ff_idle_input(&idle, FF_IDLE_T_DIM_MS + 500);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle));

    /* The DIM countdown restarts from the input instant, not from the
     * original t=0 — proves ref_ms was actually re-pinned, not just the
     * state flipped back. */
    uint32_t const restarted_dim_at = FF_IDLE_T_DIM_MS + 500 + FF_IDLE_T_DIM_MS;
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, restarted_dim_at - 1, false, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, restarted_dim_at, false, false));
}

static void S26c_AC1_input_resets_to_active_from_off(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, FF_IDLE_T_OFF_MS, false, false));

    ff_idle_input(&idle, FF_IDLE_T_OFF_MS + 42);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle));

    /* And it does not immediately slam back to OFF on the very next
     * tick — proves the natural-tick "OFF is sticky" short-circuit does
     * not also poison a fresh ACTIVE-from-input state. */
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, FF_IDLE_T_OFF_MS + 43, false, false));
}

static void S26c_AC1_input_resets_to_active_from_active(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, 1000, false, false));

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
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, 20000, true, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, 60000, true, false));
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

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, 60000, true, false)); /* held awake well past OFF */

    /* keep_awake releases at t=60000. If idle time had kept accruing in
     * the background (the bug this test catches), the very next tick
     * would already read >= FF_IDLE_T_OFF_MS and jump straight to OFF. */
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, 60000 + FF_IDLE_T_DIM_MS - 1, false, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, 60000 + FF_IDLE_T_DIM_MS, false, false));
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

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, 100, true, false));
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
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, 100, false, false));

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

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, 200, false, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, FF_IDLE_T_DIM_MS - 1, false, false));
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
/* Threshold literals (review finding: every existing boundary test     */
/* referenced FF_IDLE_T_DIM_MS/_OFF_MS symbolically, so a mutation that  */
/* changes the MACRO's value slides the test's own expectation right    */
/* along with it and still passes — pin the spec's actual numbers here, */
/* independent of the macros, as a second, redundant check).            */
/* ------------------------------------------------------------------- */

static void S26c_AC1_thresholds_pinned_to_spec_literals(void)
{
    /* The macros themselves must equal the spec's numbers... */
    TEST_ASSERT_EQUAL_UINT32(15000u, FF_IDLE_T_DIM_MS);
    TEST_ASSERT_EQUAL_UINT32(30000u, FF_IDLE_T_OFF_MS);

    /* ...and the boundary behaviour is asserted against those SAME
     * literal numbers written directly into this test, not the macros —
     * so a mutation to either macro's value is caught twice: once by
     * the two asserts above, and independently again here (a version
     * that mutated the macro AND somehow kept these literals in sync
     * would still be a real spec violation the two asserts above
     * catch). */
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, 14999, false, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, 15000, false, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, 29999, false, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, 30000, false, false));
}

/* ------------------------------------------------------------------- */
/* ff_idle_short_press — the whole PWR SHORT_PRESS decision             */
/* ------------------------------------------------------------------- */

static void S26c_AC1_short_press_from_active_forces_off(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, 100, false, false));

    ff_idle_short_press(&idle, 100, false);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_state(&idle));
}

static void S26c_AC1_short_press_from_dim_wakes(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, FF_IDLE_T_DIM_MS, false, false));

    ff_idle_short_press(&idle, FF_IDLE_T_DIM_MS + 10, false);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle));
}

static void S26c_AC1_short_press_from_off_wakes(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    ff_idle_force_off(&idle);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_state(&idle));

    ff_idle_short_press(&idle, 500, false);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle));
}

/* keep_awake dominates: a no-op regardless of the current state (would
 * be re-forced to ACTIVE by the very next ff_idle_tick anyway — see
 * ff_idle.h's doc comment on this function). Checked from all three
 * starting states so a version that only special-cased ACTIVE (or only
 * OFF) is still caught. */
static void S26c_AC1_short_press_noop_while_keep_awake(void)
{
    ff_idle_t idle;

    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    ff_idle_short_press(&idle, 100, true);
    TEST_ASSERT_EQUAL_MESSAGE(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle), "keep_awake short_press acted from ACTIVE");

    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, FF_IDLE_T_DIM_MS, false, false));
    ff_idle_short_press(&idle, FF_IDLE_T_DIM_MS + 10, true);
    TEST_ASSERT_EQUAL_MESSAGE(FF_IDLE_STATE_DIM, ff_idle_state(&idle), "keep_awake short_press acted from DIM");

    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    ff_idle_force_off(&idle);
    ff_idle_short_press(&idle, 500, true);
    TEST_ASSERT_EQUAL_MESSAGE(FF_IDLE_STATE_OFF, ff_idle_state(&idle), "keep_awake short_press acted from OFF");
}

/* ------------------------------------------------------------------- */
/* ff_idle_brightness_pct — the whole brightness-enact decision (AC2)   */
/* ------------------------------------------------------------------- */

static void S26c_AC2_brightness_active_returns_stored_pct_unchanged(void)
{
    TEST_ASSERT_EQUAL_UINT8(10, ff_idle_brightness_pct(FF_IDLE_STATE_ACTIVE, 10));
    TEST_ASSERT_EQUAL_UINT8(70, ff_idle_brightness_pct(FF_IDLE_STATE_ACTIVE, 70));
    TEST_ASSERT_EQUAL_UINT8(100, ff_idle_brightness_pct(FF_IDLE_STATE_ACTIVE, 100));
}

static void S26c_AC2_brightness_dim_returns_core_brightness_min(void)
{
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FF_BRIGHTNESS_MIN_PCT, ff_idle_brightness_pct(FF_IDLE_STATE_DIM, 70));
    /* stored_pct must not leak through DIM regardless of its value. */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FF_BRIGHTNESS_MIN_PCT, ff_idle_brightness_pct(FF_IDLE_STATE_DIM, 100));
}

static void S26c_AC2_brightness_off_returns_true_zero(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, ff_idle_brightness_pct(FF_IDLE_STATE_OFF, 70));
    TEST_ASSERT_EQUAL_UINT8(0, ff_idle_brightness_pct(FF_IDLE_STATE_OFF, 10));
}

/* The round-trip AC2 is actually about: dim, then wake, restores the
 * EXACT pre-dim stored percent — never a hardcoded value. */
static void S26c_AC2_brightness_round_trip_active_dim_active(void)
{
    uint8_t const stored = 42;
    TEST_ASSERT_EQUAL_UINT8(stored, ff_idle_brightness_pct(FF_IDLE_STATE_ACTIVE, stored));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FF_BRIGHTNESS_MIN_PCT, ff_idle_brightness_pct(FF_IDLE_STATE_DIM, stored));
    TEST_ASSERT_EQUAL_UINT8(stored, ff_idle_brightness_pct(FF_IDLE_STATE_ACTIVE, stored));
}

/* ------------------------------------------------------------------- */
/* S26 slice (f) — timer-based light sleep after screen-off             */
/* ------------------------------------------------------------------- */

/* "OFF + 119,999 ms -> OFF; + 120,000 ms -> SLEEP" — the brief's own
 * literal boundary check, same one-ms-on-both-sides technique as the
 * DIM/OFF boundary tests above (this repo's proxy-check failure mode,
 * AGENTS.md item 6: a version with an off-by-one on this threshold
 * would still pass a test that only checked one side). */
static void S26f_AC1_stays_off_below_sleep_threshold(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, FF_IDLE_T_OFF_MS, false, false));

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF,
                       ff_idle_tick(&idle, FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS - 1, false, false));
}

static void S26f_AC1_sleeps_exactly_at_sleep_threshold(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, FF_IDLE_T_OFF_MS, false, false));

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_SLEEP,
                       ff_idle_tick(&idle, FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS, false, false));
}

/* A single large jump (no intervening ticks through DIM/OFF's own
 * boundaries) still lands in SLEEP directly — mirrors
 * S26c_AC1_off_reached_directly_without_visiting_dim_first for the new
 * state one level up. */
static void S26f_AC1_sleep_reached_directly_without_visiting_off_first(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 1000);

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_SLEEP,
                       ff_idle_tick(&idle, 1000 + FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS, false, false));
}

/* Threshold literal pin (mirrors S26c_AC1_thresholds_pinned_to_spec_literals):
 * the macro AND the boundary behaviour are both checked against the
 * spec's actual literal numbers, independent of each other, so a
 * mutation to the macro's value is caught twice. */
static void S26f_AC1_thresholds_pinned_to_spec_literals(void)
{
    TEST_ASSERT_EQUAL_UINT32(120000u, FF_IDLE_T_SLEEP_MS);

    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, 30000, false, false));

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, 30000 + 119999, false, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_SLEEP, ff_idle_tick(&idle, 30000 + 120000, false, false));
}

/* ff_idle_input wakes from SLEEP to ACTIVE like any other state (S26f). */
static void S26f_AC1_input_wakes_from_sleep_to_active(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_SLEEP,
                       ff_idle_tick(&idle, FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS, false, false));

    ff_idle_input(&idle, FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS + 7);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle));

    /* And SLEEP does not slam straight back on the very next tick — same
     * "does not immediately re-poison a fresh ACTIVE" check as
     * S26c_AC1_input_resets_to_active_from_off. */
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE,
                       ff_idle_tick(&idle, FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS + 8, false, false));
}

/* ff_idle_short_press (DIM/OFF/SLEEP -> wake) reaches SLEEP too — the
 * PWR-press-during-a-GPIO-wake path app_main.c's light-sleep loop relies
 * on (S26f target enact: "wake on input must land in ACTIVE"). */
static void S26f_AC1_short_press_from_sleep_wakes(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_SLEEP,
                       ff_idle_tick(&idle, FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS, false, false));

    ff_idle_short_press(&idle, FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS + 3, false);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle));
}

/* No SLEEP while keep_awake holds (S26f AC2) — reuses (c)'s predicate:
 * keep_awake unconditionally forces ACTIVE every tick regardless of
 * current state, so ticking well past BOTH the OFF and SLEEP thresholds
 * while it holds must never report SLEEP (or OFF, or DIM). */
static void S26f_AC2_no_sleep_while_keep_awake_holds(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE,
                       ff_idle_tick(&idle, FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS + 60000, true, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle));
}

/* keep_awake also reactivates a state that had already reached SLEEP —
 * mirrors S26c_AC1_keep_awake_reactivates_from_forced_off. */
static void S26f_AC2_keep_awake_reactivates_from_sleep(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_SLEEP,
                       ff_idle_tick(&idle, FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS, false, false));

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE,
                       ff_idle_tick(&idle, FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS + 1, true, false));
}

/* ff_idle_brightness_pct(SLEEP) = 0, a true zero — same contract as OFF. */
static void S26f_AC1_brightness_sleep_returns_true_zero(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, ff_idle_brightness_pct(FF_IDLE_STATE_SLEEP, 70));
    TEST_ASSERT_EQUAL_UINT8(0, ff_idle_brightness_pct(FF_IDLE_STATE_SLEEP, 10));
}

/* ------------------------------------------------------------------- */
/* S26 slice (f) AMENDMENT (2026-09-02) — sleep_inhibit (don't enter     */
/* light sleep while USB is connected)                                  */
/* ------------------------------------------------------------------- */

/* THE PROXY (AGENTS.md item 6): the easy proxy for "the OFF->SLEEP
 * transition is inhibited" is a single tick just past the sleep
 * threshold while inhibit holds — which a version with an off-by-one, or
 * one that only samples sleep_inhibit on the FIRST inhibited tick, could
 * still pass. So this ticks to 10x FF_IDLE_T_SLEEP_MS past OFF with
 * inhibit held true the whole way (this brief's own literal ask). */
static void S26f_amendment_sleep_inhibit_holds_off_well_past_sleep_threshold(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, FF_IDLE_T_OFF_MS, false, false));

    uint32_t const far_past_sleep = FF_IDLE_T_OFF_MS + (10u * FF_IDLE_T_SLEEP_MS);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, far_past_sleep, false, true));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_state(&idle));
}

/* Releasing inhibit when the sleep threshold has ALREADY elapsed enters
 * SLEEP on the very next tick — no extra delay, no re-arming needed:
 * inhibit only ever gated the one comparison, which is re-evaluated
 * fresh (and now true) the instant inhibit is false again. */
static void S26f_amendment_sleep_inhibit_release_after_elapsed_sleeps_next_tick(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, FF_IDLE_T_OFF_MS, false, false));

    uint32_t const far_past_sleep = FF_IDLE_T_OFF_MS + (10u * FF_IDLE_T_SLEEP_MS);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, far_past_sleep, false, true));

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_SLEEP, ff_idle_tick(&idle, far_past_sleep + 1, false, false));
}

/* Releasing inhibit BEFORE the sleep threshold has elapsed waits out
 * exactly the remaining time from the SAME unmoved reference — proves
 * inhibit never re-pinned ref_ms while it held (a version that re-pinned
 * ref_ms to the release instant would need another full
 * FF_IDLE_T_OFF_MS+FF_IDLE_T_SLEEP_MS from here and would still be OFF
 * at the boundary this test checks). */
static void S26f_amendment_sleep_inhibit_release_before_elapsed_sleeps_after_remaining_time(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, FF_IDLE_T_OFF_MS, false, false));

    /* Hold inhibit, but release WELL BEFORE the sleep threshold. */
    uint32_t const still_before_sleep = FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS - 5000;
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, still_before_sleep, false, true));

    /* Inhibit released now (sleep_inhibit=false from here on): the
     * threshold has not elapsed yet, still OFF one ms short of it, then
     * SLEEP exactly at it — the ORIGINAL boundary, not a new one counted
     * from the release instant. */
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF,
                       ff_idle_tick(&idle, FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS - 1, false, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_SLEEP,
                       ff_idle_tick(&idle, FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS, false, false));
}

/* sleep_inhibit is NOT keep_awake and must not re-pin ref_ms or block
 * DIM/OFF: both still happen at their normal thresholds even while
 * sleep_inhibit holds true the whole time (a version that folded
 * sleep_inhibit into keep_awake, or treated it as any kind of wake,
 * would stay ACTIVE here instead of dimming/going OFF on schedule). */
static void S26f_amendment_sleep_inhibit_does_not_block_dim_or_off(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, FF_IDLE_T_DIM_MS - 1, false, true));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, FF_IDLE_T_DIM_MS, false, true));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, FF_IDLE_T_OFF_MS - 1, false, true));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_tick(&idle, FF_IDLE_T_OFF_MS, false, true));
}

/* sleep_inhibit is independent of keep_awake in the other direction too:
 * keep_awake still forces + holds ACTIVE regardless of sleep_inhibit's
 * value (a version that let either parameter suppress the other's effect
 * would fail this). */
static void S26f_amendment_keep_awake_dominates_regardless_of_sleep_inhibit(void)
{
    ff_idle_t idle;
    ff_idle_init(&idle);
    ff_idle_input(&idle, 0);

    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE,
                       ff_idle_tick(&idle, FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS + 60000, true, true));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle));
}

/* ------------------------------------------------------------------- */
/* Wake-only touch/button gate (amendment, 2026-09-02 maintainer        */
/* decision, S26c) — see ff_idle.h's own doc comment for the full       */
/* contract this implements.                                            */
/*                                                                       */
/* THE PROXY (AGENTS.md item 6): the easy proxy for "a wake-only press   */
/* is swallowed" is checking the FIRST sample only — which a version     */
/* that swallows once and then delivers every subsequent sample (a       */
/* one-shot latch, not a held one) would still pass. So the DIM/OFF      */
/* tests below sample MULTIPLE times while still held, before release,   */
/* and assert every one of them reads "not delivered" — not just the     */
/* first. */
/* ------------------------------------------------------------------- */

static void S26_wakeonly_AC_press_during_dim_wakes_and_swallows_until_release(void)
{
    ff_idle_t idle;
    ff_idle_touch_gate_t gate;
    ff_idle_init(&idle);
    ff_idle_touch_gate_init(&gate);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, FF_IDLE_T_DIM_MS, false, false));

    /* Press begins while DIM: wakes, and this very first sample is
     * already swallowed (not "wakes, then delivers the same sample"). */
    TEST_ASSERT_FALSE(ff_idle_touch_gate(&idle, &gate, FF_IDLE_T_DIM_MS + 10, true));
    TEST_ASSERT_EQUAL_MESSAGE(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle), "press-begin-while-DIM did not wake");

    /* Held: every subsequent sample stays swallowed, not just the
     * first (the proxy this file's own note above calls out) — even
     * though idle is now ACTIVE, which a version that re-checked state
     * every sample (instead of latching at begin) would misread as
     * "deliver". */
    TEST_ASSERT_FALSE(ff_idle_touch_gate(&idle, &gate, FF_IDLE_T_DIM_MS + 20, true));
    TEST_ASSERT_FALSE(ff_idle_touch_gate(&idle, &gate, FF_IDLE_T_DIM_MS + 30, true));

    /* Release: not delivered either (nothing to deliver), and the latch
     * resets. */
    TEST_ASSERT_FALSE(ff_idle_touch_gate(&idle, &gate, FF_IDLE_T_DIM_MS + 40, false));

    /* Next press begins while ACTIVE (the wake stuck): delivered from
     * the very first sample. */
    TEST_ASSERT_TRUE(ff_idle_touch_gate(&idle, &gate, FF_IDLE_T_DIM_MS + 100, true));
    TEST_ASSERT_TRUE(ff_idle_touch_gate(&idle, &gate, FF_IDLE_T_DIM_MS + 110, true));
    TEST_ASSERT_TRUE(ff_idle_touch_gate(&idle, &gate, FF_IDLE_T_DIM_MS + 120, true));
    TEST_ASSERT_FALSE(ff_idle_touch_gate(&idle, &gate, FF_IDLE_T_DIM_MS + 130, false)); /* release */
}

static void S26_wakeonly_AC_press_during_off_wakes_and_swallows_until_release(void)
{
    ff_idle_t idle;
    ff_idle_touch_gate_t gate;
    ff_idle_init(&idle);
    ff_idle_touch_gate_init(&gate);
    ff_idle_input(&idle, 0);
    ff_idle_force_off(&idle);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_OFF, ff_idle_state(&idle));

    TEST_ASSERT_FALSE(ff_idle_touch_gate(&idle, &gate, 500, true));
    TEST_ASSERT_EQUAL_MESSAGE(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle), "press-begin-while-OFF did not wake");
    TEST_ASSERT_FALSE(ff_idle_touch_gate(&idle, &gate, 510, true));
    TEST_ASSERT_FALSE(ff_idle_touch_gate(&idle, &gate, 520, true));
    TEST_ASSERT_FALSE(ff_idle_touch_gate(&idle, &gate, 530, false)); /* release resets */

    TEST_ASSERT_TRUE(ff_idle_touch_gate(&idle, &gate, 600, true)); /* next press: delivered */
}

static void S26_wakeonly_AC_press_during_sleep_wakes_and_swallows_until_release(void)
{
    ff_idle_t idle;
    ff_idle_touch_gate_t gate;
    ff_idle_init(&idle);
    ff_idle_touch_gate_init(&gate);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_SLEEP,
                       ff_idle_tick(&idle, FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS, false, false));

    TEST_ASSERT_FALSE(ff_idle_touch_gate(&idle, &gate, FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS + 10, true));
    TEST_ASSERT_EQUAL_MESSAGE(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle), "press-begin-while-SLEEP did not wake");
    TEST_ASSERT_FALSE(ff_idle_touch_gate(&idle, &gate, FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS + 20, true));
}

static void S26_wakeonly_AC_press_during_active_delivers_from_first_sample(void)
{
    ff_idle_t idle;
    ff_idle_touch_gate_t gate;
    ff_idle_init(&idle);
    ff_idle_touch_gate_init(&gate);
    ff_idle_input(&idle, 0);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle));

    TEST_ASSERT_TRUE(ff_idle_touch_gate(&idle, &gate, 50, true)); /* first sample, no wake needed */
    TEST_ASSERT_TRUE(ff_idle_touch_gate(&idle, &gate, 60, true));
    TEST_ASSERT_FALSE(ff_idle_touch_gate(&idle, &gate, 70, false)); /* release */
}

/* "A press that starts ACTIVE and continues past a DIM transition is
 * still delivered" — state matters only at press START. The background
 * state is advanced via ff_idle_tick directly (not via this gesture's
 * own input) to isolate the property: the decision was locked in at
 * begin and must not waver just because idle drifted to DIM under it. */
static void S26_wakeonly_AC_press_started_active_stays_delivered_through_dim_transition(void)
{
    ff_idle_t idle;
    ff_idle_touch_gate_t gate;
    ff_idle_init(&idle);
    ff_idle_touch_gate_init(&gate);
    ff_idle_input(&idle, 0);

    TEST_ASSERT_TRUE(ff_idle_touch_gate(&idle, &gate, 100, true)); /* begins ACTIVE: delivered */

    /* Background idle ticks (not this gesture's own input) push idle
     * into DIM while the SAME gesture is still held. */
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, FF_IDLE_T_DIM_MS, false, false));

    /* Still the same held gesture: still delivered, because state only
     * mattered at press-begin. A version that re-checked idle's CURRENT
     * state on every sample would flip this to false here. */
    TEST_ASSERT_TRUE_MESSAGE(ff_idle_touch_gate(&idle, &gate, FF_IDLE_T_DIM_MS + 10, true),
                              "a held gesture stopped delivering after the background state dimmed under it");
    TEST_ASSERT_TRUE(ff_idle_touch_gate(&idle, &gate, FF_IDLE_T_DIM_MS + 20, true));

    TEST_ASSERT_FALSE(ff_idle_touch_gate(&idle, &gate, FF_IDLE_T_DIM_MS + 30, false)); /* release */
}

/* Literal pin (mirrors this file's other _thresholds_pinned_to_spec_
 * literals tests): the WAKE side-effect happens at the exact instant the
 * caller's now_ms names, not some other value — the wake is pinned to
 * a literal ref_ms, independent of ff_idle_input's own unit test making
 * the same claim in isolation. */
static void S26_wakeonly_AC_wake_pins_ref_ms_to_the_literal_press_instant(void)
{
    ff_idle_t idle;
    ff_idle_touch_gate_t gate;
    ff_idle_init(&idle);
    ff_idle_touch_gate_init(&gate);
    ff_idle_input(&idle, 0);
    ff_idle_force_off(&idle);

    uint32_t const press_ms = 777777u;
    TEST_ASSERT_FALSE(ff_idle_touch_gate(&idle, &gate, press_ms, true));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_state(&idle));

    /* The DIM countdown from this wake starts at press_ms, not 0 or any
     * other value — one ms short of press_ms+T_DIM_MS still ACTIVE,
     * exactly at it, DIM. */
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(&idle, press_ms + FF_IDLE_T_DIM_MS - 1, false, false));
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_DIM, ff_idle_tick(&idle, press_ms + FF_IDLE_T_DIM_MS, false, false));
}

/* Mirrors ff_idle_short_press's own NULL-safety convention: a NULL
 * idle/gate fails OPEN (returns `pressed` unchanged), never silently
 * blocks input over a wiring bug. */
static void S26_wakeonly_AC_null_safe_fails_open(void)
{
    ff_idle_touch_gate_t gate;
    ff_idle_touch_gate_init(&gate);
    ff_idle_t idle;
    ff_idle_init(&idle);

    ff_idle_touch_gate_init(NULL);
    TEST_ASSERT_TRUE(ff_idle_touch_gate(NULL, &gate, 0, true));
    TEST_ASSERT_FALSE(ff_idle_touch_gate(NULL, &gate, 0, false));
    TEST_ASSERT_TRUE(ff_idle_touch_gate(&idle, NULL, 0, true));
    TEST_ASSERT_FALSE(ff_idle_touch_gate(&idle, NULL, 0, false));
}

/* ------------------------------------------------------------------- */
/* NULL-safety                                                          */
/* ------------------------------------------------------------------- */

static void S26c_AC1_null_safe(void)
{
    ff_idle_init(NULL);
    ff_idle_input(NULL, 0);
    ff_idle_force_off(NULL);
    TEST_ASSERT_EQUAL(FF_IDLE_STATE_ACTIVE, ff_idle_tick(NULL, 0, false, false));
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

    RUN_TEST(S26c_AC1_thresholds_pinned_to_spec_literals);

    RUN_TEST(S26c_AC1_short_press_from_active_forces_off);
    RUN_TEST(S26c_AC1_short_press_from_dim_wakes);
    RUN_TEST(S26c_AC1_short_press_from_off_wakes);
    RUN_TEST(S26c_AC1_short_press_noop_while_keep_awake);

    RUN_TEST(S26c_AC2_brightness_active_returns_stored_pct_unchanged);
    RUN_TEST(S26c_AC2_brightness_dim_returns_core_brightness_min);
    RUN_TEST(S26c_AC2_brightness_off_returns_true_zero);
    RUN_TEST(S26c_AC2_brightness_round_trip_active_dim_active);

    RUN_TEST(S26f_AC1_stays_off_below_sleep_threshold);
    RUN_TEST(S26f_AC1_sleeps_exactly_at_sleep_threshold);
    RUN_TEST(S26f_AC1_sleep_reached_directly_without_visiting_off_first);
    RUN_TEST(S26f_AC1_thresholds_pinned_to_spec_literals);
    RUN_TEST(S26f_AC1_input_wakes_from_sleep_to_active);
    RUN_TEST(S26f_AC1_short_press_from_sleep_wakes);
    RUN_TEST(S26f_AC2_no_sleep_while_keep_awake_holds);
    RUN_TEST(S26f_AC2_keep_awake_reactivates_from_sleep);
    RUN_TEST(S26f_AC1_brightness_sleep_returns_true_zero);

    RUN_TEST(S26f_amendment_sleep_inhibit_holds_off_well_past_sleep_threshold);
    RUN_TEST(S26f_amendment_sleep_inhibit_release_after_elapsed_sleeps_next_tick);
    RUN_TEST(S26f_amendment_sleep_inhibit_release_before_elapsed_sleeps_after_remaining_time);
    RUN_TEST(S26f_amendment_sleep_inhibit_does_not_block_dim_or_off);
    RUN_TEST(S26f_amendment_keep_awake_dominates_regardless_of_sleep_inhibit);

    RUN_TEST(S26_wakeonly_AC_press_during_dim_wakes_and_swallows_until_release);
    RUN_TEST(S26_wakeonly_AC_press_during_off_wakes_and_swallows_until_release);
    RUN_TEST(S26_wakeonly_AC_press_during_sleep_wakes_and_swallows_until_release);
    RUN_TEST(S26_wakeonly_AC_press_during_active_delivers_from_first_sample);
    RUN_TEST(S26_wakeonly_AC_press_started_active_stays_delivered_through_dim_transition);
    RUN_TEST(S26_wakeonly_AC_wake_pins_ref_ms_to_the_literal_press_instant);
    RUN_TEST(S26_wakeonly_AC_null_safe_fails_open);

    RUN_TEST(S26c_AC1_null_safe);

    return UNITY_END();
}
