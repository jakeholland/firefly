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
 * 2026-09-07 [api] presence-heard-vs-position (docs/specs/S02-core-crew.md
 * amendment): `ff_sigview_presence` was re-based off `ff_crew_presence`
 * (ANY packet heard, core/ff_crew.h) instead of position freshness +
 * direct-packet RSSI age — see ff_sigview.h's top comment for the full
 * "why are we LOST?" rationale. This file's fixtures were rewritten to
 * match the new (ff_crew_presence_t, heard_age_ms, out_age_ms) signature;
 * the old position/RSSI-rescue fixtures (a fresh direct packet rescuing a
 * LOST position to SEEN, etc.) no longer apply — that "is there ANY
 * recent evidence" question is now `ff_crew_presence`'s alone
 * (core/tests/test_crew.c's HEARD_* tests), not this classifier's.
 *
 * Proxy-check discipline (AGENTS.md standing brief / docs/review/
 * code-review.md item 6): presence tests exercise every branch of the
 * (now trivial) heard -> {SEEN, LOST, LINKED} mapping, plus the
 * passthrough of `heard_age_ms` into `out_age_ms`.
 */
#include "unity.h"

#include "ff_sigview.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* AC2 — honest presence                                               */
/* ------------------------------------------------------------------- */

static void S22_AC2_heard_is_seen_with_age(void)
{
    uint32_t              age = 0xDEADBEEF;
    ff_sigview_presence_t p   = ff_sigview_presence(FF_CREW_PRESENCE_HEARD, 30000, &age);
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN, p);
    TEST_ASSERT_EQUAL_UINT32(30000, age);
}

static void S22_AC2_stale_heard_is_seen_with_age(void)
{
    uint32_t              age = 0;
    ff_sigview_presence_t p   = ff_sigview_presence(FF_CREW_PRESENCE_STALE, 300000, &age);
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN, p);
    TEST_ASSERT_EQUAL_UINT32(300000, age);
}

static void S22_AC2_lost_heard_is_lost_with_real_age(void)
{
    uint32_t              age = 0;
    ff_sigview_presence_t p   = ff_sigview_presence(FF_CREW_PRESENCE_LOST, 700000, &age);
    TEST_ASSERT_EQUAL(FF_PRESENCE_LOST, p);
    TEST_ASSERT_EQUAL_UINT32(700000, age);
}

static void S22_AC2_never_heard_is_linked_and_leaves_age_untouched(void)
{
    uint32_t              age = 0x1234;
    ff_sigview_presence_t p   = ff_sigview_presence(FF_CREW_PRESENCE_NEVER, 999999, &age);
    TEST_ASSERT_EQUAL(FF_PRESENCE_LINKED, p);
    TEST_ASSERT_EQUAL_UINT32(0x1234, age); /* untouched — no honest age exists */
}

static void S22_AC2_out_age_ms_may_be_null(void)
{
    /* Every non-LINKED branch guards on out_age_ms != NULL before writing
     * through it — a caller that doesn't want the age must not crash. */
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN, ff_sigview_presence(FF_CREW_PRESENCE_HEARD, 1000, NULL));
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN, ff_sigview_presence(FF_CREW_PRESENCE_STALE, 1000, NULL));
    TEST_ASSERT_EQUAL(FF_PRESENCE_LOST, ff_sigview_presence(FF_CREW_PRESENCE_LOST, 1000, NULL));
    TEST_ASSERT_EQUAL(FF_PRESENCE_LINKED, ff_sigview_presence(FF_CREW_PRESENCE_NEVER, 1000, NULL));
}

static void S22_AC2_passthrough_is_verbatim_not_a_second_threshold(void)
{
    /* ff_crew_presence has already applied the HEARD/STALE/LOST boundary
     * (core/ff_crew.h's FF_CREW_HEARD_LIVE_MS/FF_CREW_HEARD_LOST_MS) —
     * this classifier must not re-derive it from heard_age_ms. An
     * arbitrarily large age tagged HEARD still reads SEEN, and an
     * arbitrarily small age tagged LOST still reads LOST: the enum
     * alone decides, the ms value is carried through only for display. */
    uint32_t age = 0;
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN, ff_sigview_presence(FF_CREW_PRESENCE_HEARD, 99999999u, &age));
    TEST_ASSERT_EQUAL_UINT32(99999999u, age);
    TEST_ASSERT_EQUAL(FF_PRESENCE_LOST, ff_sigview_presence(FF_CREW_PRESENCE_LOST, 1u, &age));
    TEST_ASSERT_EQUAL_UINT32(1u, age);
}

/* ------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S22_AC2_heard_is_seen_with_age);
    RUN_TEST(S22_AC2_stale_heard_is_seen_with_age);
    RUN_TEST(S22_AC2_lost_heard_is_lost_with_real_age);
    RUN_TEST(S22_AC2_never_heard_is_linked_and_leaves_age_untouched);
    RUN_TEST(S22_AC2_out_age_ms_may_be_null);
    RUN_TEST(S22_AC2_passthrough_is_verbatim_not_a_second_threshold);

    return UNITY_END();
}
