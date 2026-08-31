/**
 * test_feed.c — S08 core/feed acceptance criteria, slice (b).
 *
 * Test names follow docs/specs/S08-signals-t9.md's numbered acceptance
 * criteria: S08_ACn_description (AC3 is the feed's own criterion; a few
 * extra non-AC-numbered tests cover behavior the spec implies but doesn't
 * number explicitly — same convention as test_crew.c's unnumbered
 * "selection" tests).
 */
#include <string.h>

#include "unity.h"

#include "ff_feed.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* helpers                                                              */
/* ------------------------------------------------------------------- */

static ff_feed_item_t make_item(ff_feed_kind_t kind, uint32_t from_node, uint32_t at_ms, char const *text,
                                 bool unread)
{
    ff_feed_item_t it;
    memset(&it, 0, sizeof(it));
    it.kind = kind;
    it.from_node = from_node;
    it.at_ms = at_ms;
    if (text != NULL) {
        strncpy(it.text, text, sizeof(it.text) - 1);
    }
    it.unread = unread;
    return it;
}

/* ------------------------------------------------------------------- */
/* AC3 — newest-first ordering                                          */
/* ------------------------------------------------------------------- */

static void S08_AC3_empty_feed_has_zero_count_and_null_at(void)
{
    ff_feed_t f;
    ff_feed_init(&f);

    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(&f));
    TEST_ASSERT_NULL(ff_feed_at(&f, 0));
    TEST_ASSERT_EQUAL_UINT16(0, ff_feed_unread_count(&f));
}

static void S08_AC3_push_is_newest_first(void)
{
    ff_feed_t f;
    ff_feed_init(&f);

    ff_feed_item_t a = make_item(FEED_PULSE, 1, 100, "first", false);
    ff_feed_item_t b = make_item(FEED_TEXT, 2, 200, "second", false);
    ff_feed_item_t c = make_item(FEED_RALLY, 3, 300, "third", false);

    ff_feed_push(&f, &a);
    ff_feed_push(&f, &b);
    ff_feed_push(&f, &c);

    TEST_ASSERT_EQUAL_UINT8(3, ff_feed_count(&f));
    TEST_ASSERT_EQUAL_STRING("third", ff_feed_at(&f, 0)->text);
    TEST_ASSERT_EQUAL_STRING("second", ff_feed_at(&f, 1)->text);
    TEST_ASSERT_EQUAL_STRING("first", ff_feed_at(&f, 2)->text);
    TEST_ASSERT_NULL(ff_feed_at(&f, 3));
}

static void S08_AC3_item_fields_round_trip(void)
{
    ff_feed_t f;
    ff_feed_init(&f);

    ff_feed_item_t it = make_item(FEED_STATUS, 42, 12345, "RAGING", true);
    ff_feed_push(&f, &it);

    ff_feed_item_t const *got = ff_feed_at(&f, 0);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL(FEED_STATUS, got->kind);
    TEST_ASSERT_EQUAL_UINT32(42, got->from_node);
    TEST_ASSERT_EQUAL_UINT32(12345, got->at_ms);
    TEST_ASSERT_EQUAL_STRING("RAGING", got->text);
    TEST_ASSERT_TRUE(got->unread);
}

/* ------------------------------------------------------------------- */
/* AC3 — cap-32 boundary: exactly 32 keeps everything, the 33rd evicts   */
/* the oldest.                                                          */
/* ------------------------------------------------------------------- */

static void S08_AC3_exactly_32_pushes_keeps_every_item(void)
{
    ff_feed_t f;
    ff_feed_init(&f);

    for (int i = 0; i < FF_FEED_CAP; i++) {
        char text[8];
        snprintf(text, sizeof(text), "%d", i);
        ff_feed_item_t it = make_item(FEED_TEXT, (uint32_t)i, (uint32_t)i, text, false);
        ff_feed_push(&f, &it);
    }

    TEST_ASSERT_EQUAL_UINT8(FF_FEED_CAP, ff_feed_count(&f));
    /* Newest (index 0) is item 31, oldest still-present (index 31) is item 0 —
     * nothing evicted yet at exactly the cap. */
    TEST_ASSERT_EQUAL_STRING("31", ff_feed_at(&f, 0)->text);
    TEST_ASSERT_EQUAL_STRING("0", ff_feed_at(&f, FF_FEED_CAP - 1)->text);
}

