/**
 * test_flare.c — S10 (slice a) flare state machine acceptance criteria.
 *
 * Test names follow docs/specs/S10-flare.md's numbered acceptance criteria:
 * S10_ACn_description. AC5 (goldens/UI) is out of scope for this slice —
 * see the PR body. `S10_ruling{1,2,3}_*` tests cover the three product
 * rulings recorded in the spec's `## Amendments` (2026-08-23, PR #15
 * review): Ruling 1 (independent send/receive), Ruling 2 (an established
 * lock is never silently replaced), Ruling 3 (dismiss/release split into
 * two intent-aware functions instead of one mode-dependent
 * ff_flare_dismiss(), taken as a same-slice fix on the approved PR).
 *
 * All `now_ms` values are plain literals (no fake-clock harness needed):
 * ff_flare's entry points take `now_ms` explicitly rather than reading an
 * injected clock (see ff_flare.h's top comment for why).
 *
 * Per the wave-lessons note in the task brief: every ignore/guard path
 * gets its own direct test (not just exercised incidentally), and two
 * mutations were spot-checked by hand before pushing — deleting the
 * `paired` guard in `ff_flare_on_flare_rx` fails
 * S10_AC1_receive_from_unpaired_sender_is_ignored, and deleting the
 * node_id match in `ff_flare_on_flare_end_rx` fails
 * S10_AC1_flare_end_from_different_node_is_ignored.
 */
#include <string.h>

#include "unity.h"

#include "ff_flare.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* AC1 — state table: 12 transition tests                              */
/* ------------------------------------------------------------------- */

static void S10_AC1_send_begin_records_expiry_default_300s(void)
{
    ff_flare_t f;
    ff_flare_init(&f);

    ff_flare_result_t r = ff_flare_send_begin(&f, 0u, 1000u);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_SEND_FLARE, r.intent);
    TEST_ASSERT_EQUAL_UINT16(300u, r.dur_s);
    TEST_ASSERT_FALSE(r.should_alert);
    TEST_ASSERT_TRUE(f.sending);
    TEST_ASSERT_EQUAL_UINT32(1000u + 300000u, f.send_expiry_ms);
}

static void S10_AC1_send_begin_records_expiry_explicit_duration(void)
{
    ff_flare_t f;
    ff_flare_init(&f);

    ff_flare_result_t r = ff_flare_send_begin(&f, 60u, 5000u);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_SEND_FLARE, r.intent);
    TEST_ASSERT_EQUAL_UINT16(60u, r.dur_s);
    TEST_ASSERT_TRUE(f.sending);
    TEST_ASSERT_EQUAL_UINT32(5000u + 60000u, f.send_expiry_ms);
}

static void S10_AC1_send_auto_end_exactly_at_expiry(void)
{
    /* Boundary is INCLUSIVE (ff_flare.h judgment call 1): now_ms ==
     * expiry_ms already auto-ends; one tick earlier it must not. */
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_send_begin(&f, 10u, 1000u); /* send_expiry_ms = 11000 */

    ff_flare_result_t before = ff_flare_tick(&f, 10999u);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, before.intent);
    TEST_ASSERT_TRUE(f.sending);

    ff_flare_result_t at = ff_flare_tick(&f, 11000u);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_SEND_FLARE_END, at.intent);
    TEST_ASSERT_FALSE(f.sending);
    TEST_ASSERT_EQUAL_UINT32(0u, f.send_expiry_ms);
}

static void S10_AC1_send_cancel_emits_flare_end_once_not_twice(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_send_begin(&f, 300u, 0u);

    ff_flare_result_t first = ff_flare_send_cancel(&f);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_SEND_FLARE_END, first.intent);
    TEST_ASSERT_FALSE(f.sending);

    ff_flare_result_t second = ff_flare_send_cancel(&f);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, second.intent);
    TEST_ASSERT_FALSE(f.sending);
}

