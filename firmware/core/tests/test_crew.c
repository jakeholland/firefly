/**
 * test_crew.c — S02 core/crew acceptance criteria.
 *
 * Test names follow docs/specs/S02-core-crew.md's numbered acceptance
 * criteria: S02_ACn_description.
 *
 * AC8 (zero heap allocation) is enforced two ways: the compile-time
 * _Static_assert in ff_crew.h (struct-size sanity bound), and by
 * construction — nothing in ff_crew.c calls malloc/free (grep-able; there
 * is no <stdlib.h> include). valgrind isn't available on this dev
 * platform (macOS) to run a literal "valgrind-clean" pass locally; see the
 * PR body for that interpretation note.
 */
#include <string.h>

#include "unity.h"

#include "ff_crew.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* fake clock                                                           */
/* ------------------------------------------------------------------- */

typedef struct {
    uint32_t t;
} fake_clock_t;

static uint32_t fake_now(void *user)
{
    return ((fake_clock_t *)user)->t;
}

static ff_clock_t make_clock(fake_clock_t *fc)
{
    ff_clock_t clk;
    clk.now_ms = fake_now;
    clk.user = fc;
    return clk;
}

/* ------------------------------------------------------------------- */
/* AC1 — freshness transitions, boundary-inclusive at 45s and 600s      */
/* ------------------------------------------------------------------- */

static void S02_AC1_freshness_just_under_45s_is_live(void)
{
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.has_pos = true;
    m.pos_age_ms = 0;
    TEST_ASSERT_EQUAL(FF_FRESH_LIVE, ff_crew_freshness(&m, 44999u));
}

static void S02_AC1_freshness_exactly_45000ms_is_stale(void)
{
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.has_pos = true;
    m.pos_age_ms = 0;
    TEST_ASSERT_EQUAL(FF_FRESH_STALE, ff_crew_freshness(&m, 45000u));
}

static void S02_AC1_freshness_exactly_600000ms_is_stale(void)
{
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.has_pos = true;
    m.pos_age_ms = 0;
    TEST_ASSERT_EQUAL(FF_FRESH_STALE, ff_crew_freshness(&m, 600000u));
}

static void S02_AC1_freshness_just_over_600000ms_is_lost(void)
{
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.has_pos = true;
    m.pos_age_ms = 0;
    TEST_ASSERT_EQUAL(FF_FRESH_LOST, ff_crew_freshness(&m, 600001u));
}

static void S02_AC1_freshness_never_when_no_pos_ever(void)
{
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.has_pos = false;
    TEST_ASSERT_EQUAL(FF_FRESH_NEVER, ff_crew_freshness(&m, 999999u));
}

static void S02_AC1_freshness_just_under_600000ms_is_stale(void)
{
    /* Symmetric to S02_AC1_freshness_exactly_600000ms_is_stale: the STALE
     * side immediately below the LOST boundary, mirroring the
     * just-under-45s LIVE-side test above. */
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.has_pos = true;
    m.pos_age_ms = 0;
    TEST_ASSERT_EQUAL(FF_FRESH_STALE, ff_crew_freshness(&m, 599999u));
}

static void S02_AC1_freshness_handles_uint32_wraparound(void)
{
    /* pos_age_ms stores an absolute clock timestamp (see ff_crew.h's
     * header comment); ff_crew_freshness computes elapsed age as
     * `now_ms - m->pos_age_ms`, unsigned subtraction, which must stay
     * correct across a uint32_t rollover the same way ff_clock_t's own
     * documented convention promises.
     *
     * pos_age_ms = UINT32_MAX - 99 sits 100 ticks before the 0-rollover
     * (...UINT32_MAX-99, UINT32_MAX-98, ..., UINT32_MAX, 0, 1, ...);
     * now_ms = 100 is 100 ticks past the rollover. True elapsed time is
     * therefore 100 + 100 = 200ms - comfortably LIVE - not the ~4.29
     * billion ms a naive signed/unwrapped subtraction would produce. */
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.has_pos = true;
    m.pos_age_ms = UINT32_MAX - 99u;

    TEST_ASSERT_EQUAL(FF_FRESH_LIVE, ff_crew_freshness(&m, 100u));
}

/* ------------------------------------------------------------------- */
/* AC2 — upsert / no-eviction policy                                    */
/* ------------------------------------------------------------------- */

static void S02_AC2_upsert_existing_id_returns_same_slot(void)
{
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    ff_crew_member_t *p1 = ff_crew_upsert(&c, 42u);
    ff_crew_member_t *p2 = ff_crew_upsert(&c, 42u);
    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_TRUE(p1 == p2);
    TEST_ASSERT_EQUAL_UINT32(42u, p1->node_id);
}

static void S02_AC2_ninth_unpaired_member_rejected(void)
{
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    for (uint32_t i = 1; i <= FF_CREW_MAX; i++) {
        ff_crew_member_t *p = ff_crew_upsert(&c, i);
        TEST_ASSERT_NOT_NULL(p);
    }
    ff_crew_member_t *ninth = ff_crew_upsert(&c, 999u);
    TEST_ASSERT_NULL(ninth);
}