static void S08_AC3_33rd_push_evicts_oldest(void)
{
    ff_feed_t f;
    ff_feed_init(&f);

    for (int i = 0; i < FF_FEED_CAP; i++) {
        char text[8];
        snprintf(text, sizeof(text), "%d", i);
        ff_feed_item_t it = make_item(FEED_TEXT, (uint32_t)i, (uint32_t)i, text, false);
        ff_feed_push(&f, &it);
    }

    /* 33rd push. */
    ff_feed_item_t it32 = make_item(FEED_TEXT, 32, 32, "32", false);
    ff_feed_push(&f, &it32);

    /* Count stays pinned at the cap — this is the mutation-check: if
     * ff_feed_push ever stopped capping `count` at FF_FEED_CAP (e.g. a
     * refactor that only fixed the physical-index math but dropped the
     * `if (!full)` guard), this assertion catches it even though the
     * newest/oldest checks below would still pass. */
    TEST_ASSERT_EQUAL_UINT8(FF_FEED_CAP, ff_feed_count(&f));

    /* Newest is now "32"; oldest surviving is "1" — "0" was evicted. */
    TEST_ASSERT_EQUAL_STRING("32", ff_feed_at(&f, 0)->text);
    TEST_ASSERT_EQUAL_STRING("1", ff_feed_at(&f, FF_FEED_CAP - 1)->text);

    /* "0" is nowhere in the ring any more (mutation-check: iterate the
     * whole visible range rather than trusting a single index). */
    for (uint8_t i = 0; i < ff_feed_count(&f); i++) {
        TEST_ASSERT_NOT_EQUAL_INT(0, strcmp(ff_feed_at(&f, i)->text, "0"));
    }
}

static void S08_AC3_many_pushes_beyond_cap_still_newest_first(void)
{
    /* Push well past 33 (2x the cap) to prove the ring keeps behaving,
     * not just surviving one extra push past the boundary. */
    ff_feed_t f;
    ff_feed_init(&f);

    enum { N = FF_FEED_CAP * 2 + 5 };
    for (int i = 0; i < N; i++) {
        char text[8];
        snprintf(text, sizeof(text), "%d", i);
        ff_feed_item_t it = make_item(FEED_TEXT, (uint32_t)i, (uint32_t)i, text, false);
        ff_feed_push(&f, &it);
    }

    TEST_ASSERT_EQUAL_UINT8(FF_FEED_CAP, ff_feed_count(&f));
    char expect_newest[8];
    snprintf(expect_newest, sizeof(expect_newest), "%d", N - 1);
    TEST_ASSERT_EQUAL_STRING(expect_newest, ff_feed_at(&f, 0)->text);
    char expect_oldest[8];
    snprintf(expect_oldest, sizeof(expect_oldest), "%d", N - FF_FEED_CAP);
    TEST_ASSERT_EQUAL_STRING(expect_oldest, ff_feed_at(&f, FF_FEED_CAP - 1)->text);
}

/* ------------------------------------------------------------------- */
/* AC3 — unread count transitions                                       */
/* ------------------------------------------------------------------- */

static void S08_AC3_unread_count_increments_on_unread_push(void)
{
    ff_feed_t f;
    ff_feed_init(&f);

    ff_feed_item_t a = make_item(FEED_PULSE, 1, 0, "a", true);
    ff_feed_item_t b = make_item(FEED_PULSE, 2, 1, "b", false);
    ff_feed_item_t c = make_item(FEED_PULSE, 3, 2, "c", true);

    ff_feed_push(&f, &a);
    TEST_ASSERT_EQUAL_UINT16(1, ff_feed_unread_count(&f));
    ff_feed_push(&f, &b);
    TEST_ASSERT_EQUAL_UINT16(1, ff_feed_unread_count(&f)); /* read item doesn't bump it */
    ff_feed_push(&f, &c);
    TEST_ASSERT_EQUAL_UINT16(2, ff_feed_unread_count(&f));
}

static void S08_AC3_evicting_an_unread_item_decrements_unread_count(void)
{
    /* Fill the ring with UNREAD items, then push one more (evicting the
     * unread oldest) and confirm unread_count drops by exactly one, not
     * left stale at FF_FEED_CAP. This is the eviction-side mutation-check
     * companion to the push-side one above. */
    ff_feed_t f;
    ff_feed_init(&f);

    for (int i = 0; i < FF_FEED_CAP; i++) {
        ff_feed_item_t it = make_item(FEED_PULSE, (uint32_t)i, (uint32_t)i, "u", true);
        ff_feed_push(&f, &it);
    }
    TEST_ASSERT_EQUAL_UINT16(FF_FEED_CAP, ff_feed_unread_count(&f));

    ff_feed_item_t read_item = make_item(FEED_PULSE, 99, 99, "read", false);
    ff_feed_push(&f, &read_item);

    /* One unread item evicted, one non-unread item pushed -> net -1. */
    TEST_ASSERT_EQUAL_UINT16(FF_FEED_CAP - 1, ff_feed_unread_count(&f));
}