static void S10_AC1_receive_from_paired_sender_enters_received(void)
{
    ff_flare_t f;
    ff_flare_init(&f);

    ff_flare_result_t r = ff_flare_on_flare_rx(&f, 42u, true, 120u, 5000u);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_TRUE(r.should_alert);
    TEST_ASSERT_TRUE(f.takeover_active);
    TEST_ASSERT_EQUAL_UINT32(42u, f.takeover_node_id);
    TEST_ASSERT_EQUAL_UINT32(5000u + 120000u, f.takeover_expiry_ms);
    TEST_ASSERT_EQUAL_UINT32(0u, f.locked_node_id); /* not yet GO'd */
}

static void S10_AC1_receive_from_unpaired_sender_is_ignored(void)
{
    /* Guard path, spot-checked directly: start from an *existing* pending
     * takeover so an accidental overwrite (not just "stays idle") would be
     * caught. */
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 42u, true, 120u, 5000u);

    ff_flare_result_t r = ff_flare_on_flare_rx(&f, 99u, false, 60u, 6000u);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_FALSE(r.should_alert);
    TEST_ASSERT_TRUE(f.takeover_active);
    TEST_ASSERT_EQUAL_UINT32(42u, f.takeover_node_id); /* unchanged, not stomped by node 99 */
    TEST_ASSERT_EQUAL_UINT32(5000u + 120000u, f.takeover_expiry_ms);
}

static void S10_AC1_flare_end_from_active_sender_returns_to_idle(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 42u, true, 120u, 5000u);

    ff_flare_result_t r = ff_flare_on_flare_end_rx(&f, 42u);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_FALSE(f.takeover_active);
    TEST_ASSERT_EQUAL_UINT32(0u, f.takeover_node_id);
    TEST_ASSERT_EQUAL_UINT32(0u, f.takeover_expiry_ms);
    TEST_ASSERT_EQUAL_UINT32(0u, f.locked_node_id);
}

static void S10_AC1_flare_end_from_different_node_is_ignored(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 42u, true, 120u, 5000u);

    ff_flare_result_t r = ff_flare_on_flare_end_rx(&f, 99u);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_TRUE(f.takeover_active); /* unaffected */
    TEST_ASSERT_EQUAL_UINT32(42u, f.takeover_node_id);
    TEST_ASSERT_EQUAL_UINT32(5000u + 120000u, f.takeover_expiry_ms);
}

static void S10_AC1_newest_flare_wins_while_received(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 42u, true, 120u, 5000u);

    ff_flare_result_t r = ff_flare_on_flare_rx(&f, 77u, true, 30u, 6000u);

    TEST_ASSERT_TRUE(r.should_alert);
    TEST_ASSERT_TRUE(f.takeover_active);
    TEST_ASSERT_EQUAL_UINT32(77u, f.takeover_node_id);
    TEST_ASSERT_EQUAL_UINT32(6000u + 30000u, f.takeover_expiry_ms);
}

static void S10_AC1_newest_flare_while_locked_raises_takeover(void)
{
    /* "Newest wins the takeover" still applies to the takeover slot while
     * LOCKED — full lock-preservation semantics (MEDIUM finding /
     * Amendment 2026-08-23 Ruling 2) are the dedicated S10_ruling2_* tests
     * below; this one just confirms AC1's "newest wins" still fires. */
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 42u, true, 120u, 5000u);
    ff_flare_go(&f);
    TEST_ASSERT_EQUAL_UINT32(42u, f.locked_node_id);

    ff_flare_result_t r = ff_flare_on_flare_rx(&f, 77u, true, 30u, 6000u);

    TEST_ASSERT_TRUE(r.should_alert);
    TEST_ASSERT_TRUE(f.takeover_active);
    TEST_ASSERT_EQUAL_UINT32(77u, f.takeover_node_id);
    TEST_ASSERT_EQUAL_UINT32(6000u + 30000u, f.takeover_expiry_ms);
}

static void S10_AC1_go_transitions_received_to_locked(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 42u, true, 120u, 5000u);

    ff_flare_result_t r = ff_flare_go(&f);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_FALSE(f.takeover_active);
    TEST_ASSERT_EQUAL_UINT32(42u, f.locked_node_id);
    TEST_ASSERT_EQUAL_UINT32(5000u + 120000u, f.locked_expiry_ms); /* carried over unchanged */
    TEST_ASSERT_EQUAL_UINT32(42u, ff_flare_locked_node(&f));
}

