/**
 * test_wall_window.c — S18 slice c (#40): pack -> plausibility window.
 *
 * Criteria (docs/specs/S18-wall-clock-trust.md, "Slice c" AC1-AC4):
 *   AC1 — with NO pack the fixed bootstrap window applies. Covered here as
 *         the ff_wall_init default (a fresh state gates on the fixed
 *         window) and at the shell boundary in test_shell.c
 *         (S18c_* ); the "no pack -> stays fixed" fallback of the derivation
 *         itself is the null/absent-date case below.
 *   AC2 — with the Lost Lands dates the window tightens: a Sep 2026 stamp
 *         is plausible, a Sep 2029 stamp (inside the FIXED window, outside
 *         the tightened one) is rejected.
 *   AC3 — the derivation handles the multi-day / after-midnight span and a
 *         null-dated pack (falls back to the fixed window HONESTLY —
 *         returns false, invents nothing).
 *   AC4 — the build-date proximity guard: test_wall.c owns it (it is a core
 *         predicate), driven by synthetic dates.
 *
 * Reference: Lost Lands 2026 is Sep 18-20. Sep 18 is day-of-year 261,
 * Sep 20 is 263 (cross-checked against test_wall.c's own frozen refs and
 * Python datetime). ff_wall_unix_from_doy(2026, 261) is 2026-09-18
 * 00:00:00 UTC = 1789689600 (= U_EVENING 1789768800 - 22h).
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "ff_wall.h"
#include "ff_wall_window.h"
#include "fp_pack.h"

/* Frozen anchors, all 00:00:00 UTC (cross-checked, see the header note). */
#define SEP18_2026_UTC ((int64_t)1789689600) /* doy 261, 2026 */
#define SEP20_2026_UTC ((int64_t)(1789689600 + 2 * 86400)) /* doy 263, 2026 */
#define SEP18_2029_UTC ((int64_t)1884384000) /* doy 261, 2029 (not leap) */

/* A pack carrying only the fields the derivation reads. */
static fp_pack_t lost_lands_dates(void)
{
    fp_pack_t p;
    memset(&p, 0, sizeof(p));
    p.year = 2026;
    p.start_doy = 261; /* Sep 18 */
    p.end_doy = 263;   /* Sep 20 */
    return p;
}

void setUp(void) {}
void tearDown(void) {}

/* ---- ff_wall_unix_from_doy: the exposed civil-date math [api] ---- */

static void S18c_unix_from_doy_matches_known_anchors(void)
{
    /* Epoch anchor: doy 1 of 1970 is unix 0. */
    TEST_ASSERT_EQUAL_INT64(0, ff_wall_unix_from_doy(1970, 1));
    /* Lost Lands start, cross-checked to 2026-09-18 00:00 UTC. */
    TEST_ASSERT_EQUAL_INT64(SEP18_2026_UTC, ff_wall_unix_from_doy(2026, 261));
    /* doy 262 is exactly one day later — the primitive is linear in doy. */
    TEST_ASSERT_EQUAL_INT64(SEP18_2026_UTC + 86400, ff_wall_unix_from_doy(2026, 262));
    /* Linear extension past the year: doy 366 of a common year (2026) is
     * the next Jan 1, which is what the "day after end_doy" ceiling relies
     * on. 2026 has 365 days, so doy 366 == 2027-01-01 00:00 UTC. */
    TEST_ASSERT_EQUAL_INT64(ff_wall_unix_from_doy(2027, 1), ff_wall_unix_from_doy(2026, 366));
}

/* ---- AC2: the Lost Lands window is derived correctly and tightens ---- */