static void S08_AC3_evicting_a_read_item_leaves_unread_count_unchanged(void)
{
    /* Companion boundary case: evicting a *read* item must NOT touch
     * unread_count at all (guards against an unconditional decrement that
     * would happen to pass the unread-eviction test above by luck). */
    ff_feed_t f;
    ff_feed_init(&f);

    ff_feed_item_t oldest_read = make_item(FEED_PULSE, 0, 0, "read0", false);
    ff_feed_push(&f, &oldest_read);
    for (int i = 1; i < FF_FEED_CAP; i++) {
        ff_feed_item_t it = make_item(FEED_PULSE, (uint32_t)i, (uint32_t)i, "u", true);
        ff_feed_push(&f, &it);
    }
    uint16_t before = ff_feed_unread_count(&f);
    TEST_ASSERT_EQUAL_UINT16(FF_FEED_CAP - 1, before);

    ff_feed_item_t evictor = make_item(FEED_PULSE, 999, 999, "evictor", false);
    ff_feed_push(&f, &evictor);

    TEST_ASSERT_EQUAL_UINT16(before, ff_feed_unread_count(&f));
}

static void S08_AC3_mark_all_read_zeroes_count_and_clears_flags(void)
{
    ff_feed_t f;
    ff_feed_init(&f);

    for (int i = 0; i < 5; i++) {
        ff_feed_item_t it = make_item(FEED_PULSE, (uint32_t)i, (uint32_t)i, "u", true);
        ff_feed_push(&f, &it);
    }
    TEST_ASSERT_EQUAL_UINT16(5, ff_feed_unread_count(&f));

    ff_feed_mark_all_read(&f);

    TEST_ASSERT_EQUAL_UINT16(0, ff_feed_unread_count(&f));
    for (uint8_t i = 0; i < ff_feed_count(&f); i++) {
        TEST_ASSERT_FALSE(ff_feed_at(&f, i)->unread);
    }
    /* Count/order themselves are untouched by mark-all-read. */
    TEST_ASSERT_EQUAL_UINT8(5, ff_feed_count(&f));
}

static void S08_AC3_unread_count_never_underflows_past_zero(void)
{
    /* Defensive: mark_all_read followed by more evictions of already-read
     * items must never wrap unread_count negative (it's unsigned). */
    ff_feed_t f;
    ff_feed_init(&f);

    for (int i = 0; i < FF_FEED_CAP; i++) {
        ff_feed_item_t it = make_item(FEED_PULSE, (uint32_t)i, (uint32_t)i, "u", true);
        ff_feed_push(&f, &it);
    }
    ff_feed_mark_all_read(&f);
    TEST_ASSERT_EQUAL_UINT16(0, ff_feed_unread_count(&f));

    for (int i = 0; i < 10; i++) {
        ff_feed_item_t it = make_item(FEED_PULSE, (uint32_t)(100 + i), (uint32_t)(100 + i), "r", false);
        ff_feed_push(&f, &it);
    }
    TEST_ASSERT_EQUAL_UINT16(0, ff_feed_unread_count(&f));
}

/* ------------------------------------------------------------------- */
/* S24 AC1 — direction fact + per-item mark-read seam                   */
/* ------------------------------------------------------------------- */

/* A zero-initialized item's direction is UNKNOWN (the honest default is
 * "not recorded", never "broadcast"), and a pushed item's direction +
 * to_node survive the ring verbatim. */
static void S24_AC1_direction_defaults_unknown_and_round_trips(void)
{
    ff_feed_item_t zeroed;
    memset(&zeroed, 0, sizeof(zeroed));
    TEST_ASSERT_EQUAL(FEED_DIR_UNKNOWN, zeroed.dir);

    ff_feed_t f;
    ff_feed_init(&f);

    ff_feed_item_t out = make_item(FEED_TEXT, 0, 100, "omw", false);
    out.dir = FEED_DIR_OUT;
    out.to_node = 0xDA1Au;
    ff_feed_push(&f, &out);

    ff_feed_item_t in = make_item(FEED_TEXT, 0xBEEFu, 200, "where", true);
    in.dir = FEED_DIR_DIRECT;
    ff_feed_push(&f, &in);

    TEST_ASSERT_EQUAL(FEED_DIR_DIRECT, ff_feed_at(&f, 0)->dir);
    TEST_ASSERT_EQUAL(FEED_DIR_OUT, ff_feed_at(&f, 1)->dir);
    TEST_ASSERT_EQUAL_UINT32(0xDA1Au, ff_feed_at(&f, 1)->to_node);
}

/* mark_read_at marks exactly the idx-th NEWEST item and decrements the
 * running count by exactly one — the OTHER unread items keep their flags
 * (the per-thread mark-read property this seam exists for). */