static void S10_AC1_dismiss_takeover_transitions_to_idle(void)
{
    /* The plain "DISMISS -> IDLE" case (spec's original language): a
     * pending takeover with no lock behind it, explicitly dismissed
     * without ever pressing GO. */
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 42u, true, 120u, 5000u);
    TEST_ASSERT_TRUE(f.takeover_active);

    ff_flare_result_t r = ff_flare_dismiss_takeover(&f);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_FALSE(f.takeover_active);
    TEST_ASSERT_EQUAL_UINT32(0u, f.takeover_node_id);
    TEST_ASSERT_EQUAL_UINT32(0u, f.takeover_expiry_ms);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_flare_locked_node(&f)); /* never was locked */
}

static void S10_AC1_release_lock_transitions_to_idle(void)
{
    /* The LOCKED "DISMISS -> IDLE" case (spec's original language),
     * post-split: releasing a lock with nothing new pending on top of it. */
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 42u, true, 120u, 5000u);
    ff_flare_go(&f);
    TEST_ASSERT_EQUAL_UINT32(42u, f.locked_node_id);
    TEST_ASSERT_FALSE(f.takeover_active); /* nothing pending on top of the lock */

    ff_flare_result_t r = ff_flare_release_lock(&f);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_FALSE(f.takeover_active);
    TEST_ASSERT_EQUAL_UINT32(0u, f.locked_node_id);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_flare_locked_node(&f));
}

/* ------------------------------------------------------------------- */
/* AC2 — receive during quiet hours still fires the haptic flag         */
/*                                                                       */
/* PR #15 review (LOW finding): a mock "quiet hours" flag that ff_flare */
/* never consults doesn't actually verify an override, since there is  */
/* no quiet-hours branch at this layer to prove is bypassed. Fixed per */
/* the review's suggestion: assert the unconditional-by-design         */
/* guarantee directly, across every surrounding state this module has  */
/* (nothing here could ever gate should_alert on anything, because     */
/* there's nothing to gate it with — no ff_settings dependency exists). */
/* True quiet-hours-consultation testing belongs at the app layer once  */
/* ff_settings is wired in (out of scope for this module by design).   */
/* ------------------------------------------------------------------- */

static void S10_AC2_receive_haptic_override_is_unconditional_by_design(void)
{
    /* Plain receive from a resting state. */
    ff_flare_t f;
    ff_flare_init(&f);
    TEST_ASSERT_TRUE(ff_flare_on_flare_rx(&f, 1u, true, 300u, 0u).should_alert);

    /* Receive while already sending my own flare. */
    ff_flare_init(&f);
    ff_flare_send_begin(&f, 300u, 0u);
    TEST_ASSERT_TRUE(ff_flare_on_flare_rx(&f, 2u, true, 300u, 0u).should_alert);

    /* Receive while already locked to a different node. */
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 3u, true, 300u, 0u);
    ff_flare_go(&f);
    TEST_ASSERT_TRUE(ff_flare_on_flare_rx(&f, 4u, true, 300u, 0u).should_alert);

    /* Negative control: an unpaired sender never alerts, in any state —
     * the flag is unconditional for paired receives specifically, not
     * simply always-true. */
    ff_flare_init(&f);
    TEST_ASSERT_FALSE(ff_flare_on_flare_rx(&f, 5u, false, 300u, 0u).should_alert);
}

/* ------------------------------------------------------------------- */
/* AC3 — GO locks selection; expiry-while-LOCKED unlocks                */
/* ------------------------------------------------------------------- */

static void S10_AC3_locked_node_zero_when_not_locked(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_flare_locked_node(&f)); /* idle */

    ff_flare_send_begin(&f, 10u, 0u);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_flare_locked_node(&f)); /* sending */

    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 5u, true, 10u, 0u);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_flare_locked_node(&f)); /* takeover pending, not yet GO'd */
}