static void S02_AC2_ninth_rejected_even_when_a_slot_is_unpaired(void)
{
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    for (uint32_t i = 1; i <= FF_CREW_MAX; i++) {
        ff_crew_member_t *p = ff_crew_upsert(&c, i);
        TEST_ASSERT_NOT_NULL(p);
    }
    /* Slot 1 is explicitly unpaired (freeable-looking) - fixed policy:
     * still no eviction. */
    ff_crew_set_paired(&c, 1u, false);

    ff_crew_member_t *ninth = ff_crew_upsert(&c, 999u);
    TEST_ASSERT_NULL(ninth);
    /* And the "freeable" slot is untouched. */
    ff_crew_member_t *still_there = ff_crew_upsert(&c, 1u);
    TEST_ASSERT_NOT_NULL(still_there);
    TEST_ASSERT_EQUAL_UINT32(1u, still_there->node_id);
}

static void S02_AC2_set_paired_cannot_exceed_capacity(void)
{
    /* ff_crew_set_paired find-or-creates internally, same as upsert - the
     * no-eviction cap must hold on THIS path too, not just via
     * ff_crew_upsert. A capacity-bypass regression here would write past
     * the fixed-size `members[FF_CREW_MAX]` array (an out-of-bounds
     * write), so this also guards AC8's zero-heap/no-corruption story. */
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    for (uint32_t i = 1; i <= FF_CREW_MAX; i++) {
        ff_crew_member_t *p = ff_crew_upsert(&c, i);
        TEST_ASSERT_NOT_NULL(p);
    }
    TEST_ASSERT_EQUAL_UINT8(FF_CREW_MAX, c.count);

    uint32_t const novel_id = 999u;
    ff_crew_set_paired(&c, novel_id, true);

    /* No new slot was created ... */
    TEST_ASSERT_EQUAL_UINT8(FF_CREW_MAX, c.count);
    /* ... and nothing in the roster now claims the novel id. */
    for (uint8_t i = 0; i < c.count; i++) {
        TEST_ASSERT_NOT_EQUAL_UINT32(novel_id, c.members[i].node_id);
    }
    /* The 9 original members are untouched (no silent overwrite either). */
    for (uint32_t i = 1; i <= FF_CREW_MAX; i++) {
        ff_crew_member_t *m = ff_crew_upsert(&c, i);
        TEST_ASSERT_NOT_NULL(m);
        TEST_ASSERT_EQUAL_UINT32(i, m->node_id);
    }
}

/* ------------------------------------------------------------------- */
/* AC3 — on_position updates age from injected clock; NEVER->LIVE       */
/* ------------------------------------------------------------------- */

static void S02_AC3_on_position_first_fix_is_never_to_live(void)
{
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    ff_crew_member_t *m = ff_crew_upsert(&c, 7u);
    TEST_ASSERT_EQUAL(FF_FRESH_NEVER, ff_crew_freshness(m, 1000u));

    ff_latlon_t p = {39.9, -82.4};
    ff_crew_on_position(&c, 7u, p, 1000u, FF_CREW_POS_META_NONE);

    TEST_ASSERT_TRUE(m->has_pos);
    TEST_ASSERT_EQUAL(FF_FRESH_LIVE, ff_crew_freshness(m, 1000u));
}

static void S02_AC3_on_position_age_advances_with_now_ms(void)
{
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    ff_latlon_t p = {39.9, -82.4};
    ff_crew_member_t *m = ff_crew_upsert(&c, 7u);
    ff_crew_on_position(&c, 7u, p, 10000u, FF_CREW_POS_META_NONE);

    TEST_ASSERT_EQUAL(FF_FRESH_LIVE, ff_crew_freshness(m, 10000u));
    TEST_ASSERT_EQUAL(FF_FRESH_LIVE, ff_crew_freshness(m, 54999u));
    TEST_ASSERT_EQUAL(FF_FRESH_STALE, ff_crew_freshness(m, 55000u));
}

/* ------------------------------------------------------------------- */
/* AC4 — close-range 8-row truth table                                  */
/* ------------------------------------------------------------------- */

typedef struct {
    float distance_m;
    int16_t rssi_dbm;
    uint32_t rssi_age_ms; /* absolute timestamp of the sample */
    uint32_t now_ms;
    bool expect_close;
    char const *label;
} close_range_row_t;

static void run_close_range_row(close_range_row_t const *row)
{
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.rssi_dbm = row->rssi_dbm;
    m.rssi_age_ms = row->rssi_age_ms;

    bool got = ff_crew_close_range(&m, row->distance_m, row->now_ms);
    TEST_ASSERT_EQUAL_MESSAGE(row->expect_close, got, row->label);
}

