/**
 * test_sched.c — S07 slices (a) engine + (c) alarm, criteria-numbered
 * per docs/specs/S07-now-face.md.
 *
 *   AC1 — now_playing: concurrent/finished/future filtering, exact
 *         pct_done at both boundaries.
 *   AC2 — midnight-crossing set attribution + math with now_min > 1440.
 *   AC3 — next_starred: earliest future starred set, false cases.
 *   AC4 — alarm: fires once at T-15 crossing, idempotent, ordered.
 *   AC5 — all-null-times pack: now/next empty/false, TBD flagged. The
 *         last AC5 case parses the real vendored Lost Lands 2026
 *         fixture via fp_parse (firmware/festpack/tests/fixtures/) —
 *         see the CMakeLists.txt S07 section for why that lives in this
 *         same executable rather than a separate test_sched_integration.
 *   AC6 (golden now_live.json/now_tbd.json screenshots) is UI (slice b)
 *         — out of scope for this engine-only PR.
 *
 * Fixtures for AC1-4 are in-code fp_pack_t literals built with mk_set()
 * below (per AGENTS.md: don't parse JSON in unit tests except the one
 * deliberate integration case).
 *
 * Also covers ff_sched_toggle_star and ff_sched_day_sets/day_tbd, which
 * the spec's numbered criteria don't call out individually but are part
 * of this PR's scope (see S07-now-face.md's Interface + Slices).
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fp_pack.h"
#include "ff_sched.h"

#ifndef FP_FIXTURE_DIR
#define FP_FIXTURE_DIR "./"
#endif

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------------------
 * Fixture helpers
 * ------------------------------------------------------------------- */

#define DAY_A 100u /* an arbitrary festival day_doy used across most cases */
#define DAY_B 101u /* a second day, used to prove day-scoping */

static fp_set_t mk_set(char const *artist, int8_t stage_idx, uint16_t day_doy,
                        int16_t start_min, int16_t end_min, bool starred)
{
    fp_set_t s;
    memset(&s, 0, sizeof(s));
    strncpy(s.artist, artist, sizeof(s.artist) - 1);
    s.stage_idx = stage_idx;
    s.day_doy = day_doy;
    s.start_min = start_min;
    s.end_min = end_min;
    s.starred = starred;
    return s;
}

static void pack_init(fp_pack_t *p)
{
    memset(p, 0, sizeof(*p));
}

static void pack_add(fp_pack_t *p, fp_set_t s)
{
    TEST_ASSERT_TRUE_MESSAGE(p->n_sets < FP_MAX_SETS, "test fixture overflowed FP_MAX_SETS");
    p->sets[p->n_sets++] = s;
}

/* ======================================================================
 * AC1 — now_playing: concurrent/finished/future filtering + pct_done
 * ==================================================================== */

static void S07_AC1_now_playing_returns_exactly_the_concurrent_sets(void)
{
    fp_pack_t p;
    pack_init(&p);
    /* 3 concurrent sets (different stages), overlapping [900,960) at now=930 */
    pack_add(&p, mk_set("Concurrent A", 0, DAY_A, 900, 960, false));
    pack_add(&p, mk_set("Concurrent B", 1, DAY_A, 870, 990, false));
    pack_add(&p, mk_set("Concurrent C", 2, DAY_A, 920, 940, false));
    /* 1 finished (ended before now) */
    pack_add(&p, mk_set("Finished", 3, DAY_A, 700, 800, false));
    /* 1 future (hasn't started) */
    pack_add(&p, mk_set("Future", 4, DAY_A, 1000, 1060, false));

    ff_now_row_t rows[8];
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 930, rows, 8);
    TEST_ASSERT_EQUAL_UINT8(3, n);
    TEST_ASSERT_EQUAL_STRING("Concurrent A", rows[0].set->artist);
    TEST_ASSERT_EQUAL_STRING("Concurrent B", rows[1].set->artist);
    TEST_ASSERT_EQUAL_STRING("Concurrent C", rows[2].set->artist);
}