static void S10_AC3_expiry_while_locked_unlocks_to_idle(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 42u, true, 10u, 1000u); /* expiry = 11000 */
    ff_flare_go(&f);
    TEST_ASSERT_EQUAL_UINT32(42u, ff_flare_locked_node(&f));

    ff_flare_result_t before = ff_flare_tick(&f, 10999u);
    TEST_ASSERT_EQUAL_UINT32(42u, ff_flare_locked_node(&f));
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, before.intent);

    ff_flare_result_t at = ff_flare_tick(&f, 11000u);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_flare_locked_node(&f)); /* restores cycling per AC3 */
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, at.intent); /* nothing to announce on receive side */
}

/* ------------------------------------------------------------------- */
/* AC4 — sender auto-end at dur emits FLARE_END exactly once            */
/* ------------------------------------------------------------------- */

static void S10_AC4_sender_auto_end_emits_flare_end_exactly_once(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_send_begin(&f, 5u, 0u); /* send_expiry_ms = 5000 */

    ff_flare_result_t r1 = ff_flare_tick(&f, 4999u);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r1.intent);

    ff_flare_result_t r2 = ff_flare_tick(&f, 5000u);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_SEND_FLARE_END, r2.intent);
    TEST_ASSERT_FALSE(f.sending);

    /* Repeated ticks after auto-end must not re-emit. */
    ff_flare_result_t r3 = ff_flare_tick(&f, 5000u);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r3.intent);
    ff_flare_result_t r4 = ff_flare_tick(&f, 999999u);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r4.intent);
}

/* ------------------------------------------------------------------- */
/* Ruling 1 (PR #15 review HIGH finding; Amendment 2026-08-23) —
 * outbound send and inbound receive are fully independent.            */
/* ------------------------------------------------------------------- */

static void S10_ruling1_receive_while_sending_both_states_live_cancel_still_emits_once(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_send_begin(&f, 300u, 0u); /* I am flaring */
    TEST_ASSERT_TRUE(f.sending);

    /* Kev flares while I'm still sending. */
    ff_flare_result_t rx = ff_flare_on_flare_rx(&f, 9u, true, 30u, 1000u);
    TEST_ASSERT_TRUE(rx.should_alert);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, rx.intent); /* my send is not touched/cancelled by this */

    /* Both states are live simultaneously. */
    TEST_ASSERT_TRUE(f.sending);
    TEST_ASSERT_EQUAL_UINT32(300000u, f.send_expiry_ms);
    TEST_ASSERT_TRUE(f.takeover_active);
    TEST_ASSERT_EQUAL_UINT32(9u, f.takeover_node_id);
    TEST_ASSERT_EQUAL_UINT32(1000u + 30000u, f.takeover_expiry_ms);

    /* I can still cancel my own flare early, exactly once. */
    ff_flare_result_t cancel1 = ff_flare_send_cancel(&f);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_SEND_FLARE_END, cancel1.intent);
    TEST_ASSERT_FALSE(f.sending);
    TEST_ASSERT_TRUE(f.takeover_active); /* Kev's takeover is untouched by my cancel */

    ff_flare_result_t cancel2 = ff_flare_send_cancel(&f);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, cancel2.intent); /* not twice */
}

static void S10_ruling1_send_and_receive_expire_independently(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_send_begin(&f, 5u, 0u);              /* send_expiry_ms = 5000 */
    ff_flare_on_flare_rx(&f, 9u, true, 20u, 1000u); /* takeover_expiry_ms = 21000 */

    /* Send expires first; takeover must still be live. */
    ff_flare_result_t at_send_expiry = ff_flare_tick(&f, 5000u);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_SEND_FLARE_END, at_send_expiry.intent);
    TEST_ASSERT_FALSE(f.sending);
    TEST_ASSERT_TRUE(f.takeover_active);
    TEST_ASSERT_EQUAL_UINT32(9u, f.takeover_node_id);

    /* Takeover expires later, independently; nothing send-related left to
     * re-fire. */
    ff_flare_result_t at_takeover_expiry = ff_flare_tick(&f, 21000u);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, at_takeover_expiry.intent);
    TEST_ASSERT_FALSE(f.takeover_active);
    TEST_ASSERT_EQUAL_UINT32(0u, f.takeover_node_id);
    TEST_ASSERT_FALSE(f.sending); /* still ended, not resurrected */
}