static void S02_AC4_close_range_truth_table(void)
{
    /* now_ms fixed at 20000; rssi_age_ms chosen so (now - rssi_age_ms) is
     * either clearly < 10s (close) or clearly >= 10s (far). Distance is
     * either clearly < 30m or clearly >= 30m. Rssi is either clearly
     * > -60dBm or clearly <= -60dBm. All 8 combinations of the three
     * booleans. */
    close_range_row_t rows[] = {
        /* dist<30 | age<10s | rssi>-60 | expect */
        {10.0f, -50, 19000u, 20000u, true,  "near + fresh-strong -> close (near alone suffices)"},
        {10.0f, -50, 5000u,  20000u, true,  "near + stale-strong -> close (near alone suffices)"},
        {10.0f, -70, 19000u, 20000u, true,  "near + fresh-weak -> close (near alone suffices)"},
        {10.0f, -70, 5000u,  20000u, true,  "near + stale-weak -> close (near alone suffices)"},
        {100.0f, -50, 19000u, 20000u, true,  "far + fresh-strong -> close (radio leg suffices)"},
        {100.0f, -50, 5000u,  20000u, false, "far + stale-strong -> not close"},
        {100.0f, -70, 19000u, 20000u, false, "far + fresh-weak -> not close"},
        {100.0f, -70, 5000u,  20000u, false, "far + stale-weak -> not close"},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        run_close_range_row(&rows[i]);
    }
}

static void S02_AC4_close_range_boundary_distance_exclusive(void)
{
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.rssi_dbm = INT16_MIN; /* never direct - radio leg can't save it */
    m.rssi_age_ms = 0;

    TEST_ASSERT_FALSE(ff_crew_close_range(&m, 30.0f, 1000u));  /* == 30m: not close */
    TEST_ASSERT_TRUE(ff_crew_close_range(&m, 29.999f, 1000u)); /* just under: close */
}

static void S02_AC4_close_range_boundary_rssi_age_exclusive(void)
{
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.rssi_dbm = -50; /* strong */
    m.rssi_age_ms = 0;

    /* age == 10000ms: not < 10s -> not close (distance also far). */
    TEST_ASSERT_FALSE(ff_crew_close_range(&m, 100.0f, 10000u));
    /* age == 9999ms: < 10s -> close. */
    TEST_ASSERT_TRUE(ff_crew_close_range(&m, 100.0f, 9999u));
}

static void S02_AC4_close_range_boundary_rssi_value_exclusive(void)
{
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.rssi_age_ms = 0;

    m.rssi_dbm = -60; /* == -60: not > -60 -> not close */
    TEST_ASSERT_FALSE(ff_crew_close_range(&m, 100.0f, 1000u));

    m.rssi_dbm = -59; /* > -60 -> close (age is fresh) */
    TEST_ASSERT_TRUE(ff_crew_close_range(&m, 100.0f, 1000u));
}

static void S02_AC4_close_range_never_direct_sentinel_guard(void)
{
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.rssi_dbm = INT16_MIN; /* never had a direct packet */
    m.rssi_age_ms = 0;

    /* Even with now_ms == rssi_age_ms (age 0, "fresh"), the sentinel must
     * block the radio leg - a naive "age < 10s" check without the
     * sentinel guard would wrongly report close here. */
    TEST_ASSERT_FALSE(ff_crew_close_range(&m, 100.0f, 0u));
}

/* ------------------------------------------------------------------- */
/* AC5 — RSSI trend                                                     */
/* ------------------------------------------------------------------- */

static void feed_rssi_series(ff_crew_t *c, uint32_t node_id, fake_clock_t *fc,
                              uint32_t const *times_ms, int16_t const *values, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        fc->t = times_ms[i];
        ff_crew_on_rssi(c, node_id, values[i]);
    }
}

static void S02_AC5_rssi_trend_monotonic_rising_is_plus_one(void)
{
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    uint32_t times[] = {0u, 1000u, 2000u, 3000u, 4000u, 5000u};
    int16_t values[] = {-80, -78, -76, -74, -72, -70};
    feed_rssi_series(&c, 1u, &fc, times, values, 6);

    TEST_ASSERT_EQUAL_INT8(1, ff_crew_rssi_trend(&c, 1u, 5000u));
}

static void S02_AC5_rssi_trend_monotonic_falling_is_minus_one(void)
{
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    uint32_t times[] = {0u, 1000u, 2000u, 3000u, 4000u, 5000u};
    int16_t values[] = {-70, -72, -74, -76, -78, -80};
    feed_rssi_series(&c, 1u, &fc, times, values, 6);

    TEST_ASSERT_EQUAL_INT8(-1, ff_crew_rssi_trend(&c, 1u, 5000u));
}

static void S02_AC5_rssi_trend_flat_noisy_is_zero(void)
{
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    /* +-2dBm wobble, but the two window halves average out to exactly the
     * same value (-75) - a deliberately deterministic "noisy but flat"
     * fixture, not just "small numbers that happen to round to 0". */
    uint32_t times[] = {0u, 1000u, 2000u, 3000u, 4000u, 5000u};
    int16_t values[] = {-75, -77, -73, -73, -77, -75};
    feed_rssi_series(&c, 1u, &fc, times, values, 6);

    TEST_ASSERT_EQUAL_INT8(0, ff_crew_rssi_trend(&c, 1u, 5000u));
}

