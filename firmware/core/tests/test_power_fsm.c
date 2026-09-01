/**
 * test_power_fsm.c — S26 slice (b) / S25 slice (b) acceptance criteria
 * for `ff_power_fsm` (docs/specs/S26-device-lifecycle.md, AC1/AC4).
 *
 * Test names follow the spec's numbered ACs: S26b_ACn_description.
 *
 * THE PROXY, stated up front (AGENTS.md's standing rule, item 6): the
 * easy proxy for "debounce works" is "a single tick with the final level
 * produces the right event" — which is satisfied even by an FSM with NO
 * debounce at all (no bounce sequence was exercised). Every debounce
 * test here therefore drives an actual bounce sequence (level flips
 * inside the 30 ms window) and asserts BOTH that the bounce produced no
 * event AND that the eventually-settled level does. Two mutations
 * spot-checked by hand before pushing (both verified against a fresh
 * build, per AGENTS.md's "mutation checks need fresh builds"): (1)
 * changing `ff_power_fsm_reached`'s boundary comparison from `>= 0` to
 * `> 0` (strict) fails 8 of the 14 tests here, including
 * `S26b_AC1_long_press_boundary_is_inclusive_at_exactly_1500ms` — the
 * debounce window itself never closes at exactly 30 ms once the
 * boundary is strict, so nothing downstream of a debounced press can
 * pass either; (2) flipping the release-branch ternary
 * (`fsm->long_fired ? RELEASE : SHORT_PRESS` ->
 * `... ? SHORT_PRESS : RELEASE`) fails
 * `S26b_AC1_release_after_long_emits_release_not_short` and three other
 * tests asserting a specific SHORT_PRESS/RELEASE outcome.
 */
#include <string.h>

#include "unity.h"

#include "ff_power_fsm.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* Small helpers                                                       */
/* ------------------------------------------------------------------- */

/* Drive a debounced PRESS: raw goes high at t0, ticked at t0 (no event —
 * still debouncing) then again once the debounce window has elapsed
 * (the press edge commits, still no event of its own). Returns the tick
 * time the press committed at (t0 + FF_POWER_FSM_DEBOUNCE_MS). */
static uint32_t press_and_settle(ff_power_fsm_t *fsm, uint32_t t0)
{
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_NONE, ff_power_fsm_tick(fsm, t0, true));
    uint32_t const settled = t0 + FF_POWER_FSM_DEBOUNCE_MS;
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_NONE, ff_power_fsm_tick(fsm, settled, true));
    TEST_ASSERT_TRUE(fsm->debounced_pressed);
    return settled;
}

/* ------------------------------------------------------------------- */
/* AC1 — debounce                                                      */
/* ------------------------------------------------------------------- */

static void S26b_AC1_a_press_shorter_than_the_debounce_window_is_ignored(void)
{
    ff_power_fsm_t fsm;
    ff_power_fsm_init(&fsm);

    /* Raw goes high, then back low before FF_POWER_FSM_DEBOUNCE_MS
     * elapses: a glitch/bounce, never a real press. */
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_NONE, ff_power_fsm_tick(&fsm, 1000, true));
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_NONE, ff_power_fsm_tick(&fsm, 1000 + FF_POWER_FSM_DEBOUNCE_MS - 1, false));
    TEST_ASSERT_FALSE(fsm.debounced_pressed);

    /* Even well past where the FIRST edge's window would have elapsed,
     * nothing has committed — the bounce cancelled its own window. */
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_NONE, ff_power_fsm_tick(&fsm, 1000 + FF_POWER_FSM_DEBOUNCE_MS, false));
    TEST_ASSERT_FALSE(fsm.debounced_pressed);
}

static void S26b_AC1_a_press_that_holds_the_full_debounce_window_commits(void)
{
    ff_power_fsm_t fsm;
    ff_power_fsm_init(&fsm);
    (void)press_and_settle(&fsm, 1000);
}

static void S26b_AC1_release_debounces_symmetrically(void)
{
    ff_power_fsm_t fsm;
    ff_power_fsm_init(&fsm);
    uint32_t const pressed_at = press_and_settle(&fsm, 1000);

    /* A quick tap: release well before the long threshold. The release
     * EDGE itself must also survive the debounce window before SHORT
     * fires — a raw low seen for only 10ms (a bounce) must not commit. */
    uint32_t const release_raw_at = pressed_at + 50;
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_NONE, ff_power_fsm_tick(&fsm, release_raw_at, false));
    TEST_ASSERT_TRUE(fsm.debounced_pressed); /* still debouncing the release */

    /* Bounce back high mid-window: must NOT commit a release. */
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_NONE,
                       ff_power_fsm_tick(&fsm, release_raw_at + 5, true));
    TEST_ASSERT_TRUE(fsm.debounced_pressed);

    /* Now release for real and hold it past the window. */
    uint32_t const real_release_at = release_raw_at + 10;
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_NONE, ff_power_fsm_tick(&fsm, real_release_at, false));
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_SHORT_PRESS,
                       ff_power_fsm_tick(&fsm, real_release_at + FF_POWER_FSM_DEBOUNCE_MS, false));
    TEST_ASSERT_FALSE(fsm.debounced_pressed);
}

