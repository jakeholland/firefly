/**
 * test_time_reached.c — boundary tests for ff_time_reached
 * (firmware/platform/include/ff_clock.h).
 *
 * Tech-debt sprint: six core modules (ff_button.c, ff_power_fsm.c,
 * ff_idle.c, ff_notify.c, ff_flare.c, ff_demofeed.c) each used to define
 * an identical `static ... reached(now_ms, deadline_ms)` one-liner. This
 * pins the single shared replacement's documented contract: INCLUSIVE at
 * the boundary, and correct across uint32_t ms wraparound.
 *
 * Lives under core/tests (rather than platform/, which is a header-only
 * INTERFACE target with no Unity test scaffolding of its own) purely for
 * a place to put the executable; it links ff-platform directly (not
 * ff-core) since ff_time_reached is platform/'s own API.
 */
#include "unity.h"

#include "ff_clock.h"

void setUp(void) {}
void tearDown(void) {}

static void equal_is_reached(void)
{
    /* now_ms == deadline_ms: INCLUSIVE, already reached. */
    TEST_ASSERT_TRUE(ff_time_reached(1000u, 1000u));
    TEST_ASSERT_TRUE(ff_time_reached(0u, 0u));
}

static void one_before_is_not_reached(void)
{
    /* now_ms one ms short of deadline_ms: not yet reached. */
    TEST_ASSERT_FALSE(ff_time_reached(999u, 1000u));
}

static void one_after_is_reached(void)
{
    TEST_ASSERT_TRUE(ff_time_reached(1001u, 1000u));
}

static void reached_across_uint32_wrap(void)
{
    /* A deadline set just before the ~49.7-day uint32 ms rollover, and
     * "now" just after it: the true elapsed gap is small and positive,
     * so this must read as reached, not as some huge unreached distance
     * (which a naive now_ms < deadline_ms comparison would wrongly see). */
    uint32_t const deadline = 0xFFFFFFF0u; /* shortly before the wrap */
    uint32_t const now      = 0x00000010u; /* shortly after the wrap */
    TEST_ASSERT_TRUE(ff_time_reached(now, deadline));
}

static void not_yet_reached_across_uint32_wrap(void)
{
    /* Same wrap neighborhood, but "now" is still short of the deadline. */
    uint32_t const deadline = 0x00000020u; /* just after the wrap */
    uint32_t const now      = 0xFFFFFFF0u; /* just before the wrap: earlier */
    TEST_ASSERT_FALSE(ff_time_reached(now, deadline));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(equal_is_reached);
    RUN_TEST(one_before_is_not_reached);
    RUN_TEST(one_after_is_reached);
    RUN_TEST(reached_across_uint32_wrap);
    RUN_TEST(not_yet_reached_across_uint32_wrap);

    return UNITY_END();
}