static void S02_AC5_rssi_trend_unknown_node_is_zero(void)
{
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    TEST_ASSERT_EQUAL_INT8(0, ff_crew_rssi_trend(&c, 12345u, 5000u));
}

static void S02_AC5_rssi_trend_single_sample_is_zero(void)
{
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    fc.t = 4000u;
    ff_crew_on_rssi(&c, 1u, -60);

    /* Only one sample -> it lands entirely in one half of the window;
     * the other half has zero samples, so "not enough data" applies. */
    TEST_ASSERT_EQUAL_INT8(0, ff_crew_rssi_trend(&c, 1u, 5000u));
}

/* ------------------------------------------------------------------- */
/* AC6 — distance formatting, exact strings, both unit systems          */
/* ------------------------------------------------------------------- */

typedef struct {
    float meters;
    char const *metric;
    char const *imperial;
} dist_row_t;

static void S02_AC6_distance_formatting_exact_strings(void)
{
    dist_row_t rows[] = {
        {5.0f,    "5 m",    "16 ft"},
        {999.0f,  "999 m",  "0.6 mi"},
        {1000.0f, "1.0 km", "0.6 mi"},
        {1049.0f, "1.0 km", "0.7 mi"},
        {1500.0f, "1.5 km", "0.9 mi"},
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        char buf[32];

        ff_fmt_distance(buf, sizeof(buf), rows[i].meters, false);
        TEST_ASSERT_EQUAL_STRING(rows[i].metric, buf);

        ff_fmt_distance(buf, sizeof(buf), rows[i].meters, true);
        TEST_ASSERT_EQUAL_STRING(rows[i].imperial, buf);
    }
}

static void S02_AC6_distance_formatting_1km_boundary_is_exclusive_of_m(void)
{
    char buf[32];
    ff_fmt_distance(buf, sizeof(buf), 999.9f, false);
    TEST_ASSERT_EQUAL_STRING("1000 m", buf); /* still under 1000.0f -> m branch, rounds to 1000 */

    ff_fmt_distance(buf, sizeof(buf), 1000.0f, false);
    TEST_ASSERT_EQUAL_STRING("1.0 km", buf); /* exactly 1000 -> km branch, not "1000 m" */
}

static void S02_AC6_distance_formatting_1000ft_boundary_is_exclusive_of_ft(void)
{
    /* Imperial analogue of the 1km boundary test above: the breakpoint is
     * 1000ft, which is 304.8m - but ff_fmt_distance's boundary check
     * operates on the float32 *feet* value (meters / 0.3048f), not on
     * meters directly, and 304.8f/0.3048f rounds down to 999.99994ft in
     * float32 (verified: it does NOT cross the boundary). So this test
     * deliberately does not use the mathematically "clean" 304.8m value
     * for the over-the-line case - it uses 304.81m, which reliably
     * computes to just over 1000ft in float32. A future refactor "simplifying"
     * this to 304.8m would silently flip that row back into the ft branch. */
    char buf[32];

    ff_fmt_distance(buf, sizeof(buf), 304.76952f, true); /* ~999.9 ft */
    TEST_ASSERT_EQUAL_STRING("1000 ft", buf); /* still under 1000.0f ft -> ft branch, rounds to 1000 */

    ff_fmt_distance(buf, sizeof(buf), 304.81f, true); /* ~1000.03 ft */
    TEST_ASSERT_EQUAL_STRING("0.2 mi", buf); /* over 1000ft -> mi branch, not "1000 ft" */
}

/* ------------------------------------------------------------------- */
/* AC7 — age formatting                                                 */
/* ------------------------------------------------------------------- */

static void S02_AC7_age_formatting_exact_strings(void)
{
    char buf[32];

    /* Under a minute reads the steady "now" (honest "less than a minute
     * ago"), never a per-second counter — see ff_fmt_age. */
    ff_fmt_age(buf, sizeof(buf), 8000u);
    TEST_ASSERT_EQUAL_STRING("now", buf);

    ff_fmt_age(buf, sizeof(buf), 45000u);
    TEST_ASSERT_EQUAL_STRING("now", buf);

    ff_fmt_age(buf, sizeof(buf), 59u * 60u * 1000u);
    TEST_ASSERT_EQUAL_STRING("59 MIN", buf);

    ff_fmt_age(buf, sizeof(buf), 61u * 60u * 1000u);
    TEST_ASSERT_EQUAL_STRING("1 HR", buf);
}

