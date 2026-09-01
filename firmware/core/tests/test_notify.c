/**
 * test_notify.c — S26 slice (d), AC1: core `ff_notify` (docs/specs/
 * S26-device-lifecycle.md, "(d) ff_notify + message banner").
 *
 * Proxy-check discipline (AGENTS.md standing brief / docs/review/
 * code-review.md item 6): the FIFO test asserts the exact SEQUENCE
 * popped, not just a count; the overflow test asserts WHICH entry
 * survived (the newest four, not merely "count == 4"); the coalesce
 * tests assert both the boundary that DOES coalesce (2000ms, count
 * unchanged, text/at_ms updated) and the one that does NOT (2001ms,
 * count grows, a second entry) — a test that only checked one side could
 * pass on a queue that always (or never) coalesces.
 */
#include <string.h>

#include "unity.h"

#include "ff_notify.h"

void setUp(void) {}
void tearDown(void) {}

#define NOW ((uint32_t)1000000u)

/* ------------------------------------------------------------------- */
/* FIFO order + count                                                   */
/* ------------------------------------------------------------------- */

static void S26_AC1_push_then_head_is_oldest(void)
{
    ff_notify_t q;
    ff_notify_init(&q);

    TEST_ASSERT_TRUE(ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 1, "a", NOW));
    TEST_ASSERT_TRUE(ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 2, "b", NOW + 10u));
    TEST_ASSERT_EQUAL_UINT8(2, ff_notify_count(&q));

    ff_notify_entry_t const *h = ff_notify_head(&q);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_UINT32(1, h->node_id);
    TEST_ASSERT_EQUAL_STRING("a", h->text);
}

static void S26_AC1_fifo_pop_order(void)
{
    ff_notify_t q;
    ff_notify_init(&q);

    ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 1, "a", NOW);
    ff_notify_push(&q, FF_NOTIFY_RALLY, FF_NOTIFY_TIER_BANNER, 2, "b", NOW + 10u);
    ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 3, "c", NOW + 20u);

    uint32_t const expect_node[] = {1u, 2u, 3u};
    char const *expect_text[] = {"a", "b", "c"};
    for (int i = 0; i < 3; i++) {
        ff_notify_entry_t const *h = ff_notify_head(&q);
        TEST_ASSERT_NOT_NULL(h);
        TEST_ASSERT_EQUAL_UINT32(expect_node[i], h->node_id);
        TEST_ASSERT_EQUAL_STRING(expect_text[i], h->text);
        TEST_ASSERT_TRUE(ff_notify_pop(&q));
    }
    TEST_ASSERT_EQUAL_UINT8(0, ff_notify_count(&q));
    TEST_ASSERT_NULL(ff_notify_head(&q));
    TEST_ASSERT_FALSE(ff_notify_pop(&q)); /* empty: no-op */
}

/* ------------------------------------------------------------------- */
/* Overflow: oldest dropped                                             */
/* ------------------------------------------------------------------- */

static void S26_AC1_overflow_drops_oldest(void)
{
    ff_notify_t q;
    ff_notify_init(&q);

    /* Five distinct (kind, node) pairs so none coalesce. */
    for (uint32_t i = 1; i <= 5u; i++) {
        char text[8];
        (void)snprintf(text, sizeof(text), "n%u", (unsigned)i);
        ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, i, text, NOW + i);
    }

    TEST_ASSERT_EQUAL_UINT8(FF_NOTIFY_DEPTH, ff_notify_count(&q));
    /* node 1 (the first pushed) was evicted; the survivors are 2..5,
     * oldest-first. */
    uint32_t const expect_node[] = {2u, 3u, 4u, 5u};
    for (int i = 0; i < 4; i++) {
        ff_notify_entry_t const *h = ff_notify_head(&q);
        TEST_ASSERT_NOT_NULL(h);
        TEST_ASSERT_EQUAL_UINT32(expect_node[i], h->node_id);
        TEST_ASSERT_TRUE(ff_notify_pop(&q));
    }
}

/* ------------------------------------------------------------------- */
/* Expiry                                                                */
/* ------------------------------------------------------------------- */

static void S26_AC1_banner_expiry_is_6000ms_after_at_ms(void)
{
    ff_notify_t q;
    ff_notify_init(&q);
    ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 1, "hi", NOW);

    /* Not yet expired one tick before the deadline. */
    ff_notify_tick(&q, NOW + 6000u - 1u);
    TEST_ASSERT_EQUAL_UINT8(1, ff_notify_count(&q));

    /* Expired exactly AT the deadline — inclusive boundary (ff_notify.h
     * judgment call 1, matching ff_flare's convention). */
    ff_notify_tick(&q, NOW + 6000u);
    TEST_ASSERT_EQUAL_UINT8(0, ff_notify_count(&q));
    TEST_ASSERT_NULL(ff_notify_head(&q));
}

