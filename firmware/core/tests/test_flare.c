/**
 * test_flare.c — S10 (slice a) flare state machine acceptance criteria.
 *
 * Test names follow docs/specs/S10-flare.md's numbered acceptance criteria:
 * S10_ACn_description. AC5 (goldens/UI) is out of scope for this slice —
 * see the PR body.
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
    TEST_ASSERT_EQUAL(FF_FLARE_SENDING, f.state);
    TEST_ASSERT_EQUAL_UINT32(1000u + 300000u, f.expiry_ms);
}

static void S10_AC1_send_begin_records_expiry_explicit_duration(void)
{
    ff_flare_t f;
    ff_flare_init(&f);

    ff_flare_result_t r = ff_flare_send_begin(&f, 60u, 5000u);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_SEND_FLARE, r.intent);
    TEST_ASSERT_EQUAL_UINT16(60u, r.dur_s);
    TEST_ASSERT_EQUAL(FF_FLARE_SENDING, f.state);
    TEST_ASSERT_EQUAL_UINT32(5000u + 60000u, f.expiry_ms);
}

static void S10_AC1_send_auto_end_exactly_at_expiry(void)
{
    /* Boundary is INCLUSIVE (ff_flare.h judgment call 1): now_ms ==
     * expiry_ms already auto-ends; one tick earlier it must not. */
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_send_begin(&f, 10u, 1000u); /* expiry = 11000 */

    ff_flare_result_t before = ff_flare_tick(&f, 10999u);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, before.intent);
    TEST_ASSERT_EQUAL(FF_FLARE_SENDING, f.state);

    ff_flare_result_t at = ff_flare_tick(&f, 11000u);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_SEND_FLARE_END, at.intent);
    TEST_ASSERT_EQUAL(FF_FLARE_IDLE, f.state);
    TEST_ASSERT_EQUAL_UINT32(0u, f.expiry_ms);
}

static void S10_AC1_send_cancel_emits_flare_end_once_not_twice(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_send_begin(&f, 300u, 0u);

    ff_flare_result_t first = ff_flare_send_cancel(&f);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_SEND_FLARE_END, first.intent);
    TEST_ASSERT_EQUAL(FF_FLARE_IDLE, f.state);

    ff_flare_result_t second = ff_flare_send_cancel(&f);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, second.intent);
    TEST_ASSERT_EQUAL(FF_FLARE_IDLE, f.state);
}

static void S10_AC1_receive_from_paired_sender_enters_received(void)
{
    ff_flare_t f;
    ff_flare_init(&f);

    ff_flare_result_t r = ff_flare_on_flare_rx(&f, 42u, true, 120u, 5000u);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_TRUE(r.should_alert);
    TEST_ASSERT_EQUAL(FF_FLARE_RECEIVED, f.state);
    TEST_ASSERT_EQUAL_UINT32(42u, f.node_id);
    TEST_ASSERT_EQUAL_UINT32(5000u + 120000u, f.expiry_ms);
}

static void S10_AC1_receive_from_unpaired_sender_is_ignored(void)
{
    /* Guard path, spot-checked directly: start from an *existing* RECEIVED
     * takeover so an accidental overwrite (not just "stays IDLE") would be
     * caught. */
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 42u, true, 120u, 5000u);

    ff_flare_result_t r = ff_flare_on_flare_rx(&f, 99u, false, 60u, 6000u);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_FALSE(r.should_alert);
    TEST_ASSERT_EQUAL(FF_FLARE_RECEIVED, f.state);
    TEST_ASSERT_EQUAL_UINT32(42u, f.node_id); /* unchanged, not stomped by node 99 */
    TEST_ASSERT_EQUAL_UINT32(5000u + 120000u, f.expiry_ms);
}

static void S10_AC1_flare_end_from_active_sender_returns_to_idle(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 42u, true, 120u, 5000u);

    ff_flare_result_t r = ff_flare_on_flare_end_rx(&f, 42u);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_EQUAL(FF_FLARE_IDLE, f.state);
    TEST_ASSERT_EQUAL_UINT32(0u, f.node_id);
    TEST_ASSERT_EQUAL_UINT32(0u, f.expiry_ms);
}

static void S10_AC1_flare_end_from_different_node_is_ignored(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 42u, true, 120u, 5000u);

    ff_flare_result_t r = ff_flare_on_flare_end_rx(&f, 99u);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_EQUAL(FF_FLARE_RECEIVED, f.state); /* unaffected */
    TEST_ASSERT_EQUAL_UINT32(42u, f.node_id);
    TEST_ASSERT_EQUAL_UINT32(5000u + 120000u, f.expiry_ms);
}

static void S10_AC1_newest_flare_wins_while_received(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 42u, true, 120u, 5000u);

    ff_flare_result_t r = ff_flare_on_flare_rx(&f, 77u, true, 30u, 6000u);

    TEST_ASSERT_TRUE(r.should_alert);
    TEST_ASSERT_EQUAL(FF_FLARE_RECEIVED, f.state);
    TEST_ASSERT_EQUAL_UINT32(77u, f.node_id);
    TEST_ASSERT_EQUAL_UINT32(6000u + 30000u, f.expiry_ms);
}