static void S02_AC7_age_formatting_60s_boundary_rolls_to_minutes(void)
{
    char buf[32];
    /* The under-a-minute boundary: 59s is still "now"; 60s is the first
     * minute ("1 MIN", never "60 SEC" / never a lingering "now"). */
    ff_fmt_age(buf, sizeof(buf), 59000u);
    TEST_ASSERT_EQUAL_STRING("now", buf);

    ff_fmt_age(buf, sizeof(buf), 59999u);
    TEST_ASSERT_EQUAL_STRING("now", buf);

    ff_fmt_age(buf, sizeof(buf), 60000u);
    TEST_ASSERT_EQUAL_STRING("1 MIN", buf); /* not "60 SEC", not "now" */
}

static void S02_AC7_age_formatting_60min_boundary_rolls_to_hours(void)
{
    char buf[32];
    ff_fmt_age(buf, sizeof(buf), 3599000u);
    TEST_ASSERT_EQUAL_STRING("59 MIN", buf);

    ff_fmt_age(buf, sizeof(buf), 3600000u);
    TEST_ASSERT_EQUAL_STRING("1 HR", buf); /* not "60 MIN" */
}

/* ------------------------------------------------------------------- */
/* AC8 — zero heap allocation                                           */
/* ------------------------------------------------------------------- */

static void S02_AC8_crew_roster_lives_entirely_on_the_stack(void)
{
    /* If ff_crew_t needed heap storage, this would need a matching
     * free()/destroy() - there is none, and this whole roster + its RSSI
     * history for all 8 slots fits on the stack. Exercise it end to end
     * to prove the type is genuinely usable without any allocator. */
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c; /* stack-allocated, no ff_crew_alloc/create anywhere */
    ff_crew_init(&c, &clk);

    for (uint32_t i = 1; i <= FF_CREW_MAX; i++) {
        ff_crew_member_t *m = ff_crew_upsert(&c, i);
        TEST_ASSERT_NOT_NULL(m);
        ff_crew_set_paired(&c, i, true);
        fc.t = i * 100u;
        ff_crew_on_rssi(&c, i, (int16_t)(-40 - (int)i));
    }
    TEST_ASSERT_EQUAL(FF_CREW_MAX, c.count);
}

/* ------------------------------------------------------------------- */
/* Slice d — selection cycling                                          */
/* ------------------------------------------------------------------- */

static void S02_selection_skips_unpaired_and_wraps(void)
{
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    ff_crew_upsert(&c, 1u);
    ff_crew_upsert(&c, 2u);
    ff_crew_upsert(&c, 3u);
    ff_crew_set_paired(&c, 1u, true);
    ff_crew_set_paired(&c, 2u, false); /* heard, not crew - must be skipped */
    ff_crew_set_paired(&c, 3u, true);

    ff_crew_member_t *sel = ff_crew_selected(&c);
    TEST_ASSERT_NOT_NULL(sel);
    TEST_ASSERT_EQUAL_UINT32(1u, sel->node_id);

    ff_crew_select_next(&c);
    sel = ff_crew_selected(&c);
    TEST_ASSERT_EQUAL_UINT32(3u, sel->node_id); /* skipped node 2 */

    ff_crew_select_next(&c);
    sel = ff_crew_selected(&c);
    TEST_ASSERT_EQUAL_UINT32(1u, sel->node_id); /* wrapped back to node 1 */
}

static void S02_selection_none_paired_returns_null(void)
{
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    ff_crew_upsert(&c, 1u);
    ff_crew_upsert(&c, 2u);
    /* neither paired */

    TEST_ASSERT_NULL(ff_crew_selected(&c));
    ff_crew_select_next(&c); /* must not crash */
    TEST_ASSERT_NULL(ff_crew_selected(&c));
}

static void S02_selection_single_paired_member_wraps_to_itself(void)
{
    /* With exactly one paired member, "next" has nowhere else to go and
     * must land back on the same member, not NULL or a crash. */
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    ff_crew_upsert(&c, 1u);
    ff_crew_upsert(&c, 2u); /* present but unpaired - must stay skipped */
    ff_crew_set_paired(&c, 1u, true);

    ff_crew_member_t *sel = ff_crew_selected(&c);
    TEST_ASSERT_NOT_NULL(sel);
    TEST_ASSERT_EQUAL_UINT32(1u, sel->node_id);

    ff_crew_select_next(&c);
    sel = ff_crew_selected(&c);
    TEST_ASSERT_NOT_NULL(sel);
    TEST_ASSERT_EQUAL_UINT32(1u, sel->node_id);

    /* Repeated calls stay stable too. */
    ff_crew_select_next(&c);
    sel = ff_crew_selected(&c);
    TEST_ASSERT_EQUAL_UINT32(1u, sel->node_id);
}

static void S02_selection_survives_member_disappearing(void)
{
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    ff_crew_upsert(&c, 1u);
    ff_crew_upsert(&c, 2u);
    ff_crew_set_paired(&c, 1u, true);
    ff_crew_set_paired(&c, 2u, true);

    ff_crew_member_t *sel = ff_crew_selected(&c);
    TEST_ASSERT_EQUAL_UINT32(1u, sel->node_id);

    /* node 1 "disappears" (unpaired) while selected */
    ff_crew_set_paired(&c, 1u, false);

    sel = ff_crew_selected(&c);
    TEST_ASSERT_NOT_NULL(sel);
    TEST_ASSERT_EQUAL_UINT32(2u, sel->node_id); /* self-healed to the only paired member left */
}