static void S26_AC1_tick_expires_only_the_due_entry(void)
{
    ff_notify_t q;
    ff_notify_init(&q);
    ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 1, "old", NOW);
    ff_notify_push(&q, FF_NOTIFY_RALLY, FF_NOTIFY_TIER_BANNER, 2, "new", NOW + 5000u);

    /* At NOW+6000: node 1 (at_ms=NOW) has reached its NOW+6000 deadline;
     * node 2 (at_ms=NOW+5000) has not (its deadline is NOW+11000). */
    ff_notify_tick(&q, NOW + 6000u);
    TEST_ASSERT_EQUAL_UINT8(1, ff_notify_count(&q));
    ff_notify_entry_t const *h = ff_notify_head(&q);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_UINT32(2, h->node_id);
}

/* TAKEOVER-tier entries carry expiry_ms == 0 (ff_notify.h judgment call
 * 3: "no core-managed deadline") — tick must never drop one on its own,
 * however far `now_ms` advances. Unexercised by the shell this slice,
 * but the queue's own contract is still asserted directly. */
static void S26_takeover_tier_never_auto_expires(void)
{
    ff_notify_t q;
    ff_notify_init(&q);
    ff_notify_push(&q, FF_NOTIFY_FLARE, FF_NOTIFY_TIER_TAKEOVER, 1, "flare", NOW);
    TEST_ASSERT_EQUAL_UINT32(0, ff_notify_head(&q)->expiry_ms);

    ff_notify_tick(&q, NOW + 1000000u);
    TEST_ASSERT_EQUAL_UINT8(1, ff_notify_count(&q));
}

/* ------------------------------------------------------------------- */
/* Dismiss                                                               */
/* ------------------------------------------------------------------- */

static void S26_AC1_dismiss_by_index_shifts_remainder(void)
{
    ff_notify_t q;
    ff_notify_init(&q);
    ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 1, "a", NOW);
    ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 2, "b", NOW + 10u);
    ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 3, "c", NOW + 20u);

    TEST_ASSERT_TRUE(ff_notify_dismiss(&q, 1)); /* remove "b" */
    TEST_ASSERT_EQUAL_UINT8(2, ff_notify_count(&q));
    TEST_ASSERT_EQUAL_UINT32(1, ff_notify_head(&q)->node_id);
    ff_notify_pop(&q);
    TEST_ASSERT_EQUAL_UINT32(3, ff_notify_head(&q)->node_id); /* "b" is gone, "c" shifted down */
}

static void S26_AC1_dismiss_out_of_range_is_noop(void)
{
    ff_notify_t q;
    ff_notify_init(&q);
    ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 1, "a", NOW);
    TEST_ASSERT_FALSE(ff_notify_dismiss(&q, 1));
    TEST_ASSERT_FALSE(ff_notify_dismiss(&q, 99));
    TEST_ASSERT_EQUAL_UINT8(1, ff_notify_count(&q));
}

/* ------------------------------------------------------------------- */
/* Coalescing (spec AC1)                                                */
/* ------------------------------------------------------------------- */

static void S26_AC1_coalesce_within_2s_replaces_in_place(void)
{
    ff_notify_t q;
    ff_notify_init(&q);
    ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 5, "hi", NOW);
    TEST_ASSERT_EQUAL_UINT8(1, ff_notify_count(&q));

    /* Same node+kind, exactly at the 2000ms boundary: still coalesces
     * (inclusive). */
    TEST_ASSERT_TRUE(ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 5, "hi again", NOW + 2000u));
    TEST_ASSERT_EQUAL_UINT8(1, ff_notify_count(&q)); /* no new entry */

    ff_notify_entry_t const *h = ff_notify_head(&q);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_STRING("hi again", h->text);
    TEST_ASSERT_EQUAL_UINT32(NOW + 2000u, h->at_ms);
    TEST_ASSERT_EQUAL_UINT32(NOW + 2000u + FF_NOTIFY_BANNER_TTL_MS, h->expiry_ms);
}

static void S26_AC1_no_coalesce_past_2001ms(void)
{
    ff_notify_t q;
    ff_notify_init(&q);
    ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 5, "hi", NOW);

    /* 2001ms later: past the window — a SECOND entry, not a coalesce. */
    TEST_ASSERT_TRUE(ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 5, "hi again", NOW + 2001u));
    TEST_ASSERT_EQUAL_UINT8(2, ff_notify_count(&q));

    ff_notify_entry_t const *h = ff_notify_head(&q);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_STRING("hi", h->text); /* the original entry, untouched */
}

