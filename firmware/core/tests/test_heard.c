/**
 * test_heard.c — S08 core/heard acceptance tests (PR #25 code review,
 * MEDIUM finding: bounded LRU-evictable heard-node list, separate from
 * the paired ff_crew roster).
 *
 * Test names: S08_heard_ACn_description, mirroring the same
 * S08_ACn_description convention test_feed.c/test_wiring.c use for this
 * spec, since this module exists specifically to satisfy the review's
 * ruling on S08's AC4 (crew filtering) — not a separately-numbered spec
 * criterion of its own.
 */
#include <string.h>

#include "unity.h"

#include "ff_heard.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* basics                                                                */
/* ------------------------------------------------------------------- */

static void S08_heard_empty_list_has_zero_count(void)
{
    ff_heard_t h;
    ff_heard_init(&h);

    TEST_ASSERT_EQUAL_UINT8(0, ff_heard_count(&h));
    TEST_ASSERT_NULL(ff_heard_at(&h, 0));
    TEST_ASSERT_FALSE(ff_heard_contains(&h, 1));
}

static void S08_heard_note_appends_new_id(void)
{
    ff_heard_t h;
    ff_heard_init(&h);

    ff_heard_note(&h, 100, 5000);

    TEST_ASSERT_EQUAL_UINT8(1, ff_heard_count(&h));
    TEST_ASSERT_TRUE(ff_heard_contains(&h, 100));
    ff_heard_entry_t const *e = ff_heard_at(&h, 0);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT32(100, e->node_id);
    TEST_ASSERT_EQUAL_UINT32(5000, e->last_heard_ms);
}

static void S08_heard_renote_existing_id_updates_timestamp_not_count(void)
{
    ff_heard_t h;
    ff_heard_init(&h);

    ff_heard_note(&h, 100, 1000);
    ff_heard_note(&h, 200, 1500);
    ff_heard_note(&h, 100, 9000); /* re-heard, later */

    TEST_ASSERT_EQUAL_UINT8(2, ff_heard_count(&h)); /* still 2 distinct ids, not 3 */
    bool found = false;
    for (uint8_t i = 0; i < ff_heard_count(&h); i++) {
        ff_heard_entry_t const *e = ff_heard_at(&h, i);
        if (e->node_id == 100) {
            TEST_ASSERT_EQUAL_UINT32(9000, e->last_heard_ms); /* updated, not stuck at 1000 */
            found = true;
        }
    }
    TEST_ASSERT_TRUE(found);
}

/* ------------------------------------------------------------------- */
/* boundary: exactly FF_HEARD_MAX keeps everything; the (MAX+1)th evicts */
/* the least-recently-heard entry (true LRU, not FIFO/insertion-order). */
/* ------------------------------------------------------------------- */

static void S08_heard_exactly_max_distinct_ids_keeps_all(void)
{
    ff_heard_t h;
    ff_heard_init(&h);

    for (int i = 0; i < FF_HEARD_MAX; i++) {
        ff_heard_note(&h, (uint32_t)(1000 + i), (uint32_t)(1000 + i));
    }

    TEST_ASSERT_EQUAL_UINT8(FF_HEARD_MAX, ff_heard_count(&h));
    for (int i = 0; i < FF_HEARD_MAX; i++) {
        TEST_ASSERT_TRUE(ff_heard_contains(&h, (uint32_t)(1000 + i)));
    }
}

static void S08_heard_over_max_evicts_least_recently_heard(void)
{
    ff_heard_t h;
    ff_heard_init(&h);

    /* Fill to cap with strictly increasing timestamps: id 1000+i heard at
     * time i. id 1000 (i=0) is the least-recently-heard by construction. */
    for (int i = 0; i < FF_HEARD_MAX; i++) {
        ff_heard_note(&h, (uint32_t)(1000 + i), (uint32_t)i);
    }

    /* A brand-new id arrives — must evict 1000 (timestamp 0, the oldest),
     * NOT 1001 (which a naive FIFO/insertion-order eviction would pick if
     * it happened to be first in some other ordering scheme). */
    ff_heard_note(&h, 9999, 999);

    TEST_ASSERT_EQUAL_UINT8(FF_HEARD_MAX, ff_heard_count(&h)); /* still pinned at cap */
    TEST_ASSERT_FALSE(ff_heard_contains(&h, 1000)); /* evicted */
    TEST_ASSERT_TRUE(ff_heard_contains(&h, 1001));  /* survives — second-oldest, not evicted */
    TEST_ASSERT_TRUE(ff_heard_contains(&h, 9999));  /* the new arrival */
}

static void S08_heard_renoting_the_oldest_protects_it_from_eviction(void)
{
    /* Companion to the above: if the entry that WOULD be LRU-evicted gets
     * re-heard (refreshing its timestamp) before the evicting note()
     * call, it must survive instead — proving eviction reads live
     * timestamps, not a frozen insertion order. */
    ff_heard_t h;
    ff_heard_init(&h);

    for (int i = 0; i < FF_HEARD_MAX; i++) {
        ff_heard_note(&h, (uint32_t)(2000 + i), (uint32_t)i);
    }
    /* id 2000 (originally oldest, timestamp 0) is re-heard now, at a time
     * later than everything else. */
    ff_heard_note(&h, 2000, 500);

    /* Next new arrival should evict id 2001 (now the actual oldest,
     * timestamp 1), not 2000 (just refreshed to 500). */
    ff_heard_note(&h, 8888, 501);

    TEST_ASSERT_TRUE(ff_heard_contains(&h, 2000));  /* protected by the re-note */
    TEST_ASSERT_FALSE(ff_heard_contains(&h, 2001)); /* now the true LRU, evicted */
}

