/**
 * test_nodeinfo_req.c — the rate-limit state behind "ask a nameless
 * crew member for their NodeInfo" (bench finding, 2026-09-14).
 *
 * Pure core: this file only exercises `ff_nodeinfo_req_should_send`'s
 * decision and memory. The "nameless admission asks once; a named
 * member never asks; the want_config replay never asks" behaviour is a
 * shell-level integration and is tested in
 * firmware/app/tests/test_shell.c.
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "ff_nodeinfo_req.h"

void setUp(void) {}
void tearDown(void) {}

static void nodeinfo_req_first_ask_is_always_due(void)
{
    ff_nodeinfo_req_t r;
    ff_nodeinfo_req_init(&r);
    TEST_ASSERT_TRUE(ff_nodeinfo_req_should_send(&r, 0x1001u, 1000u));
}

static void nodeinfo_req_second_ask_within_window_is_refused(void)
{
    ff_nodeinfo_req_t r;
    ff_nodeinfo_req_init(&r);
    uint32_t const t0 = 1000u;
    TEST_ASSERT_TRUE(ff_nodeinfo_req_should_send(&r, 0x1001u, t0));

    /* Anywhere inside the 10-minute window, including right up to (but
     * not touching) the boundary — a false return must touch no state. */
    TEST_ASSERT_FALSE(ff_nodeinfo_req_should_send(&r, 0x1001u, t0 + 1u));
    TEST_ASSERT_FALSE(ff_nodeinfo_req_should_send(&r, 0x1001u,
                                                    t0 + FF_NODEINFO_REQ_RATE_LIMIT_MS - 1u));
}

static void nodeinfo_req_due_again_at_the_boundary_and_after(void)
{
    ff_nodeinfo_req_t r;
    ff_nodeinfo_req_init(&r);
    uint32_t const t0 = 5000u;
    TEST_ASSERT_TRUE(ff_nodeinfo_req_should_send(&r, 0x2002u, t0));

    /* Exactly FF_NODEINFO_REQ_RATE_LIMIT_MS later is due again — the
     * comparison is age < limit refuses, so age == limit is due (same
     * inclusive-toward-"stale-is-over" convention as ff_crew_presence's
     * own boundary rule elsewhere in this tree). */
    uint32_t const t1 = t0 + FF_NODEINFO_REQ_RATE_LIMIT_MS;
    TEST_ASSERT_TRUE(ff_nodeinfo_req_should_send(&r, 0x2002u, t1));

    /* And the clock re-starts from the SECOND send, not the first. */
    TEST_ASSERT_FALSE(ff_nodeinfo_req_should_send(&r, 0x2002u, t1 + 1u));
    TEST_ASSERT_TRUE(ff_nodeinfo_req_should_send(&r, 0x2002u, t1 + FF_NODEINFO_REQ_RATE_LIMIT_MS));
}

static void nodeinfo_req_tracks_each_node_independently(void)
{
    ff_nodeinfo_req_t r;
    ff_nodeinfo_req_init(&r);
    TEST_ASSERT_TRUE(ff_nodeinfo_req_should_send(&r, 0x1001u, 1000u));
    /* A different node id at the same instant is unaffected by the
     * first node's just-recorded request. */
    TEST_ASSERT_TRUE(ff_nodeinfo_req_should_send(&r, 0x1002u, 1000u));
    TEST_ASSERT_FALSE(ff_nodeinfo_req_should_send(&r, 0x1001u, 1001u));
    TEST_ASSERT_FALSE(ff_nodeinfo_req_should_send(&r, 0x1002u, 1001u));
}