static void S24_AC1_mark_read_at_marks_only_that_item(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    for (uint32_t i = 0; i < 3; ++i) {
        ff_feed_item_t it = make_item(FEED_TEXT, 0x100u + i, 100u * (i + 1u), "m", true);
        ff_feed_push(&f, &it);
    }
    TEST_ASSERT_EQUAL_UINT16(3, ff_feed_unread_count(&f));

    ff_feed_mark_read_at(&f, 1); /* the MIDDLE item (from_node 0x101) */

    TEST_ASSERT_EQUAL_UINT16(2, ff_feed_unread_count(&f));
    TEST_ASSERT_TRUE(ff_feed_at(&f, 0)->unread);  /* newest untouched */
    TEST_ASSERT_FALSE(ff_feed_at(&f, 1)->unread); /* marked */
    TEST_ASSERT_TRUE(ff_feed_at(&f, 2)->unread);  /* oldest untouched */
    /* And it hit the item we asked for, not a same-index physical slot. */
    TEST_ASSERT_EQUAL_UINT32(0x101u, ff_feed_at(&f, 1)->from_node);

    /* Marking an already-read item is a no-op, not a double decrement. */
    ff_feed_mark_read_at(&f, 1);
    TEST_ASSERT_EQUAL_UINT16(2, ff_feed_unread_count(&f));

    /* Out-of-range and NULL are no-ops. */
    ff_feed_mark_read_at(&f, 200);
    ff_feed_mark_read_at(NULL, 0);
    TEST_ASSERT_EQUAL_UINT16(2, ff_feed_unread_count(&f));
}

/* Proxy check: after the ring has WRAPPED, logical index 0 is not
 * physical slot 0 — mark_read_at must use the same idx-th-newest mapping
 * ff_feed_at uses. Push CAP+2 items so head has lapped, then mark the
 * newest and verify by CONTENT (from_node), not just by count. */
static void S24_AC1_mark_read_at_maps_through_a_wrapped_ring(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    for (uint32_t i = 0; i < (uint32_t)FF_FEED_CAP + 2u; ++i) {
        ff_feed_item_t it = make_item(FEED_TEXT, 0x1000u + i, i, "m", true);
        ff_feed_push(&f, &it);
    }
    TEST_ASSERT_EQUAL_UINT16(FF_FEED_CAP, ff_feed_unread_count(&f));

    ff_feed_mark_read_at(&f, 0);

    TEST_ASSERT_EQUAL_UINT16(FF_FEED_CAP - 1u, ff_feed_unread_count(&f));
    ff_feed_item_t const *newest = ff_feed_at(&f, 0);
    TEST_ASSERT_EQUAL_UINT32(0x1000u + (uint32_t)FF_FEED_CAP + 1u, newest->from_node);
    TEST_ASSERT_FALSE(newest->unread);
    TEST_ASSERT_TRUE(ff_feed_at(&f, 1)->unread); /* neighbor untouched */
}

/* ------------------------------------------------------------------- */
/* NULL-argument guards                                                 */
/* ------------------------------------------------------------------- */

static void S08_AC3_null_guards_are_no_ops_not_crashes(void)
{
    ff_feed_push(NULL, NULL);
    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(NULL));
    TEST_ASSERT_NULL(ff_feed_at(NULL, 0));
    TEST_ASSERT_EQUAL_UINT16(0, ff_feed_unread_count(NULL));
    ff_feed_mark_all_read(NULL);

    ff_feed_t f;
    ff_feed_init(&f);
    ff_feed_item_t it = make_item(FEED_PULSE, 1, 1, "x", false);
    ff_feed_push(&f, NULL); /* no-op, doesn't corrupt f */
    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(&f));
    ff_feed_push(&f, &it);
    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(&f));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S08_AC3_empty_feed_has_zero_count_and_null_at);
    RUN_TEST(S08_AC3_push_is_newest_first);
    RUN_TEST(S08_AC3_item_fields_round_trip);

    RUN_TEST(S08_AC3_exactly_32_pushes_keeps_every_item);
    RUN_TEST(S08_AC3_33rd_push_evicts_oldest);
    RUN_TEST(S08_AC3_many_pushes_beyond_cap_still_newest_first);

    RUN_TEST(S08_AC3_unread_count_increments_on_unread_push);
    RUN_TEST(S08_AC3_evicting_an_unread_item_decrements_unread_count);
    RUN_TEST(S08_AC3_evicting_a_read_item_leaves_unread_count_unchanged);
    RUN_TEST(S08_AC3_mark_all_read_zeroes_count_and_clears_flags);
    RUN_TEST(S08_AC3_unread_count_never_underflows_past_zero);

    RUN_TEST(S24_AC1_direction_defaults_unknown_and_round_trips);
    RUN_TEST(S24_AC1_mark_read_at_marks_only_that_item);
    RUN_TEST(S24_AC1_mark_read_at_maps_through_a_wrapped_ring);

    RUN_TEST(S08_AC3_null_guards_are_no_ops_not_crashes);

    return UNITY_END();
}