static void S08_heard_many_pushes_beyond_cap_stays_bounded(void)
{
    /* Push well past 2x the cap to prove the list doesn't quietly grow
     * unbounded under sustained flood conditions (the actual festival
     * scenario this module exists for), not just survive one eviction. */
    ff_heard_t h;
    ff_heard_init(&h);

    for (int i = 0; i < FF_HEARD_MAX * 3; i++) {
        ff_heard_note(&h, (uint32_t)(5000 + i), (uint32_t)i);
    }

    TEST_ASSERT_EQUAL_UINT8(FF_HEARD_MAX, ff_heard_count(&h));
    /* The most recent FF_HEARD_MAX ids must all still be present. */
    for (int i = FF_HEARD_MAX * 2; i < FF_HEARD_MAX * 3; i++) {
        TEST_ASSERT_TRUE(ff_heard_contains(&h, (uint32_t)(5000 + i)));
    }
    /* The earliest ids are long gone. */
    TEST_ASSERT_FALSE(ff_heard_contains(&h, 5000));
}

/* ------------------------------------------------------------------- */
/* NULL-argument guards                                                 */
/* ------------------------------------------------------------------- */

/* S16 slice b2 (PR #46 review caveat): the shell purges its own id from
 * the heard list once on_my_info names it. This is the primitive that
 * makes that possible; the shell-level behavior is pinned in
 * app/tests/test_shell.c (S16_b2_my_info_purges_our_own_id_from_heard). */
static void S16_b2_heard_remove_forgets_only_the_named_id(void)
{
    ff_heard_t h;
    ff_heard_init(&h);
    ff_heard_note(&h, 0xA, 100);
    ff_heard_note(&h, 0xB, 200);
    ff_heard_note(&h, 0xC, 300);

    TEST_ASSERT_TRUE(ff_heard_remove(&h, 0xB));
    TEST_ASSERT_EQUAL_UINT8(2, ff_heard_count(&h));
    TEST_ASSERT_FALSE(ff_heard_contains(&h, 0xB));
    TEST_ASSERT_TRUE(ff_heard_contains(&h, 0xA));
    TEST_ASSERT_TRUE(ff_heard_contains(&h, 0xC));

    /* Removing an untracked id reports false and changes nothing. */
    TEST_ASSERT_FALSE(ff_heard_remove(&h, 0xB));
    TEST_ASSERT_EQUAL_UINT8(2, ff_heard_count(&h));

    /* A removed id can be re-noted later as a brand-new sighting. */
    ff_heard_note(&h, 0xB, 400);
    TEST_ASSERT_TRUE(ff_heard_contains(&h, 0xB));
    TEST_ASSERT_EQUAL_UINT8(3, ff_heard_count(&h));

    /* And the freed slot genuinely came back: fill to the cap and check
     * the count is exactly FF_HEARD_MAX, not FF_HEARD_MAX - 1. */
    for (uint32_t i = 0; i < FF_HEARD_MAX; i++) {
        ff_heard_note(&h, 0x1000u + i, 500 + i);
    }
    TEST_ASSERT_EQUAL_UINT8(FF_HEARD_MAX, ff_heard_count(&h));

    TEST_ASSERT_FALSE(ff_heard_remove(NULL, 0xA)); /* NULL guard */
}

static void S08_heard_null_guards_are_no_ops_not_crashes(void)
{
    ff_heard_init(NULL);
    ff_heard_note(NULL, 1, 1);
    TEST_ASSERT_EQUAL_UINT8(0, ff_heard_count(NULL));
    TEST_ASSERT_NULL(ff_heard_at(NULL, 0));
    TEST_ASSERT_FALSE(ff_heard_contains(NULL, 1));

    ff_heard_t h;
    ff_heard_init(&h);
    ff_heard_note(&h, 1, 100);
    TEST_ASSERT_EQUAL_UINT8(1, ff_heard_count(&h)); /* untouched by the NULL calls above */
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S08_heard_empty_list_has_zero_count);
    RUN_TEST(S08_heard_note_appends_new_id);
    RUN_TEST(S08_heard_renote_existing_id_updates_timestamp_not_count);

    RUN_TEST(S08_heard_exactly_max_distinct_ids_keeps_all);
    RUN_TEST(S08_heard_over_max_evicts_least_recently_heard);
    RUN_TEST(S08_heard_renoting_the_oldest_protects_it_from_eviction);
    RUN_TEST(S08_heard_many_pushes_beyond_cap_stays_bounded);

    RUN_TEST(S16_b2_heard_remove_forgets_only_the_named_id);

    RUN_TEST(S08_heard_null_guards_are_no_ops_not_crashes);

    return UNITY_END();
}