static void nodeinfo_req_wraparound_is_safe(void)
{
    /* Unsigned-subtraction age must stay correct across a uint32_t ms
     * wraparound, the same convention ff_heard_note/ff_crew's own age
     * math documents (~49.7 days of uptime). */
    ff_nodeinfo_req_t r;
    ff_nodeinfo_req_init(&r);
    uint32_t const near_wrap = 0xFFFFFFFFu - 100u;
    TEST_ASSERT_TRUE(ff_nodeinfo_req_should_send(&r, 0x3003u, near_wrap));
    /* 100 + 50 = 150ms after near_wrap, wrapping past UINT32_MAX — well
     * inside the 10-minute window, so still refused. */
    uint32_t const wrapped_soon = near_wrap + 150u; /* wraps */
    TEST_ASSERT_FALSE(ff_nodeinfo_req_should_send(&r, 0x3003u, wrapped_soon));
    /* A full window later (measured from near_wrap, wrapping) is due. */
    uint32_t const wrapped_due = near_wrap + FF_NODEINFO_REQ_RATE_LIMIT_MS;
    TEST_ASSERT_TRUE(ff_nodeinfo_req_should_send(&r, 0x3003u, wrapped_due));
}

static void nodeinfo_req_full_table_evicts_least_recently_requested(void)
{
    ff_nodeinfo_req_t r;
    ff_nodeinfo_req_init(&r);

    /* Fill every slot, each at a distinct, increasing timestamp so
     * there is exactly one least-recently-requested entry (node 0) at
     * every later point. */
    for (uint32_t i = 0; i < FF_NODEINFO_REQ_MAX; i++) {
        TEST_ASSERT_TRUE(ff_nodeinfo_req_should_send(&r, 0x9000u + i, 1000u + i));
    }

    /* A brand-new id, table full: evicts node 0 (oldest), not any other
     * occupant — and the eviction itself counts as "asked", so this
     * call is due. */
    uint32_t const now = 1000u + FF_NODEINFO_REQ_MAX;
    TEST_ASSERT_TRUE(ff_nodeinfo_req_should_send(&r, 0xAAAAu, now));

    /* Every OTHER original occupant is untouched by that eviction and
     * still inside its own window — checked first, and as a group,
     * because a FALSE return touches no state (a query that instead
     * returned TRUE would itself consume a slot and could evict one of
     * these, which is exactly the property this loop is checking for). */
    for (uint32_t i = 1; i < FF_NODEINFO_REQ_MAX; i++) {
        TEST_ASSERT_FALSE(ff_nodeinfo_req_should_send(&r, 0x9000u + i, now + 1u));
    }

    /* Node 0's slot was reused (by 0xAAAA above), so it reads as
     * never-asked — due immediately. Checked LAST: the table is still
     * full, so this call itself evicts whichever occupant is now
     * least-recently-requested, which would otherwise disturb the
     * group check above. */
    TEST_ASSERT_TRUE(ff_nodeinfo_req_should_send(&r, 0x9000u, now + 1u));
}

static void nodeinfo_req_null_and_zero_id_are_safe_and_touch_nothing(void)
{
    ff_nodeinfo_req_init(NULL); /* no crash */

    ff_nodeinfo_req_t r;
    ff_nodeinfo_req_init(&r);
    TEST_ASSERT_FALSE(ff_nodeinfo_req_should_send(NULL, 0x1234u, 1000u));
    /* 0 is never a valid Meshtastic node id (the wire protocol reserves
     * it as "unset") — must never be recorded. */
    TEST_ASSERT_FALSE(ff_nodeinfo_req_should_send(&r, 0u, 1000u));
    TEST_ASSERT_EQUAL_UINT8(0u, r.count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(nodeinfo_req_first_ask_is_always_due);
    RUN_TEST(nodeinfo_req_second_ask_within_window_is_refused);
    RUN_TEST(nodeinfo_req_due_again_at_the_boundary_and_after);
    RUN_TEST(nodeinfo_req_tracks_each_node_independently);
    RUN_TEST(nodeinfo_req_wraparound_is_safe);
    RUN_TEST(nodeinfo_req_full_table_evicts_least_recently_requested);
    RUN_TEST(nodeinfo_req_null_and_zero_id_are_safe_and_touch_nothing);
    return UNITY_END();
}