static void S07_AC1_pct_done_boundary_start_is_zero(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Opener", 0, DAY_A, 600, 660, false));

    ff_now_row_t rows[4];
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 600, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_UINT8(0, rows[0].pct_done);
    TEST_ASSERT_EQUAL_INT16(60, rows[0].mins_left);
}

static void S07_AC1_pct_done_boundary_end_is_hundred(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Closer", 0, DAY_A, 600, 660, false));

    ff_now_row_t rows[4];
    /* Exactly the last minute: inclusive end per ff_sched.h's "now window". */
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 660, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_UINT8(100, rows[0].pct_done);
    TEST_ASSERT_EQUAL_INT16(0, rows[0].mins_left);

    /* One minute past the end: no longer "now". */
    n = ff_sched_now_playing(&p, DAY_A, 661, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(0, n);
}

static void S07_AC1_pct_done_midpoint(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Half", 0, DAY_A, 600, 700, false)); /* 100 min set */

    ff_now_row_t rows[4];
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 650, rows, 4); /* 50/100 */
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_UINT8(50, rows[0].pct_done);
    TEST_ASSERT_EQUAL_INT16(50, rows[0].mins_left);
}

static void S07_AC1_null_time_sets_never_appear(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("TBD Start", 0, DAY_A, -1, 660, false));
    pack_add(&p, mk_set("TBD End", 1, DAY_A, 600, -1, false));
    pack_add(&p, mk_set("All TBD", 2, DAY_A, -1, -1, false));

    ff_now_row_t rows[8];
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 620, rows, 8);
    TEST_ASSERT_EQUAL_UINT8(0, n);
}

static void S07_AC1_max_cap_truncates_in_pack_order(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("First", 0, DAY_A, 600, 700, false));
    pack_add(&p, mk_set("Second", 1, DAY_A, 600, 700, false));
    pack_add(&p, mk_set("Third", 2, DAY_A, 600, 700, false));

    ff_now_row_t rows[2];
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 650, rows, 2);
    TEST_ASSERT_EQUAL_UINT8(2, n);
    TEST_ASSERT_EQUAL_STRING("First", rows[0].set->artist);
    TEST_ASSERT_EQUAL_STRING("Second", rows[1].set->artist);
}

static void S07_AC1_day_scoping_excludes_other_days(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Day A set", 0, DAY_A, 600, 700, false));
    pack_add(&p, mk_set("Day B set", 0, DAY_B, 600, 700, false));

    ff_now_row_t rows[4];
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 650, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_STRING("Day A set", rows[0].set->artist);
}

/* ======================================================================
 * AC2 — midnight-crossing set attribution + now_min > 1440
 * ==================================================================== */

static void S07_AC2_midnight_crossing_set_is_now_at_0030(void)
{
    fp_pack_t p;
    pack_init(&p);
    /* 23:30 -> 01:00, stored start_min=1410, end_min=60 (end < start) */
    pack_add(&p, mk_set("Late Night B2B", 0, DAY_A, 1410, 60, false));

    /* 00:30 the following calendar morning, still DAY_A's festival day:
     * now_min = 30 + 1440 = 1470. */
    ff_now_row_t rows[4];
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 1470, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_STRING("Late Night B2B", rows[0].set->artist);
    TEST_ASSERT_EQUAL_UINT8(DAY_A, rows[0].set->day_doy); /* attributed to the start day */
}