static void S10_AC1_newest_flare_wins_while_locked(void)
{
    /* judgment call 3: a fresh sender always lands unlocked, even though
     * the previous takeover had been GO'd to LOCKED. */
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 42u, true, 120u, 5000u);
    ff_flare_go(&f);
    TEST_ASSERT_EQUAL(FF_FLARE_LOCKED, f.state);

    ff_flare_result_t r = ff_flare_on_flare_rx(&f, 77u, true, 30u, 6000u);

    TEST_ASSERT_TRUE(r.should_alert);
    TEST_ASSERT_EQUAL(FF_FLARE_RECEIVED, f.state); /* not LOCKED */
    TEST_ASSERT_EQUAL_UINT32(77u, f.node_id);
    TEST_ASSERT_EQUAL_UINT32(6000u + 30000u, f.expiry_ms);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_flare_locked_node(&f));
}

static void S10_AC1_go_transitions_received_to_locked(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 42u, true, 120u, 5000u);

    ff_flare_result_t r = ff_flare_go(&f);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_EQUAL(FF_FLARE_LOCKED, f.state);
    TEST_ASSERT_EQUAL_UINT32(42u, f.node_id);
    TEST_ASSERT_EQUAL_UINT32(5000u + 120000u, f.expiry_ms); /* unchanged */
    TEST_ASSERT_EQUAL_UINT32(42u, ff_flare_locked_node(&f));
}

static void S10_AC1_dismiss_transitions_to_idle_and_unlocks(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 42u, true, 120u, 5000u);
    ff_flare_go(&f);
    TEST_ASSERT_EQUAL(FF_FLARE_LOCKED, f.state);

    ff_flare_result_t r = ff_flare_dismiss(&f);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_EQUAL(FF_FLARE_IDLE, f.state);
    TEST_ASSERT_EQUAL_UINT32(0u, f.node_id);
    TEST_ASSERT_EQUAL_UINT32(0u, f.expiry_ms);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_flare_locked_node(&f));
}

/* ------------------------------------------------------------------- */
/* AC2 — receive during quiet hours still fires the haptic flag         */
/* ------------------------------------------------------------------- */

static void S10_AC2_receive_during_quiet_hours_still_sets_should_alert(void)
{
    /* ff_flare has no ff_settings/quiet-hours dependency at all (see
     * ff_flare.h's top comment) -- this test models the caller's side of
     * the override: a mock "it is currently quiet hours" flag that the
     * app's haptic scheduler would normally gate on, proving
     * should_alert is asserted regardless and the caller must honor it
     * unconditionally rather than running it through ff_quiet_now. */
    bool mock_quiet_hours_active = true;

    ff_flare_t f;
    ff_flare_init(&f);

    ff_flare_result_t r = ff_flare_on_flare_rx(&f, 7u, true, 300u, 0u);

    TEST_ASSERT_TRUE(r.should_alert);

    /* Simulated app-layer haptic decision: the flag alone decides, quiet
     * hours never gates it. */
    bool would_buzz = r.should_alert;
    TEST_ASSERT_TRUE(would_buzz);
    TEST_ASSERT_TRUE(mock_quiet_hours_active); /* still true: not consulted */
}

/* ------------------------------------------------------------------- */
/* AC3 — GO locks selection; expiry-while-LOCKED unlocks                */
/* ------------------------------------------------------------------- */

static void S10_AC3_locked_node_zero_when_not_locked(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_flare_locked_node(&f)); /* IDLE */

    ff_flare_send_begin(&f, 10u, 0u);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_flare_locked_node(&f)); /* SENDING */

    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 5u, true, 10u, 0u);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_flare_locked_node(&f)); /* RECEIVED, not yet GO'd */
}

static void S10_AC3_expiry_while_locked_unlocks_to_idle(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 42u, true, 10u, 1000u); /* expiry = 11000 */
    ff_flare_go(&f);
    TEST_ASSERT_EQUAL_UINT32(42u, ff_flare_locked_node(&f));

    ff_flare_result_t before = ff_flare_tick(&f, 10999u);
    TEST_ASSERT_EQUAL(FF_FLARE_LOCKED, f.state);
    TEST_ASSERT_EQUAL_UINT32(42u, ff_flare_locked_node(&f));
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, before.intent);

    ff_flare_result_t at = ff_flare_tick(&f, 11000u);
    TEST_ASSERT_EQUAL(FF_FLARE_IDLE, f.state);
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
    ff_flare_send_begin(&f, 5u, 0u); /* expiry = 5000 */

    ff_flare_result_t r1 = ff_flare_tick(&f, 4999u);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r1.intent);

    ff_flare_result_t r2 = ff_flare_tick(&f, 5000u);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_SEND_FLARE_END, r2.intent);
    TEST_ASSERT_EQUAL(FF_FLARE_IDLE, f.state);

    /* Repeated ticks after auto-end must not re-emit. */
    ff_flare_result_t r3 = ff_flare_tick(&f, 5000u);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r3.intent);
    ff_flare_result_t r4 = ff_flare_tick(&f, 999999u);
    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r4.intent);
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
    TEST_ASSERT_EQUAL(FF_FLARE_IDLE, f.state);
}