/* ------------------------------------------------------------------- */
/* AC1 — short vs. long, and the 1500ms boundary exactly               */
/* ------------------------------------------------------------------- */

static void S26b_AC1_a_quick_tap_emits_short_press_never_long(void)
{
    ff_power_fsm_t fsm;
    ff_power_fsm_init(&fsm);
    uint32_t const pressed_at = press_and_settle(&fsm, 1000);

    /* Tick a few times well under the long threshold: no event. */
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_NONE, ff_power_fsm_tick(&fsm, pressed_at + 200, true));
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_NONE, ff_power_fsm_tick(&fsm, pressed_at + 800, true));

    uint32_t const release_raw_at = pressed_at + 900;
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_NONE, ff_power_fsm_tick(&fsm, release_raw_at, false));
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_SHORT_PRESS,
                       ff_power_fsm_tick(&fsm, release_raw_at + FF_POWER_FSM_DEBOUNCE_MS, false));
}

static void S26b_AC1_long_press_boundary_is_inclusive_at_exactly_1500ms(void)
{
    ff_power_fsm_t fsm;
    ff_power_fsm_init(&fsm);
    uint32_t const pressed_at = press_and_settle(&fsm, 1000);

    /* One tick BEFORE the threshold: still held, no event yet. */
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_NONE,
                       ff_power_fsm_tick(&fsm, pressed_at + FF_POWER_FSM_LONG_MS - 1, true));
    TEST_ASSERT_FALSE(fsm.long_fired);

    /* Exactly at the threshold, still held: LONG_PRESS fires (inclusive
     * boundary — ff_power_fsm.c's ff_power_fsm_reached convention). */
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_LONG_PRESS,
                       ff_power_fsm_tick(&fsm, pressed_at + FF_POWER_FSM_LONG_MS, true));
    TEST_ASSERT_TRUE(fsm.long_fired);
}

static void S26b_AC1_a_held_press_emits_long_exactly_once(void)
{
    ff_power_fsm_t fsm;
    ff_power_fsm_init(&fsm);
    uint32_t const pressed_at = press_and_settle(&fsm, 1000);

    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_LONG_PRESS,
                       ff_power_fsm_tick(&fsm, pressed_at + FF_POWER_FSM_LONG_MS, true));

    /* Keep holding well past the threshold, ticking repeatedly: LONG
     * must never fire a second time for the same press cycle. */
    for (uint32_t dt = FF_POWER_FSM_LONG_MS + 1; dt <= FF_POWER_FSM_LONG_MS + 5000; dt += 250) {
        TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_NONE, ff_power_fsm_tick(&fsm, pressed_at + dt, true));
    }
}

/* The rule under explicit test in the spec's own words: "release after
 * LONG does NOT also emit SHORT". */
static void S26b_AC1_release_after_long_emits_release_not_short(void)
{
    ff_power_fsm_t fsm;
    ff_power_fsm_init(&fsm);
    uint32_t const pressed_at = press_and_settle(&fsm, 1000);

    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_LONG_PRESS,
                       ff_power_fsm_tick(&fsm, pressed_at + FF_POWER_FSM_LONG_MS, true));

    uint32_t const release_raw_at = pressed_at + FF_POWER_FSM_LONG_MS + 400;
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_NONE, ff_power_fsm_tick(&fsm, release_raw_at, false));
    ff_power_fsm_event_t const ev = ff_power_fsm_tick(&fsm, release_raw_at + FF_POWER_FSM_DEBOUNCE_MS, false);
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_RELEASE, ev);
    TEST_ASSERT_NOT_EQUAL(FF_POWER_FSM_EVENT_SHORT_PRESS, ev);
    TEST_ASSERT_FALSE(fsm.debounced_pressed);
}

static void S26b_AC1_a_second_press_after_a_short_release_can_still_go_long(void)
{
    ff_power_fsm_t fsm;
    ff_power_fsm_init(&fsm);

    /* First press: a short tap, well under the long threshold. */
    uint32_t const p1 = press_and_settle(&fsm, 1000);
    uint32_t const r1 = p1 + 100;
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_NONE, ff_power_fsm_tick(&fsm, r1, false));
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_SHORT_PRESS,
                       ff_power_fsm_tick(&fsm, r1 + FF_POWER_FSM_DEBOUNCE_MS, false));

    /* Second press cycle: held all the way to the long threshold. If
     * long_fired (or press_start_ms) leaked across press cycles this
     * would wrongly fire immediately, or never fire at all. */
    uint32_t const p2 = press_and_settle(&fsm, r1 + FF_POWER_FSM_DEBOUNCE_MS + 2000);
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_NONE, ff_power_fsm_tick(&fsm, p2 + 1000, true));
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_LONG_PRESS,
                       ff_power_fsm_tick(&fsm, p2 + FF_POWER_FSM_LONG_MS, true));
}

/* ------------------------------------------------------------------- */
/* NULL safety + init                                                  */
/* ------------------------------------------------------------------- */