static void S07_AC2_midnight_crossing_pct_done_across_midnight(void)
{
    fp_pack_t p;
    pack_init(&p);
    /* 23:30 -> 01:00 = 90-minute set, effective_end = 60 + 1440 = 1500 */
    pack_add(&p, mk_set("Late Night B2B", 0, DAY_A, 1410, 60, false));

    ff_now_row_t rows[4];

    /* Start instant: 23:30, pct 0. */
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 1410, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_UINT8(0, rows[0].pct_done);

    /* Two-thirds through: 23:30 + 60min = 00:30, 60/90 = 66%. */
    n = ff_sched_now_playing(&p, DAY_A, 1470, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_UINT8(66, rows[0].pct_done);

    /* End instant: 01:00, now_min = 1500, pct 100 (inclusive). */
    n = ff_sched_now_playing(&p, DAY_A, 1500, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_UINT8(100, rows[0].pct_done);
    TEST_ASSERT_EQUAL_INT16(0, rows[0].mins_left);

    /* One minute past: gone. */
    n = ff_sched_now_playing(&p, DAY_A, 1501, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(0, n);
}

static void S07_AC2_midnight_crossing_set_not_visible_on_next_day_doy(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Late Night B2B", 0, DAY_A, 1410, 60, false));

    /* Even at a numerically "early morning" now_min, querying the NEXT
     * festival day (DAY_A + 1) must not surface a set attributed to
     * DAY_A — day attribution is who owns it, not raw clock overlap. */
    ff_now_row_t rows[4];
    uint8_t n = ff_sched_now_playing(&p, DAY_A + 1, 30, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(0, n);
}

static void S07_AC2_0600_boundary_both_sides(void)
{
    fp_pack_t p;
    pack_init(&p);
    /* A set starting right at the festival-day-roll instant, 06:00. */
    pack_add(&p, mk_set("Sunrise Set", 0, DAY_A,
                         FF_SCHED_FESTIVAL_DAY_START_MIN,
                         FF_SCHED_FESTIVAL_DAY_START_MIN + 60, false));

    ff_now_row_t rows[4];
    /* One minute before 06:00: not started yet. */
    uint8_t n = ff_sched_now_playing(&p, DAY_A, FF_SCHED_FESTIVAL_DAY_START_MIN - 1, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(0, n);

    /* Exactly 06:00: started, pct 0 — no special-casing needed at the
     * boundary itself, same start_min<=now_min<=end comparison as any
     * other minute. */
    n = ff_sched_now_playing(&p, DAY_A, FF_SCHED_FESTIVAL_DAY_START_MIN, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_UINT8(0, rows[0].pct_done);
}

/* ======================================================================
 * AC3 — next_starred
 * ==================================================================== */

static void S07_AC3_picks_earliest_future_starred(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Later Star", 0, DAY_A, 900, 960, true));
    pack_add(&p, mk_set("Sooner Star", 1, DAY_A, 700, 760, true));
    pack_add(&p, mk_set("Unstarred", 2, DAY_A, 650, 690, false));

    ff_next_t next;
    bool found = ff_sched_next_starred(&p, DAY_A, 600, &next);
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_STRING("Sooner Star", next.set->artist);
    TEST_ASSERT_EQUAL_INT16(100, next.mins_until);
}

static void S07_AC3_false_when_none_starred(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Unstarred", 0, DAY_A, 700, 760, false));

    ff_next_t next;
    bool found = ff_sched_next_starred(&p, DAY_A, 600, &next);
    TEST_ASSERT_FALSE(found);
    TEST_ASSERT_NULL(next.set);
}

static void S07_AC3_false_when_starred_sets_are_past_or_live(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Already Live", 0, DAY_A, 600, 700, true));
    pack_add(&p, mk_set("Already Over", 1, DAY_A, 400, 500, true));

    ff_next_t next;
    /* now_min=650: "Already Live" has started (not future), "Already
     * Over" has finished. Neither counts as "next". */
    bool found = ff_sched_next_starred(&p, DAY_A, 650, &next);
    TEST_ASSERT_FALSE(found);
}

static void S07_AC3_ignores_null_time_starred_sets(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Starred but TBD", 0, DAY_A, -1, -1, true));

    ff_next_t next;
    bool found = ff_sched_next_starred(&p, DAY_A, 600, &next);
    TEST_ASSERT_FALSE(found);
}

static void S07_AC3_ignores_other_days(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Starred Tomorrow", 0, DAY_B, 700, 760, true));

    ff_next_t next;
    bool found = ff_sched_next_starred(&p, DAY_A, 600, &next);
    TEST_ASSERT_FALSE(found);
}

/* ======================================================================
 * toggle_star (not individually numbered by the spec's AC list, but in
 * this PR's scope)
 * ==================================================================== */

static void test_toggle_star_flips_and_is_bounds_checked(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Toggle Me", 0, DAY_A, 600, 700, false));

    TEST_ASSERT_FALSE(p.sets[0].starred);
    ff_sched_toggle_star(&p, 0);
    TEST_ASSERT_TRUE(p.sets[0].starred);
    ff_sched_toggle_star(&p, 0);
    TEST_ASSERT_FALSE(p.sets[0].starred);

    /* Out-of-range index: no-op, no crash. */
    ff_sched_toggle_star(&p, 999);
    TEST_ASSERT_FALSE(p.sets[0].starred);
}

/* ======================================================================
 * AC4 — alarm
 * ==================================================================== */

static void S07_AC4_fires_once_at_t15_crossing(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Star", 0, DAY_A, 900, 960, true)); /* T-15 at now_min=885 */

    ff_sched_alarm_t st;
    ff_sched_alarm_init(&st);

    /* Before T-15: nothing. */
    TEST_ASSERT_NULL(ff_sched_alarm_tick(&st, &p, DAY_A, 884));

    /* Exactly T-15: fires. */
    fp_set_t const *fired = ff_sched_alarm_tick(&st, &p, DAY_A, 885);
    TEST_ASSERT_NOT_NULL(fired);
    TEST_ASSERT_EQUAL_STRING("Star", fired->artist);
}

static void S07_AC4_idempotent_no_refire_on_retick(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Star", 0, DAY_A, 900, 960, true));

    ff_sched_alarm_t st;
    ff_sched_alarm_init(&st);

    TEST_ASSERT_NOT_NULL(ff_sched_alarm_tick(&st, &p, DAY_A, 885));
    /* Re-tick at the same minute: no refire. */
    TEST_ASSERT_NULL(ff_sched_alarm_tick(&st, &p, DAY_A, 885));
    /* Re-tick later, still before the set ends: no refire. */
    TEST_ASSERT_NULL(ff_sched_alarm_tick(&st, &p, DAY_A, 920));
    /* Re-tick after the set has ended: still no refire. */
    TEST_ASSERT_NULL(ff_sched_alarm_tick(&st, &p, DAY_A, 970));
}

static void S07_AC4_two_stars_five_min_apart_fire_in_order(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("First Star", 0, DAY_A, 900, 960, true));  /* T-15 @ 885 */
    pack_add(&p, mk_set("Second Star", 1, DAY_A, 905, 965, true)); /* T-15 @ 890 */

    ff_sched_alarm_t st;
    ff_sched_alarm_init(&st);

    /* Tick-by-tick, crossing each threshold separately. */
    TEST_ASSERT_NULL(ff_sched_alarm_tick(&st, &p, DAY_A, 880));

    fp_set_t const *a = ff_sched_alarm_tick(&st, &p, DAY_A, 885);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("First Star", a->artist);

    /* Second star not due yet at 885 (mins_until=20). */
    TEST_ASSERT_NULL(ff_sched_alarm_tick(&st, &p, DAY_A, 887));

    fp_set_t const *b = ff_sched_alarm_tick(&st, &p, DAY_A, 890);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_STRING("Second Star", b->artist);
}

static void S07_AC4_both_due_in_one_tick_fire_in_start_order_across_calls(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("First Star", 0, DAY_A, 900, 960, true));
    pack_add(&p, mk_set("Second Star", 1, DAY_A, 905, 965, true));

    ff_sched_alarm_t st;
    ff_sched_alarm_init(&st);

    /* A single tick lands after BOTH T-15 thresholds have passed (e.g.
     * the app was asleep). Each call still only reports one set; the
     * earlier-starting one must come first. */
    fp_set_t const *first = ff_sched_alarm_tick(&st, &p, DAY_A, 895);
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_STRING("First Star", first->artist);

    fp_set_t const *second = ff_sched_alarm_tick(&st, &p, DAY_A, 895);
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_EQUAL_STRING("Second Star", second->artist);

    TEST_ASSERT_NULL(ff_sched_alarm_tick(&st, &p, DAY_A, 895));
}

static void S07_AC4_does_not_fire_for_a_set_already_ended(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Missed Entirely", 0, DAY_A, 900, 960, true));

    ff_sched_alarm_t st;
    ff_sched_alarm_init(&st);

    /* First tick happens long after the set is over: T-15 crossing was
     * missed AND the set has finished — must not fire a stale alert. */
    TEST_ASSERT_NULL(ff_sched_alarm_tick(&st, &p, DAY_A, 1200));
}

static void S07_AC4_ignores_unstarred_and_null_time_sets(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Unstarred", 0, DAY_A, 900, 960, false));
    pack_add(&p, mk_set("Starred TBD", 1, DAY_A, -1, -1, true));

    ff_sched_alarm_t st;
    ff_sched_alarm_init(&st);

    TEST_ASSERT_NULL(ff_sched_alarm_tick(&st, &p, DAY_A, 890));
}

static void S07_AC4_reinit_clears_fired_state(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Star", 0, DAY_A, 900, 960, true));

    ff_sched_alarm_t st;
    ff_sched_alarm_init(&st);
    TEST_ASSERT_NOT_NULL(ff_sched_alarm_tick(&st, &p, DAY_A, 885));
    TEST_ASSERT_NULL(ff_sched_alarm_tick(&st, &p, DAY_A, 886));

    /* Simulates a fresh pack load: re-init clears fired-bits. */
    ff_sched_alarm_init(&st);
    TEST_ASSERT_NOT_NULL(ff_sched_alarm_tick(&st, &p, DAY_A, 886));
}

/* ======================================================================
 * AC5 — all-null-times pack: now/next empty/false, TBD flagged
 * ==================================================================== */

static void S07_AC5_all_null_pack_now_playing_is_empty(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Unknown Set 1", 0, DAY_A, -1, -1, false));
    pack_add(&p, mk_set("Unknown Set 2", 1, DAY_A, -1, -1, false));

    ff_now_row_t rows[4];
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 700, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(0, n);
}

static void S07_AC5_all_null_pack_next_starred_is_false(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Unknown Starred Set", 0, DAY_A, -1, -1, true));

    ff_next_t next;
    bool found = ff_sched_next_starred(&p, DAY_A, 700, &next);
    TEST_ASSERT_FALSE(found);
}

static void S07_AC5_day_tbd_true_when_all_null(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Unknown Set 1", 0, DAY_A, -1, -1, false));
    pack_add(&p, mk_set("Unknown Set 2", 1, DAY_A, -1, -1, false));

    TEST_ASSERT_TRUE(ff_sched_day_tbd(&p, DAY_A));
}

static void S07_AC5_day_tbd_false_when_any_known_time(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Unknown Set", 0, DAY_A, -1, -1, false));
    pack_add(&p, mk_set("Known Set", 1, DAY_A, 600, 660, false));

    TEST_ASSERT_FALSE(ff_sched_day_tbd(&p, DAY_A));
}

static void S07_AC5_day_tbd_false_when_day_has_no_sets(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Some Other Day", 0, DAY_B, -1, -1, false));

    TEST_ASSERT_FALSE(ff_sched_day_tbd(&p, DAY_A));
}

static void test_day_sets_lists_all_including_null_times_in_pack_order(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("TBD One", 0, DAY_A, -1, -1, false));
    pack_add(&p, mk_set("Known", 1, DAY_A, 600, 660, false));
    pack_add(&p, mk_set("Other Day", 2, DAY_B, 600, 660, false));
    pack_add(&p, mk_set("TBD Two", 3, DAY_A, -1, -1, false));

    fp_set_t const *out[8];
    uint16_t n = ff_sched_day_sets(&p, DAY_A, out, 8);
    TEST_ASSERT_EQUAL_UINT16(3, n);
    TEST_ASSERT_EQUAL_STRING("TBD One", out[0]->artist);
    TEST_ASSERT_EQUAL_STRING("Known", out[1]->artist);
    TEST_ASSERT_EQUAL_STRING("TBD Two", out[2]->artist);
}

/* AC5, integration case: the real vendored Lost Lands 2026 fixture is
 * (as of this writing, per docs/specs/S07-now-face.md's "current Lost
 * Lands state") entirely null-times. Parse it for real via fp_parse and
 * assert the all-null-TBD path against actual pack data, not a
 * hand-built fixture. */
static void S07_AC5_real_lost_lands_fixture_is_all_null_tbd(void)
{
    char buf[256u * 1024u];
    char path[512];
    snprintf(path, sizeof(path), "%slost-lands-2026.festpack.json", FP_FIXTURE_DIR);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    size_t len = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    TEST_ASSERT_TRUE(len > 0);

    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);
    TEST_ASSERT_TRUE_MESSAGE(pack.n_sets > 0, "fixture unexpectedly has no sets");

    /* festival.start ("2026-09-18") is a day every set's "day" can be
     * compared against; use the festival's own start_doy so this test
     * doesn't hardcode a day-of-year computation. */
    uint16_t day = pack.start_doy;

    TEST_ASSERT_TRUE_MESSAGE(ff_sched_day_tbd(&pack, day),
                              "expected the real Lost Lands fixture to still be all-null (SET TIMES TBD) — "
                              "if this now fails, the fixture has been updated with real set times and "
                              "S07-now-face.md's AC5/AC6 goldens (now_tbd.json) likely need attention too");

    ff_now_row_t rows[8];
    TEST_ASSERT_EQUAL_UINT8(0, ff_sched_now_playing(&pack, day, 720, rows, 8));

    ff_next_t next;
    TEST_ASSERT_FALSE(ff_sched_next_starred(&pack, day, 720, &next));

    /* But the day lineup helper still lists them (TBD sets are shown,
     * just excluded from now/next/alarms). */
    fp_set_t const *lineup[FP_MAX_SETS];
    uint16_t n = ff_sched_day_sets(&pack, day, lineup, FP_MAX_SETS);
    TEST_ASSERT_TRUE(n > 0);
}

/* ======================================================================
 * main
 * ==================================================================== */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S07_AC1_now_playing_returns_exactly_the_concurrent_sets);
    RUN_TEST(S07_AC1_pct_done_boundary_start_is_zero);
    RUN_TEST(S07_AC1_pct_done_boundary_end_is_hundred);
    RUN_TEST(S07_AC1_pct_done_midpoint);
    RUN_TEST(S07_AC1_null_time_sets_never_appear);
    RUN_TEST(S07_AC1_max_cap_truncates_in_pack_order);
    RUN_TEST(S07_AC1_day_scoping_excludes_other_days);

    RUN_TEST(S07_AC2_midnight_crossing_set_is_now_at_0030);
    RUN_TEST(S07_AC2_midnight_crossing_pct_done_across_midnight);
    RUN_TEST(S07_AC2_midnight_crossing_set_not_visible_on_next_day_doy);
    RUN_TEST(S07_AC2_0600_boundary_both_sides);

    RUN_TEST(S07_AC3_picks_earliest_future_starred);
    RUN_TEST(S07_AC3_false_when_none_starred);
    RUN_TEST(S07_AC3_false_when_starred_sets_are_past_or_live);
    RUN_TEST(S07_AC3_ignores_null_time_starred_sets);
    RUN_TEST(S07_AC3_ignores_other_days);

    RUN_TEST(test_toggle_star_flips_and_is_bounds_checked);

    RUN_TEST(S07_AC4_fires_once_at_t15_crossing);
    RUN_TEST(S07_AC4_idempotent_no_refire_on_retick);
    RUN_TEST(S07_AC4_two_stars_five_min_apart_fire_in_order);
    RUN_TEST(S07_AC4_both_due_in_one_tick_fire_in_start_order_across_calls);
    RUN_TEST(S07_AC4_does_not_fire_for_a_set_already_ended);
    RUN_TEST(S07_AC4_ignores_unstarred_and_null_time_sets);
    RUN_TEST(S07_AC4_reinit_clears_fired_state);

    RUN_TEST(S07_AC5_all_null_pack_now_playing_is_empty);
    RUN_TEST(S07_AC5_all_null_pack_next_starred_is_false);
    RUN_TEST(S07_AC5_day_tbd_true_when_all_null);
    RUN_TEST(S07_AC5_day_tbd_false_when_any_known_time);
    RUN_TEST(S07_AC5_day_tbd_false_when_day_has_no_sets);
    RUN_TEST(test_day_sets_lists_all_including_null_times_in_pack_order);
    RUN_TEST(S07_AC5_real_lost_lands_fixture_is_all_null_tbd);

    return UNITY_END();
}