static void S02_selection_survives_member_appearing(void)
{
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    ff_crew_upsert(&c, 1u);
    ff_crew_set_paired(&c, 1u, true);

    ff_crew_member_t *sel = ff_crew_selected(&c);
    TEST_ASSERT_EQUAL_UINT32(1u, sel->node_id);

    /* node 2 appears and pairs mid-cycle */
    ff_crew_upsert(&c, 2u);
    ff_crew_set_paired(&c, 2u, true);

    ff_crew_select_next(&c);
    sel = ff_crew_selected(&c);
    TEST_ASSERT_EQUAL_UINT32(2u, sel->node_id);
}

/* ------------------------------------------------------------------- */
/* ff_crew_find — read-only lookup (S08 PR #25 code review, MEDIUM       */
/* finding: distinguishes this from ff_crew_upsert's find-or-CREATE).   */
/* ------------------------------------------------------------------- */

static void S02_find_returns_existing_paired_member(void)
{
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    ff_crew_upsert(&c, 42u);
    ff_crew_set_paired(&c, 42u, true);

    ff_crew_member_t const *m = ff_crew_find(&c, 42u);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_UINT32(42u, m->node_id);
    TEST_ASSERT_TRUE(m->paired);
}

static void S02_find_returns_existing_unpaired_member(void)
{
    /* "merely heard" slots are found too — ff_crew_find reports
     * existence/pairing state, it doesn't filter on paired. */
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    ff_crew_upsert(&c, 7u); /* never paired */

    ff_crew_member_t const *m = ff_crew_find(&c, 7u);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_FALSE(m->paired);
}

static void S02_find_unknown_id_returns_null(void)
{
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    ff_crew_upsert(&c, 1u);

    TEST_ASSERT_NULL(ff_crew_find(&c, 999u));
}

static void S02_find_never_creates_a_slot(void)
{
    /* The whole point: unlike ff_crew_upsert, a lookup miss must NOT
     * grow c->count or occupy a slot — mutation-check: fill the roster
     * to FF_CREW_MAX-1, then find() an unknown id FF_CREW_MAX times;
     * count must never move, and a genuinely new node must still be
     * upsert-able afterward (the roster-exhaustion bug this function
     * exists to prevent, from the OTHER direction: proving find() itself
     * carries no slot cost, not just that ff_wiring.c stopped calling
     * upsert). */
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    for (uint32_t i = 0; i < FF_CREW_MAX - 1; i++) {
        ff_crew_upsert(&c, 100u + i);
    }
    uint8_t count_before = c.count;
    TEST_ASSERT_EQUAL_UINT8(FF_CREW_MAX - 1, count_before);

    for (int i = 0; i < 50; i++) {
        ff_crew_member_t const *m = ff_crew_find(&c, 999000u + (uint32_t)i);
        TEST_ASSERT_NULL(m);
    }
    TEST_ASSERT_EQUAL_UINT8(count_before, c.count); /* untouched by 50 misses */

    /* One slot was always left free — a real new node can still claim it. */
    ff_crew_member_t *fresh = ff_crew_upsert(&c, 5555u);
    TEST_ASSERT_NOT_NULL(fresh);
    TEST_ASSERT_EQUAL_UINT8(FF_CREW_MAX, c.count);
}

static void S02_find_null_crew_is_safe(void)
{
    TEST_ASSERT_NULL(ff_crew_find(NULL, 1u));
}

/* ------------------------------------------------------------------- */
/* issue #33 — asserted positions never ride the freshness axis         */
/* ------------------------------------------------------------------- */

static void S33_asserted_fix_is_never_live(void)
{
    ff_crew_t c;
    ff_crew_init(&c, NULL);
    ff_crew_pos_meta_t meta = {.asserted = true, .has_precision_bits = false, .precision_bits = 0};
    ff_crew_on_position(&c, 1u, (ff_latlon_t){39.9, -82.4}, 1000u, meta);
    ff_crew_member_t const *m = ff_crew_find(&c, 1u);

    /* age 0 at the instant of the fix — the exact case that lands on LIVE
     * for a measured position (S02_AC3_on_position_first_fix_is_never_to_live).
     * The proxy this pins against: a broken implementation that only
     * special-cases "old" asserted fixes (age > some threshold) would still
     * pass a test that only checked an aged reading — this checks age 0. */
    TEST_ASSERT_EQUAL(FF_FRESH_ASSERTED, ff_crew_freshness(m, 1000u));
}