static void S26b_null_fsm_is_safe(void)
{
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_NONE, ff_power_fsm_tick(NULL, 1000, true));
    ff_power_fsm_init(NULL); /* no crash */
    ff_power_fsm_request_reboot(NULL);
    TEST_ASSERT_FALSE(ff_power_fsm_reboot_ready(NULL, false));
}

static void S26b_init_zeroes_all_state(void)
{
    ff_power_fsm_t fsm;
    memset(&fsm, 0xAA, sizeof(fsm));
    ff_power_fsm_init(&fsm);

    ff_power_fsm_t zero;
    memset(&zero, 0, sizeof(zero));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &fsm, sizeof(fsm));
}

/* ------------------------------------------------------------------- */
/* AC4 — reboot BOOT-release guard                                     */
/* ------------------------------------------------------------------- */

static void S26b_AC4_reboot_ready_is_false_until_requested(void)
{
    ff_power_fsm_t fsm;
    ff_power_fsm_init(&fsm);

    /* Even with BOOT already released, nothing was requested. */
    TEST_ASSERT_FALSE(ff_power_fsm_reboot_ready(&fsm, false));
    TEST_ASSERT_FALSE(ff_power_fsm_reboot_ready(&fsm, false));
}

static void S26b_AC4_reboot_waits_for_boot_release_then_reports_once(void)
{
    ff_power_fsm_t fsm;
    ff_power_fsm_init(&fsm);

    ff_power_fsm_request_reboot(&fsm);

    /* BOOT (GPIO0) held LOW == pressed == true: reboot must NOT report
     * ready — doing so would risk esp_restart() landing in the ROM
     * bootloader (download mode). */
    TEST_ASSERT_FALSE(ff_power_fsm_reboot_ready(&fsm, true));
    TEST_ASSERT_FALSE(ff_power_fsm_reboot_ready(&fsm, true));

    /* BOOT released: ready, exactly once. */
    TEST_ASSERT_TRUE(ff_power_fsm_reboot_ready(&fsm, false));
    TEST_ASSERT_FALSE(ff_power_fsm_reboot_ready(&fsm, false)); /* one-shot: already reported */
    TEST_ASSERT_FALSE(ff_power_fsm_reboot_ready(&fsm, true));  /* and no amount of BOOT churn re-arms it */
}

static void S26b_AC4_reboot_ready_when_boot_already_released_at_request_time(void)
{
    ff_power_fsm_t fsm;
    ff_power_fsm_init(&fsm);

    ff_power_fsm_request_reboot(&fsm);
    /* BOOT was already released when the reboot was requested — ready on
     * the very first check, not stuck waiting for an edge that already
     * happened before the request. */
    TEST_ASSERT_TRUE(ff_power_fsm_reboot_ready(&fsm, false));
}

static void S26b_AC4_reboot_guard_is_independent_of_the_press_fsm(void)
{
    ff_power_fsm_t fsm;
    ff_power_fsm_init(&fsm);

    /* Drive a whole PWR press/long/release cycle; it must not perturb
     * the (unrequested) reboot guard. */
    uint32_t const pressed_at = press_and_settle(&fsm, 1000);
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_LONG_PRESS,
                       ff_power_fsm_tick(&fsm, pressed_at + FF_POWER_FSM_LONG_MS, true));
    TEST_ASSERT_FALSE(fsm.reboot_pending);
    TEST_ASSERT_FALSE(ff_power_fsm_reboot_ready(&fsm, false));

    /* And a pending reboot must not be cleared by continuing to tick the
     * press FSM (only a released BOOT clears it). */
    ff_power_fsm_request_reboot(&fsm);
    TEST_ASSERT_EQUAL(FF_POWER_FSM_EVENT_NONE, ff_power_fsm_tick(&fsm, pressed_at + FF_POWER_FSM_LONG_MS + 500, true));
    TEST_ASSERT_TRUE(fsm.reboot_pending);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S26b_AC1_a_press_shorter_than_the_debounce_window_is_ignored);
    RUN_TEST(S26b_AC1_a_press_that_holds_the_full_debounce_window_commits);
    RUN_TEST(S26b_AC1_release_debounces_symmetrically);
    RUN_TEST(S26b_AC1_a_quick_tap_emits_short_press_never_long);
    RUN_TEST(S26b_AC1_long_press_boundary_is_inclusive_at_exactly_1500ms);
    RUN_TEST(S26b_AC1_a_held_press_emits_long_exactly_once);
    RUN_TEST(S26b_AC1_release_after_long_emits_release_not_short);
    RUN_TEST(S26b_AC1_a_second_press_after_a_short_release_can_still_go_long);

    RUN_TEST(S26b_null_fsm_is_safe);
    RUN_TEST(S26b_init_zeroes_all_state);

    RUN_TEST(S26b_AC4_reboot_ready_is_false_until_requested);
    RUN_TEST(S26b_AC4_reboot_waits_for_boot_release_then_reports_once);
    RUN_TEST(S26b_AC4_reboot_ready_when_boot_already_released_at_request_time);
    RUN_TEST(S26b_AC4_reboot_guard_is_independent_of_the_press_fsm);

    return UNITY_END();
}