/* ------------------------------------------------------------------- */
/* Ruling 2 (PR #15 review MEDIUM finding; Amendment 2026-08-23) —
 * an established LOCK is never silently replaced by a new takeover.    */
/* ------------------------------------------------------------------- */

static void S10_ruling2_flare_from_b_while_locked_to_a_shows_takeover_keeps_lock(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 0xA1u, true, 300u, 0u); /* Dana flares */
    ff_flare_go(&f);                                  /* I GO -> locked to Dana */
    TEST_ASSERT_EQUAL_UINT32(0xA1u, ff_flare_locked_node(&f));

    ff_flare_result_t rx = ff_flare_on_flare_rx(&f, 0xB2u, true, 60u, 10000u); /* Kev flares too */

    TEST_ASSERT_TRUE(rx.should_alert);
    TEST_ASSERT_TRUE(f.takeover_active); /* Kev's takeover IS shown */
    TEST_ASSERT_EQUAL_UINT32(0xB2u, f.takeover_node_id);
    TEST_ASSERT_EQUAL_UINT32(10000u + 60000u, f.takeover_expiry_ms);
    TEST_ASSERT_EQUAL_UINT32(0xA1u, ff_flare_locked_node(&f)); /* lock on Dana is untouched */
}

static void S10_ruling2_dismiss_takeover_on_new_takeover_returns_to_intact_lock(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 0xA1u, true, 300u, 0u);
    ff_flare_go(&f); /* locked to A */
    ff_flare_on_flare_rx(&f, 0xB2u, true, 60u, 10000u); /* B's takeover raised */
    TEST_ASSERT_EQUAL_UINT32(0xA1u, ff_flare_locked_node(&f));

    ff_flare_result_t r = ff_flare_dismiss_takeover(&f);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_FALSE(f.takeover_active); /* B's takeover cleared */
    TEST_ASSERT_EQUAL_UINT32(0xA1u, ff_flare_locked_node(&f)); /* still locked to A, intact */
}

static void S10_ruling2_go_on_new_takeover_switches_lock_to_b(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 0xA1u, true, 300u, 0u);
    ff_flare_go(&f); /* locked to A */
    ff_flare_on_flare_rx(&f, 0xB2u, true, 60u, 10000u); /* B's takeover raised */
    TEST_ASSERT_EQUAL_UINT32(0xA1u, ff_flare_locked_node(&f));

    ff_flare_result_t r = ff_flare_go(&f);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_FALSE(f.takeover_active);
    TEST_ASSERT_EQUAL_UINT32(0xB2u, ff_flare_locked_node(&f)); /* explicit switch to B */
    TEST_ASSERT_EQUAL_UINT32(10000u + 60000u, f.locked_expiry_ms);
}

/* ------------------------------------------------------------------- */
/* Ruling 3 (PR #15 review, approved with recommendation taken
 * immediately; Amendment 2026-08-23, third entry) — dismiss/release
 * split into two intent-aware functions instead of one mode-dependent
 * ff_flare_dismiss().                                                  */
/* ------------------------------------------------------------------- */

static void S10_ruling3_release_lock_while_takeover_pending_keeps_takeover_visible(void)
{
    /* The reviewer's concrete repro, directly: user taps "stop
     * navigating" (intending ff_flare_release_lock) at the exact instant
     * a new flare's takeover is pending. With the old double-duty
     * dismiss(), this would have silently taken the takeover-dismiss
     * branch instead — dropping the user's actual intent AND swallowing
     * the new takeover unseen. release_lock() only ever touches the
     * lock: the takeover must come through completely untouched. */
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 0xA1u, true, 300u, 0u);
    ff_flare_go(&f); /* locked to A */
    ff_flare_on_flare_rx(&f, 0xB2u, true, 60u, 10000u); /* Kev's takeover arrives */
    TEST_ASSERT_EQUAL_UINT32(0xA1u, ff_flare_locked_node(&f));
    TEST_ASSERT_TRUE(f.takeover_active);

    ff_flare_result_t r = ff_flare_release_lock(&f);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_flare_locked_node(&f)); /* lock released, as intended */
    TEST_ASSERT_TRUE(f.takeover_active);                    /* Kev's takeover: still pending, still visible */
    TEST_ASSERT_EQUAL_UINT32(0xB2u, f.takeover_node_id);
    TEST_ASSERT_EQUAL_UINT32(10000u + 60000u, f.takeover_expiry_ms);
}

