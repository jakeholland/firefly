/**
 * test_sigview.c — presence classifier tests for core/sigview.
 *
 * `ff_sigview_presence` is the only behavior left in this module (the S24
 * inbox rework moved the row-list/target machinery to ff_inbox.c and the
 * shell — see ff_sigview.h's top comment); this file was trimmed to match
 * on the tech-debt sprint that deleted the dead API (ff_sigview_init/
 * _build/_row_count/_row_at, ff_sigrow_t, the target_* functions).
 *
 * Test names follow docs/specs/S22-signals-rework.md's numbered
 * acceptance criteria (AC2, the presence classification criterion) —
 * kept for traceability even though S22's other criteria (AC1/AC3/AC4)
 * no longer have a home in this file.
 *
 * Proxy-check discipline (AGENTS.md standing brief / docs/review/
 * code-review.md item 6): presence tests exercise every branch
 * including ASSERTED-is-silent and "the freshest sighting wins" (RSSI
 * rescuing a LOST/NEVER position).
 */
#include "unity.h"

#include "ff_sigview.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* AC2 — honest presence                                               */
/* ------------------------------------------------------------------- */

static void S22_AC2_live_position_is_seen_with_age(void)
{
    uint32_t              age = 0xDEADBEEF;
    ff_sigview_presence_t p   = ff_sigview_presence(FF_FRESH_LIVE, 10000, false, 0, &age);
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN, p);
    TEST_ASSERT_EQUAL_UINT32(10000, age);
}

static void S22_AC2_stale_position_is_seen(void)
{
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN, ff_sigview_presence(FF_FRESH_STALE, 300000, false, 0, NULL));
}

static void S22_AC2_lost_position_is_lost_with_real_age(void)
{
    uint32_t              age = 0;
    ff_sigview_presence_t p   = ff_sigview_presence(FF_FRESH_LOST, 700000, false, 0, &age);
    TEST_ASSERT_EQUAL(FF_PRESENCE_LOST, p);
    TEST_ASSERT_EQUAL_UINT32(700000, age);
}

static void S22_AC2_never_no_rssi_is_linked_and_leaves_age_untouched(void)
{
    uint32_t              age = 0x1234;
    ff_sigview_presence_t p   = ff_sigview_presence(FF_FRESH_NEVER, 999999, false, 0, &age);
    TEST_ASSERT_EQUAL(FF_PRESENCE_LINKED, p);
    TEST_ASSERT_EQUAL_UINT32(0x1234, age); /* untouched — no honest age exists */
}

static void S22_AC2_asserted_is_silent_on_age_no_rssi_is_linked(void)
{
    /* An ASSERTED position (LOC_MANUAL) must NOT be read as a recent
     * sighting — with no direct packet, the member is LINKED, never SEEN,
     * regardless of the (meaningless) pos_age value. */
    TEST_ASSERT_EQUAL(FF_PRESENCE_LINKED, ff_sigview_presence(FF_FRESH_ASSERTED, 5000, false, 0, NULL));
}

static void S22_AC2_rssi_rescues_never_to_seen(void)
{
    uint32_t age = 0;
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN, ff_sigview_presence(FF_FRESH_NEVER, 999999, true, 5000, &age));
    TEST_ASSERT_EQUAL_UINT32(5000, age);
}

static void S22_AC2_freshest_sighting_wins_rssi_over_lost_position(void)
{
    /* Old measured position (LOST) but a fresh direct packet -> SEEN with
     * the fresh RSSI age (we DID just hear them). */
    uint32_t age = 0;
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN, ff_sigview_presence(FF_FRESH_LOST, 700000, true, 5000, &age));
    TEST_ASSERT_EQUAL_UINT32(5000, age);
}

static void S22_AC2_freshest_sighting_wins_position_over_old_rssi(void)
{
    uint32_t age = 0;
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN, ff_sigview_presence(FF_FRESH_LIVE, 10000, true, 700000, &age));
    TEST_ASSERT_EQUAL_UINT32(10000, age); /* min of the two */
}

static void S22_AC2_seen_lost_boundary_is_inclusive_toward_seen(void)
{
    /* age == FF_CREW_LOST_MS is still SEEN; one ms more is LOST. */
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN,
                      ff_sigview_presence(FF_FRESH_STALE, FF_CREW_LOST_MS, false, 0, NULL));
    TEST_ASSERT_EQUAL(FF_PRESENCE_LOST,
                      ff_sigview_presence(FF_FRESH_LOST, FF_CREW_LOST_MS + 1, false, 0, NULL));
}

/* ------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S22_AC2_live_position_is_seen_with_age);
    RUN_TEST(S22_AC2_stale_position_is_seen);
    RUN_TEST(S22_AC2_lost_position_is_lost_with_real_age);
    RUN_TEST(S22_AC2_never_no_rssi_is_linked_and_leaves_age_untouched);
    RUN_TEST(S22_AC2_asserted_is_silent_on_age_no_rssi_is_linked);
    RUN_TEST(S22_AC2_rssi_rescues_never_to_seen);
    RUN_TEST(S22_AC2_freshest_sighting_wins_rssi_over_lost_position);
    RUN_TEST(S22_AC2_freshest_sighting_wins_position_over_old_rssi);
    RUN_TEST(S22_AC2_seen_lost_boundary_is_inclusive_toward_seen);

    return UNITY_END();
}