/* Mutation-conscious: the whole point of #33 is that elapsed time must
 * NEVER move an asserted fix off ASSERTED — not into STALE, not into
 * LOST, no matter how large `now_ms - pos_age_ms` grows. Checked at both
 * named boundaries (S02's own 45s/600s thresholds) plus a value far past
 * LOST, so a mutant that deletes the `pos_asserted` early-return (letting
 * the age math underneath run unconditionally) fails at every one of
 * them, not just one. */
static void S33_asserted_fix_never_ages_into_stale_or_lost(void)
{
    ff_crew_t c;
    ff_crew_init(&c, NULL);
    ff_crew_pos_meta_t meta = {.asserted = true, .has_precision_bits = false, .precision_bits = 0};
    ff_crew_on_position(&c, 1u, (ff_latlon_t){39.9, -82.4}, 0u, meta);
    ff_crew_member_t const *m = ff_crew_find(&c, 1u);

    TEST_ASSERT_EQUAL(FF_FRESH_ASSERTED, ff_crew_freshness(m, 0u));
    TEST_ASSERT_EQUAL(FF_FRESH_ASSERTED, ff_crew_freshness(m, FF_CREW_LIVE_MS));       /* the LIVE->STALE boundary */
    TEST_ASSERT_EQUAL(FF_FRESH_ASSERTED, ff_crew_freshness(m, FF_CREW_LOST_MS));       /* the STALE->LOST boundary */
    TEST_ASSERT_EQUAL(FF_FRESH_ASSERTED, ff_crew_freshness(m, FF_CREW_LOST_MS * 100)); /* absurdly old */
}

/* A measured fix (asserted == false) is completely unaffected — this is
 * the regression guard for the new early-return: it must be gated on
 * `pos_asserted`, not unconditional. */
static void S33_unasserted_fix_still_ages_normally(void)
{
    ff_crew_t c;
    ff_crew_init(&c, NULL);
    ff_crew_on_position(&c, 1u, (ff_latlon_t){39.9, -82.4}, 0u, FF_CREW_POS_META_NONE);
    ff_crew_member_t const *m = ff_crew_find(&c, 1u);

    TEST_ASSERT_EQUAL(FF_FRESH_LIVE, ff_crew_freshness(m, 0u));
    TEST_ASSERT_EQUAL(FF_FRESH_STALE, ff_crew_freshness(m, FF_CREW_LIVE_MS));
    TEST_ASSERT_EQUAL(FF_FRESH_LOST, ff_crew_freshness(m, FF_CREW_LOST_MS + 1u));
}

/* A later MEASURED fix must overwrite an earlier ASSERTED one (and vice
 * versa) — meta is not sticky. Pins the "whole-fix overwrite" contract
 * ff_crew_on_position's doc comment states explicitly. */
static void S33_newer_fix_overwrites_asserted_flag_in_both_directions(void)
{
    ff_crew_t c;
    ff_crew_init(&c, NULL);
    ff_crew_pos_meta_t asserted = {.asserted = true, .has_precision_bits = false, .precision_bits = 0};

    ff_crew_on_position(&c, 1u, (ff_latlon_t){1.0, 1.0}, 0u, asserted);
    ff_crew_member_t const *m = ff_crew_find(&c, 1u);
    TEST_ASSERT_EQUAL(FF_FRESH_ASSERTED, ff_crew_freshness(m, 0u));

    /* A real GPS fix arrives later for the same node id (e.g. a landmark
     * decommissioned and its slot reused, or simply a bug on the sender's
     * side) — asserted must clear, not linger. */
    ff_crew_on_position(&c, 1u, (ff_latlon_t){1.0, 1.0}, 1000u, FF_CREW_POS_META_NONE);
    TEST_ASSERT_EQUAL(FF_FRESH_LIVE, ff_crew_freshness(m, 1000u));

    /* And back the other way. */
    ff_crew_on_position(&c, 1u, (ff_latlon_t){1.0, 1.0}, 2000u, asserted);
    TEST_ASSERT_EQUAL(FF_FRESH_ASSERTED, ff_crew_freshness(m, 2000u));
}

/* ------------------------------------------------------------------- */
/* issue #47 — precision grid formula + threshold                       */
/* ------------------------------------------------------------------- */

/* Named values transcribed from mc_client.h's own worked examples and
 * issue #47's hardware measurement (13 bits on the default public
 * channel), so this test doubles as a regression guard on that doc
 * comment's own math, not just this function's implementation. */
static void S47_precision_grid_matches_documented_examples(void)
{
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 5836.0f, ff_crew_pos_precision_grid_m(13));
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 730.0f, ff_crew_pos_precision_grid_m(16));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 2.9f, ff_crew_pos_precision_grid_m(24));
    /* bits=32 is "untruncated" in the practical sense (no channel
     * quantization applied), but the formula's own cell size at the full
     * bit width is 2^32>>32 = 1 raw unit, i.e. one 1e-7-degree LSB of the
     * fixed-point coordinate (~1.1 cm) — not literally 0. */
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.011132f, ff_crew_pos_precision_grid_m(32));
}