/* A push whose kind differs must NOT coalesce, even from the same node
 * within the window — the proxy check: something that only varies
 * node_id could pass a bug that ignores kind entirely. */
static void S26_AC1_different_kind_does_not_coalesce(void)
{
    ff_notify_t q;
    ff_notify_init(&q);
    ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 5, "text", NOW);
    ff_notify_push(&q, FF_NOTIFY_RALLY, FF_NOTIFY_TIER_BANNER, 5, "rally", NOW + 10u);
    TEST_ASSERT_EQUAL_UINT8(2, ff_notify_count(&q));
}

/* A push whose node_id differs must NOT coalesce, even from the same
 * kind within the window. */
static void S26_AC1_different_node_does_not_coalesce(void)
{
    ff_notify_t q;
    ff_notify_init(&q);
    ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 5, "a", NOW);
    ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 6, "b", NOW + 10u);
    TEST_ASSERT_EQUAL_UINT8(2, ff_notify_count(&q));
}

/* Coalescing must not clobber a DIFFERENT, still-live entry's queue
 * position. */
static void S26_AC1_coalesce_preserves_other_entries_order(void)
{
    ff_notify_t q;
    ff_notify_init(&q);
    ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 1, "first", NOW);
    ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 2, "second", NOW + 10u);
    /* Coalesce into node 1. */
    ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 1, "first-updated", NOW + 20u);

    TEST_ASSERT_EQUAL_UINT8(2, ff_notify_count(&q));
    ff_notify_entry_t const *h0 = ff_notify_head(&q);
    TEST_ASSERT_EQUAL_UINT32(1, h0->node_id); /* still queue position 0 — not moved to the back */
    TEST_ASSERT_EQUAL_STRING("first-updated", h0->text);
}

/* A long text is safely truncated, never overrunning the buffer. */
static void S26_text_overlong_is_truncated_safely(void)
{
    ff_notify_t q;
    ff_notify_init(&q);
    char long_text[FF_NOTIFY_TEXT_MAX + 32];
    memset(long_text, 'x', sizeof(long_text) - 1u);
    long_text[sizeof(long_text) - 1u] = '\0';

    ff_notify_push(&q, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 1, long_text, NOW);
    ff_notify_entry_t const *h = ff_notify_head(&q);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_UINT32(FF_NOTIFY_TEXT_MAX - 1u, (uint32_t)strlen(h->text));
}

/* ------------------------------------------------------------------- */
/* NULL guards                                                          */
/* ------------------------------------------------------------------- */

static void S26_null_guards(void)
{
    ff_notify_init(NULL); /* no crash */
    TEST_ASSERT_EQUAL_UINT8(0, ff_notify_count(NULL));
    TEST_ASSERT_NULL(ff_notify_head(NULL));
    TEST_ASSERT_FALSE(ff_notify_pop(NULL));
    TEST_ASSERT_FALSE(ff_notify_dismiss(NULL, 0));
    ff_notify_tick(NULL, NOW); /* no crash */
    TEST_ASSERT_FALSE(ff_notify_push(NULL, FF_NOTIFY_MESSAGE, FF_NOTIFY_TIER_BANNER, 1, "x", NOW));
}

/* ------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S26_AC1_push_then_head_is_oldest);
    RUN_TEST(S26_AC1_fifo_pop_order);
    RUN_TEST(S26_AC1_overflow_drops_oldest);
    RUN_TEST(S26_AC1_banner_expiry_is_6000ms_after_at_ms);
    RUN_TEST(S26_AC1_tick_expires_only_the_due_entry);
    RUN_TEST(S26_takeover_tier_never_auto_expires);
    RUN_TEST(S26_AC1_dismiss_by_index_shifts_remainder);
    RUN_TEST(S26_AC1_dismiss_out_of_range_is_noop);
    RUN_TEST(S26_AC1_coalesce_within_2s_replaces_in_place);
    RUN_TEST(S26_AC1_no_coalesce_past_2001ms);
    RUN_TEST(S26_AC1_different_kind_does_not_coalesce);
    RUN_TEST(S26_AC1_different_node_does_not_coalesce);
    RUN_TEST(S26_AC1_coalesce_preserves_other_entries_order);
    RUN_TEST(S26_text_overlong_is_truncated_safely);
    RUN_TEST(S26_null_guards);

    return UNITY_END();
}