static void S10_send_cancel_while_received_does_not_affect_receive_state(void)
{
    /* cancel only ever cancels *my own* SENDING; it must not reach into
     * an unrelated RECEIVED/LOCKED takeover. */
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_on_flare_rx(&f, 42u, true, 120u, 5000u);

    ff_flare_result_t r = ff_flare_send_cancel(&f);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_EQUAL(FF_FLARE_RECEIVED, f.state);
    TEST_ASSERT_EQUAL_UINT32(42u, f.node_id);
}

static void S10_go_while_idle_is_noop(void)
{
    ff_flare_t f;
    ff_flare_init(&f);

    ff_flare_result_t r = ff_flare_go(&f);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_EQUAL(FF_FLARE_IDLE, f.state);
}

static void S10_dismiss_while_idle_is_noop(void)
{
    ff_flare_t f;
    ff_flare_init(&f);

    ff_flare_result_t r = ff_flare_dismiss(&f);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_EQUAL(FF_FLARE_IDLE, f.state);
}

static void S10_flare_end_rx_while_idle_is_ignored(void)
{
    ff_flare_t f;
    ff_flare_init(&f);

    ff_flare_result_t r = ff_flare_on_flare_end_rx(&f, 42u);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_NONE, r.intent);
    TEST_ASSERT_EQUAL(FF_FLARE_IDLE, f.state);
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
    TEST_ASSERT_EQUAL(FF_FLARE_SENDING, f.state);
}

static void S10_receive_paired_flare_overrides_active_sending(void)
{
    /* judgment call 4, documented and directly tested. */
    ff_flare_t f;
    ff_flare_init(&f);
    ff_flare_send_begin(&f, 300u, 0u);
    TEST_ASSERT_EQUAL(FF_FLARE_SENDING, f.state);

    ff_flare_result_t r = ff_flare_on_flare_rx(&f, 9u, true, 30u, 1000u);

    TEST_ASSERT_TRUE(r.should_alert);
    TEST_ASSERT_EQUAL(FF_FLARE_RECEIVED, f.state);
    TEST_ASSERT_EQUAL_UINT32(9u, f.node_id);
}

static void S10_wraparound_safe_expiry_check(void)
{
    /* Mirrors S02's ff_crew wraparound test: expiry_ms sits 100 ticks
     * before a uint32_t rollover, now_ms is 100 ticks past it. True
     * elapsed time is 200ms past expiry, which must already read as
     * expired -- not the ~4.29 billion ms a naive comparison would see. */
    ff_flare_t f;
    ff_flare_init(&f);
    f.state = FF_FLARE_SENDING;
    f.expiry_ms = UINT32_MAX - 99u;

    ff_flare_result_t r = ff_flare_tick(&f, 100u);

    TEST_ASSERT_EQUAL(FF_FLARE_INTENT_SEND_FLARE_END, r.intent);
    TEST_ASSERT_EQUAL(FF_FLARE_IDLE, f.state);
}

static void S10_init_zeroes_to_idle(void)
{
    ff_flare_t f;
    memset(&f, 0xAA, sizeof(f));

    ff_flare_init(&f);

    TEST_ASSERT_EQUAL(FF_FLARE_IDLE, f.state);
    TEST_ASSERT_EQUAL_UINT32(0u, f.node_id);
    TEST_ASSERT_EQUAL_UINT32(0u, f.expiry_ms);
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
    RUN_TEST(S10_AC1_newest_flare_wins_while_locked);
    RUN_TEST(S10_AC1_go_transitions_received_to_locked);
    RUN_TEST(S10_AC1_dismiss_transitions_to_idle_and_unlocks);

    RUN_TEST(S10_AC2_receive_during_quiet_hours_still_sets_should_alert);

    RUN_TEST(S10_AC3_locked_node_zero_when_not_locked);
    RUN_TEST(S10_AC3_expiry_while_locked_unlocks_to_idle);

    RUN_TEST(S10_AC4_sender_auto_end_emits_flare_end_exactly_once);

    RUN_TEST(S10_send_cancel_while_idle_is_noop);
    RUN_TEST(S10_send_cancel_while_received_does_not_affect_receive_state);
    RUN_TEST(S10_go_while_idle_is_noop);
    RUN_TEST(S10_dismiss_while_idle_is_noop);
    RUN_TEST(S10_flare_end_rx_while_idle_is_ignored);
    RUN_TEST(S10_flare_end_rx_while_sending_is_ignored);
    RUN_TEST(S10_receive_paired_flare_overrides_active_sending);
    RUN_TEST(S10_wraparound_safe_expiry_check);
    RUN_TEST(S10_init_zeroes_to_idle);

    return UNITY_END();
}