static void S47_precision_grid_out_of_range_bits_is_zero(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ff_crew_pos_precision_grid_m(0));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ff_crew_pos_precision_grid_m(33));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ff_crew_pos_precision_grid_m(255));
}

/* The FF_CREW_POS_PRECISION_MIN_BITS threshold boundary, from both sides:
 * one bit below is degraded (grid exceeds close range), one bit at/above
 * is precise (grid comfortably under it). This is the exact row a
 * fencepost mutant (< vs <=) would flip. */
static void S47_precision_threshold_boundary(void)
{
    float const grid_below = ff_crew_pos_precision_grid_m(FF_CREW_POS_PRECISION_MIN_BITS - 1u);
    float const grid_at = ff_crew_pos_precision_grid_m(FF_CREW_POS_PRECISION_MIN_BITS);

    TEST_ASSERT_TRUE_MESSAGE(grid_below > FF_CREW_CLOSE_RANGE_M,
                              "one bit below the threshold should exceed close range");
    TEST_ASSERT_TRUE_MESSAGE(grid_at <= FF_CREW_CLOSE_RANGE_M,
                              "the threshold's own bit count should be at/under close range");
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S02_AC1_freshness_just_under_45s_is_live);
    RUN_TEST(S02_AC1_freshness_exactly_45000ms_is_stale);
    RUN_TEST(S02_AC1_freshness_exactly_600000ms_is_stale);
    RUN_TEST(S02_AC1_freshness_just_over_600000ms_is_lost);
    RUN_TEST(S02_AC1_freshness_never_when_no_pos_ever);
    RUN_TEST(S02_AC1_freshness_just_under_600000ms_is_stale);
    RUN_TEST(S02_AC1_freshness_handles_uint32_wraparound);

    RUN_TEST(S02_AC2_upsert_existing_id_returns_same_slot);
    RUN_TEST(S02_AC2_ninth_unpaired_member_rejected);
    RUN_TEST(S02_AC2_ninth_rejected_even_when_a_slot_is_unpaired);
    RUN_TEST(S02_AC2_set_paired_cannot_exceed_capacity);

    RUN_TEST(S02_AC3_on_position_first_fix_is_never_to_live);
    RUN_TEST(S02_AC3_on_position_age_advances_with_now_ms);

    RUN_TEST(S02_AC4_close_range_truth_table);
    RUN_TEST(S02_AC4_close_range_boundary_distance_exclusive);
    RUN_TEST(S02_AC4_close_range_boundary_rssi_age_exclusive);
    RUN_TEST(S02_AC4_close_range_boundary_rssi_value_exclusive);
    RUN_TEST(S02_AC4_close_range_never_direct_sentinel_guard);

    RUN_TEST(S02_AC5_rssi_trend_monotonic_rising_is_plus_one);
    RUN_TEST(S02_AC5_rssi_trend_monotonic_falling_is_minus_one);
    RUN_TEST(S02_AC5_rssi_trend_flat_noisy_is_zero);
    RUN_TEST(S02_AC5_rssi_trend_unknown_node_is_zero);
    RUN_TEST(S02_AC5_rssi_trend_single_sample_is_zero);

    RUN_TEST(S02_AC6_distance_formatting_exact_strings);
    RUN_TEST(S02_AC6_distance_formatting_1km_boundary_is_exclusive_of_m);
    RUN_TEST(S02_AC6_distance_formatting_1000ft_boundary_is_exclusive_of_ft);

    RUN_TEST(S02_AC7_age_formatting_exact_strings);
    RUN_TEST(S02_AC7_age_formatting_60s_boundary_rolls_to_minutes);
    RUN_TEST(S02_AC7_age_formatting_60min_boundary_rolls_to_hours);

    RUN_TEST(S02_AC8_crew_roster_lives_entirely_on_the_stack);

    RUN_TEST(S02_selection_skips_unpaired_and_wraps);
    RUN_TEST(S02_selection_none_paired_returns_null);
    RUN_TEST(S02_selection_survives_member_disappearing);
    RUN_TEST(S02_selection_survives_member_appearing);
    RUN_TEST(S02_selection_single_paired_member_wraps_to_itself);

    RUN_TEST(S02_find_returns_existing_paired_member);
    RUN_TEST(S02_find_returns_existing_unpaired_member);
    RUN_TEST(S02_find_unknown_id_returns_null);
    RUN_TEST(S02_find_never_creates_a_slot);
    RUN_TEST(S02_find_null_crew_is_safe);

    RUN_TEST(S33_asserted_fix_is_never_live);
    RUN_TEST(S33_asserted_fix_never_ages_into_stale_or_lost);
    RUN_TEST(S33_unasserted_fix_still_ages_normally);
    RUN_TEST(S33_newer_fix_overwrites_asserted_flag_in_both_directions);

    RUN_TEST(S47_precision_grid_matches_documented_examples);
    RUN_TEST(S47_precision_grid_out_of_range_bits_is_zero);
    RUN_TEST(S47_precision_threshold_boundary);

    return UNITY_END();
}