static void S18c_AC2_lost_lands_window_has_the_expected_bounds(void)
{
    fp_pack_t const p = lost_lands_dates();
    int64_t floor_s = -1, ceiling_s = -1;
    TEST_ASSERT_TRUE(ff_wall_window_from_pack(&p, &floor_s, &ceiling_s));

    /* floor = Sep 18 00:00 UTC - 14d; ceiling = Sep 21 00:00 UTC + 14d
     * (the day AFTER end_doy 263, so all of Sep 20 is inside). */
    TEST_ASSERT_EQUAL_INT64(SEP18_2026_UTC - FF_WALL_WINDOW_MARGIN_S, floor_s);
    TEST_ASSERT_EQUAL_INT64(SEP20_2026_UTC + 86400 + FF_WALL_WINDOW_MARGIN_S, ceiling_s);
}

static void S18c_AC2_sep2026_plausible_sep2029_rejected(void)
{
    fp_pack_t const p = lost_lands_dates();
    int64_t floor_s = 0, ceiling_s = 0;
    TEST_ASSERT_TRUE(ff_wall_window_from_pack(&p, &floor_s, &ceiling_s));

    /* Install the tightened window and drive the REAL observe gate. */
    ff_wall_state_t st;
    ff_wall_init(&st);
    TEST_ASSERT_TRUE(ff_wall_set_window(&st, floor_s, ceiling_s));

    /* A Sep 2026 festival-time stamp latches — it is inside the window. */
    int64_t const sep2026 = SEP18_2026_UTC + 22 * 3600; /* Sep 18 22:00 UTC */
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_LATCHED,
                          ff_wall_observe(&st, sep2026, 1000u, FF_WALL_TRUST_BOOTSTRAP));

    /* A Sep 2029 stamp — INSIDE the fixed [FLOOR, CEILING) window, so the
     * un-tightened gate would have taken it — is rejected by the tightened
     * one. Use a fresh unlatched state so this is a pure window test, not a
     * re-latch/trust test. */
    ff_wall_state_t st2;
    ff_wall_init(&st2);
    TEST_ASSERT_TRUE(ff_wall_set_window(&st2, floor_s, ceiling_s));
    int64_t const sep2029 = SEP18_2029_UTC + 22 * 3600;
    TEST_ASSERT_TRUE(sep2029 >= FF_WALL_EPOCH_FLOOR && sep2029 < FF_WALL_EPOCH_CEILING); /* fixed-window control */
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_REJECTED,
                          ff_wall_observe(&st2, sep2029, 1000u, FF_WALL_TRUST_BOOTSTRAP));
}

/* ---- AC3: multi-day / after-midnight span, and honest null-date fallback ---- */

static void S18c_AC3_after_midnight_of_the_last_day_is_inside(void)
{
    /* The festival day rolls at 06:00 local and sets run past midnight, so
     * a "Sep 20 night" set at 02:00 local is really Sep 21 wall-clock. It
     * MUST stay plausible — the window narrowing may never reject a genuine
     * festival-time reading (S18 honest-data brief). end_doy 263's ceiling
     * reaches the day after, and the 14-day margin covers the rest. */
    fp_pack_t const p = lost_lands_dates();
    int64_t floor_s = 0, ceiling_s = 0;
    TEST_ASSERT_TRUE(ff_wall_window_from_pack(&p, &floor_s, &ceiling_s));

    ff_wall_state_t st;
    ff_wall_init(&st);
    TEST_ASSERT_TRUE(ff_wall_set_window(&st, floor_s, ceiling_s));

    /* Sep 21 04:00 UTC — after midnight following the last festival day. */
    int64_t const after_midnight = SEP20_2026_UTC + 86400 + 4 * 3600;
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_LATCHED,
                          ff_wall_observe(&st, after_midnight, 1000u, FF_WALL_TRUST_BOOTSTRAP));
}