static void S10_ruling3_dismiss_takeover_while_locked_with_nothing_pending_is_noop(void)
{
    /* Mirror of the race case: dismissing a takeover that doesn't exist
     * must not accidentally release an unrelated lock. */
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 0xA1u, true, 300u, 0u);
    ff_flare_go(&f); /* locked to A, nothing pending */
    TEST_ASSERT_FALSE(f.takeover_active);

    ff_flare_result_t r = ff_flare_dismiss_takeover(&f);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_EQUAL_UINT32(0xA1u, ff_flare_locked_node(&f)); /* untouched */
}

static void S10_ruling3_dismiss_takeover_while_idle_is_noop(void)
{
    ff_flare_t f;
    ff_flare_init(&f);

    ff_flare_result_t r = ff_flare_dismiss_takeover(&f);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_FALSE(f.takeover_active);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_flare_locked_node(&f));
}

static void S10_ruling3_release_lock_while_idle_is_noop(void)
{
    ff_flare_t f;
    ff_flare_init(&f);

    ff_flare_result_t r = ff_flare_release_lock(&f);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_flare_locked_node(&f));
}

/* ------------------------------------------------------------------- */
/* Extra guard-path coverage (beyond the spec's 12-test minimum, per the
 * wave-lessons note: every ignore/guard path gets its own direct test) */
/* ------------------------------------------------------------------- */

static void S10_send_cancel_while_idle_is_noop(void)
{
    ff_flare_t f;
    ff_flare_init(&f);

    ff_flare_result_t r = ff_flare_send_cancel(&f);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_FALSE(f.sending);
}

static void S10_go_while_idle_is_noop(void)
{
    ff_flare_t f;
    ff_flare_init(&f);

    ff_flare_result_t r = ff_flare_go(&f);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_flare_locked_node(&f));
}

static void S10_flare_end_rx_while_idle_is_ignored(void)
{
    ff_flare_t f;
    ff_flare_init(&f);

    ff_flare_result_t r = ff_flare_on_flare_end_rx(&f, 42u);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_FALSE(f.takeover_active);
    TEST_ASSERT_EQUAL_UINT32(0u, f.locked_node_id);
}

static void S10_flare_end_rx_while_sending_is_ignored(void)
{
    /* A FLARE_END on the *receive* path never reaches into my own
     * SENDING state — that's what ff_flare_send_cancel/auto-end are for. */
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_send_begin(&f, 300u, 0u);

    ff_flare_result_t r = ff_flare_on_flare_end_rx(&f, 1234u);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_TRUE(f.sending);
}

static void S10_flare_end_rx_matching_both_takeover_and_lock_clears_both(void)
{
    /* Edge case: the pending takeover and the lock happen to be the same
     * node (e.g. the locked sender flares again before I've dismissed the
     * fresh takeover). A FLARE_END from that node clears both
     * independently-tracked fields, not just one. */
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 0xA1u, true, 300u, 0u);
    ff_flare_go(&f); /* locked to A */
    ff_flare_on_flare_rx(&f, 0xA1u, true, 30u, 10000u); /* A flares again -> new takeover, still locked to A */
    TEST_ASSERT_TRUE(f.takeover_active);
    TEST_ASSERT_EQUAL_UINT32(0xA1u, ff_flare_locked_node(&f));

    ff_flare_result_t r = ff_flare_on_flare_end_rx(&f, 0xA1u);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_FALSE(f.takeover_active);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_flare_locked_node(&f));
}

static void S10_wraparound_safe_expiry_check(void)
{
    /* Mirrors S02's ff_crew wraparound test: send_expiry_ms sits 100 ticks
     * before a uint32_t rollover, now_ms is 100 ticks past it. True
     * elapsed time is 200ms past expiry, which must already read as
     * expired -- not the ~4.29 billion ms a naive comparison would see. */
    ff_flare_t f;
    ff_flare_init(&f);
    f.sending = true;
    f.send_expiry_ms = UINT32_MAX - 99u;

    ff_flare_result_t r = ff_flare_tick(&f, 100u);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_SEND_FLARE_END, r.intent);
    TEST_ASSERT_FALSE(f.sending);
}

static void S10_init_zeroes_all_independent_state(void)
{
    ff_flare_t f;
    memset(&f, 0xAA, sizeof(f));

    ff_flare_init(&f);

    TEST_ASSERT_FALSE(f.sending);
    TEST_ASSERT_EQUAL_UINT32(0u, f.send_expiry_ms);
    TEST_ASSERT_FALSE(f.takeover_active);
    TEST_ASSERT_EQUAL_UINT32(0u, f.takeover_node_id);
    TEST_ASSERT_EQUAL_UINT32(0u, f.takeover_expiry_ms);
    TEST_ASSERT_EQUAL_UINT32(0u, f.locked_node_id);
    TEST_ASSERT_EQUAL_UINT32(0u, f.locked_expiry_ms);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S10_AC1_send_begin_records_expiry_default_300s);
    RUN_TEST(S10_AC1_send_begin_records_expiry_explicit_duration);
    RUN_TEST(S10_AC1_send_auto_end_exactly_at_expiry);
    RUN_TEST(S10_AC1_send_cancel_emits_flare_end_once_not_twice);
    RUN_TEST(S10_AC1_receive_from_paired_sender_enters_received);
    RUN_TEST(S10_AC1_receive_from_unpaired_sender_is_ignored);
    RUN_TEST(S10_AC1_flare_end_from_active_sender_returns_to_idle);
    RUN_TEST(S10_AC1_flare_end_from_different_node_is_ignored);
    RUN_TEST(S10_AC1_newest_flare_wins_while_received);
    RUN_TEST(S10_AC1_newest_flare_while_locked_raises_takeover);
    RUN_TEST(S10_AC1_go_transitions_received_to_locked);
    RUN_TEST(S10_AC1_dismiss_takeover_transitions_to_idle);
    RUN_TEST(S10_AC1_release_lock_transitions_to_idle);

    RUN_TEST(S10_AC2_receive_haptic_override_is_unconditional_by_design);

    RUN_TEST(S10_AC3_locked_node_zero_when_not_locked);
    RUN_TEST(S10_AC3_expiry_while_locked_unlocks_to_idle);

    RUN_TEST(S10_AC4_sender_auto_end_emits_flare_end_exactly_once);

    RUN_TEST(S10_ruling1_receive_while_sending_both_states_live_cancel_still_emits_once);
    RUN_TEST(S10_ruling1_send_and_receive_expire_independently);

    RUN_TEST(S10_ruling2_flare_from_b_while_locked_to_a_shows_takeover_keeps_lock);
    RUN_TEST(S10_ruling2_dismiss_takeover_on_new_takeover_returns_to_intact_lock);
    RUN_TEST(S10_ruling2_go_on_new_takeover_switches_lock_to_b);

    RUN_TEST(S10_ruling3_release_lock_while_takeover_pending_keeps_takeover_visible);
    RUN_TEST(S10_ruling3_dismiss_takeover_while_locked_with_nothing_pending_is_noop);
    RUN_TEST(S10_ruling3_dismiss_takeover_while_idle_is_noop);
    RUN_TEST(S10_ruling3_release_lock_while_idle_is_noop);

    RUN_TEST(S10_send_cancel_while_idle_is_noop);
    RUN_TEST(S10_go_while_idle_is_noop);
    RUN_TEST(S10_flare_end_rx_while_idle_is_ignored);
    RUN_TEST(S10_flare_end_rx_while_sending_is_ignored);
    RUN_TEST(S10_flare_end_rx_matching_both_takeover_and_lock_clears_both);
    RUN_TEST(S10_wraparound_safe_expiry_check);
    RUN_TEST(S10_init_zeroes_all_independent_state);

    return UNITY_END();
}