static void S18c_AC3_null_dated_pack_falls_back_to_fixed_window(void)
{
    int64_t floor_s = 0x7fffffff, ceiling_s = 0x7fffffff; /* sentinels */

    /* start_doy == 0 (fp_parse leaves doy zero for an absent/null date). */
    fp_pack_t p = lost_lands_dates();
    p.start_doy = 0;
    TEST_ASSERT_FALSE(ff_wall_window_from_pack(&p, &floor_s, &ceiling_s));
    TEST_ASSERT_EQUAL_INT64(0x7fffffff, floor_s);   /* untouched */
    TEST_ASSERT_EQUAL_INT64(0x7fffffff, ceiling_s); /* untouched */

    /* end_doy == 0 likewise. */
    p = lost_lands_dates();
    p.end_doy = 0;
    TEST_ASSERT_FALSE(ff_wall_window_from_pack(&p, &floor_s, &ceiling_s));

    /* year == 0 (absent). */
    p = lost_lands_dates();
    p.year = 0;
    TEST_ASSERT_FALSE(ff_wall_window_from_pack(&p, &floor_s, &ceiling_s));

    /* The honest consequence: a shell handed such a pack keeps the fixed
     * window. Simulated here — set_window is simply never called with a
     * derived window, so ff_wall_init's default (the fixed bounds) stands
     * and a Sep 2029 stamp is still accepted. */
    ff_wall_state_t st;
    ff_wall_init(&st);
    int64_t const sep2029 = SEP18_2029_UTC + 22 * 3600;
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_LATCHED,
                          ff_wall_observe(&st, sep2029, 1000u, FF_WALL_TRUST_BOOTSTRAP));
}

static void S18c_AC3_corrupt_dates_fall_back_not_garbage(void)
{
    int64_t floor_s = 0, ceiling_s = 0;

    /* Inverted span: end before start. Never produce a floor >= ceiling. */
    fp_pack_t p = lost_lands_dates();
    p.start_doy = 263;
    p.end_doy = 261;
    TEST_ASSERT_FALSE(ff_wall_window_from_pack(&p, &floor_s, &ceiling_s));

    /* Out-of-range day-of-year (corrupt pack). */
    p = lost_lands_dates();
    p.end_doy = 400;
    TEST_ASSERT_FALSE(ff_wall_window_from_pack(&p, &floor_s, &ceiling_s));

    /* A NULL pack or NULL outputs are refused, not dereferenced. */
    TEST_ASSERT_FALSE(ff_wall_window_from_pack(NULL, &floor_s, &ceiling_s));
    fp_pack_t const ok = lost_lands_dates();
    TEST_ASSERT_FALSE(ff_wall_window_from_pack(&ok, NULL, &ceiling_s));
    TEST_ASSERT_FALSE(ff_wall_window_from_pack(&ok, &floor_s, NULL));
}

static void S18c_a_single_day_festival_still_yields_a_valid_window(void)
{
    /* start_doy == end_doy (a one-day event). The +1-day ceiling keeps the
     * window non-empty and covers the whole day; nothing degenerate. */
    fp_pack_t p = lost_lands_dates();
    p.start_doy = 261;
    p.end_doy = 261;
    int64_t floor_s = 0, ceiling_s = 0;
    TEST_ASSERT_TRUE(ff_wall_window_from_pack(&p, &floor_s, &ceiling_s));
    TEST_ASSERT_TRUE(floor_s < ceiling_s);
    TEST_ASSERT_EQUAL_INT64(SEP18_2026_UTC - FF_WALL_WINDOW_MARGIN_S, floor_s);
    TEST_ASSERT_EQUAL_INT64(SEP18_2026_UTC + 86400 + FF_WALL_WINDOW_MARGIN_S, ceiling_s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S18c_unix_from_doy_matches_known_anchors);
    RUN_TEST(S18c_AC2_lost_lands_window_has_the_expected_bounds);
    RUN_TEST(S18c_AC2_sep2026_plausible_sep2029_rejected);
    RUN_TEST(S18c_AC3_after_midnight_of_the_last_day_is_inside);
    RUN_TEST(S18c_AC3_null_dated_pack_falls_back_to_fixed_window);
    RUN_TEST(S18c_AC3_corrupt_dates_fall_back_not_garbage);
    RUN_TEST(S18c_a_single_day_festival_still_yields_a_valid_window);
    return UNITY_END();
}
