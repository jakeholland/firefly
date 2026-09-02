/**
 * test_sched.c — S07 slices (a) engine + (c) alarm, criteria-numbered
 * per docs/specs/S07-now-face.md.
 *
 *   AC1 — now_playing: concurrent/finished/future filtering, exact
 *         pct_done boundaries under the half-open "now" window (start=0%,
 *         monotonically increasing, never reaches 100 while live; a
 *         zero-gap same-stage changeover yields exactly one row — the
 *         starting set). See docs/specs/S07-now-face.md's ## Amendments
 *         (PR #9 review) for the ruling that made the window half-open;
 *         it was originally inclusive at both ends.
 *   AC2 — midnight-crossing set attribution + math with now_min > 1440.
 *   AC3 — next_starred: earliest future starred set, false cases, and a
 *         tie-break case (equal start_min -> lower set index).
 *   AC4 — alarm: fires once at T-15 crossing, idempotent, ordered
 *         (including a tie-break case), un-star/re-star re-arms.
 *   AC5 — all-null-times pack: now/next empty/false, TBD flagged. The
 *         last AC5 case parses the real vendored Lost Lands 2026
 *         fixture via fp_parse (firmware/festpack/tests/fixtures/) —
 *         see the CMakeLists.txt S07 section for why that lives in this
 *         same executable rather than a separate test_sched_integration.
 *   AC6 (golden lineup_live.json/lineup_tbd.json screenshots) is UI (slice b)
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

/* S26 slice (a) - shared file-scope jsmn scratch: fp_parse no longer owns
 * a static token arena (fp_pack.h), so every test in this file supplies
 * one. Unity runs tests sequentially in one thread, so sharing this
 * across test functions is safe - each call fully consumes and
 * re-tokenizes it. */
static jsmntok_t s_toks[FP_MAX_TOKENS];

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

static void S07_AC1_pct_done_leaves_now_exactly_at_end_half_open(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Closer", 0, DAY_A, 600, 660, false)); /* 60-minute set */

    ff_now_row_t rows[4];

    /* Last live minute (end - 1 = 659): pct is high but never 100 — the
     * half-open window means the set leaves "now" at end_min itself. */
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 659, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_UINT8(98, rows[0].pct_done); /* (59*100)/60 = 98 */
    TEST_ASSERT_EQUAL_INT16(1, rows[0].mins_left);

    /* Exactly at end_min (660): half-open -> no longer "now" at all. */
    n = ff_sched_now_playing(&p, DAY_A, 660, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(0, n);
}

static void S07_AC1_zero_gap_changeover_single_row(void)
{
    fp_pack_t p;
    pack_init(&p);
    /* Ordinary festival scheduling: one stage, no gap between sets — the
     * next artist's start_min equals the previous artist's end_min. Per
     * the spec's ## Amendments ruling, the STARTING set wins at that
     * exact minute; the ending set has already left "now". */
    pack_add(&p, mk_set("Ending", 0, DAY_A, 600, 660, false));
    pack_add(&p, mk_set("Starting", 0, DAY_A, 660, 720, false));

    ff_now_row_t rows[4];
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 660, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(1, n); /* exactly one row, not two, for stage 0 */
    TEST_ASSERT_EQUAL_STRING("Starting", rows[0].set->artist);
    TEST_ASSERT_EQUAL_UINT8(0, rows[0].pct_done);
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

static void S07_AC1_null_start_sets_never_appear(void)
{
    /* 2026-08-24 amendment: only a NULL start_min excludes a set from
     * now_playing. This used to be named
     * "S07_AC1_null_time_sets_never_appear" and included a "TBD End" set
     * (known start, null end) among the ones asserted to never appear —
     * that assumption is now WRONG (a known-start/null-end set is timed
     * and, if live, DOES appear — see
     * S07_2026_08_24_null_end_derives_from_next_same_stage_start and
     * S07_AC1_a_known_start_with_null_end_still_appears_now below). Only
     * genuinely null-START sets are covered here. */
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("TBD Start", 0, DAY_A, -1, 660, false));
    pack_add(&p, mk_set("All TBD", 2, DAY_A, -1, -1, false));

    ff_now_row_t rows[8];
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 620, rows, 8);
    TEST_ASSERT_EQUAL_UINT8(0, n);
}

static void S07_AC1_a_known_start_with_null_end_still_appears_now(void)
{
    /* The exact case the old (now-removed) "TBD End" assertion above got
     * backwards: a known start_min with a null end_min is TIMED per the
     * 2026-08-24 amendment. With no other set sharing its stage/day, its
     * end is unknowable (pct_valid=false) — but it must still appear as
     * a live row, not silently vanish. */
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Starts-Only", 1, DAY_A, 600, -1, false));

    ff_now_row_t rows[8];
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 620, rows, 8);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_STRING("Starts-Only", rows[0].set->artist);
    TEST_ASSERT_FALSE(rows[0].pct_valid);
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
 * 2026-08-24 amendment — "starts-only set grids" (S07-now-face.md
 * ## Amendments). Found end-to-end against the first real festpack
 * (Bass Canyon 2026: 82 published start times, every end_min null).
 * A set with a known start_min is "timed" regardless of end_min; a null
 * end_min derives from the next known start_min on the SAME stage, SAME
 * day (searched by (stage_idx, day_doy, start_min), NOT by p->sets[]
 * array order — the real Bass Canyon pack lists each stage's sets
 * headliner-first, i.e. DESCENDING by start_min). The last known-start
 * set on a stage/day, with no later same-stage start to derive from, is
 * live but pct_valid=false — no invented duration.
 * ==================================================================== */

static void S07_2026_08_24_null_end_derives_from_next_same_stage_start(void)
{
    fp_pack_t p;
    pack_init(&p);
    /* Deliberately added in DESCENDING start order (mirrors the real
     * Bass Canyon pack's headliner-first array order) to prove the
     * derivation is a scan, not an array-order lookup. */
    pack_add(&p, mk_set("Excision", 0, DAY_A, 1410, -1, false));       /* 23:30, last of night */
    pack_add(&p, mk_set("Sullivan King", 0, DAY_A, 1350, -1, false));  /* 22:30 */
    pack_add(&p, mk_set("NGHTMRE", 0, DAY_A, 1290, -1, false));        /* 21:30 */

    ff_now_row_t rows[4];
    /* NGHTMRE mid-set: derived end is Sullivan King's 1350, a 60-minute
     * derived duration, so 1320 (30 min in) is 50%. */
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 1320, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_STRING("NGHTMRE", rows[0].set->artist);
    TEST_ASSERT_TRUE(rows[0].pct_valid);
    TEST_ASSERT_EQUAL_UINT8(50, rows[0].pct_done);
    TEST_ASSERT_EQUAL_INT16(30, rows[0].mins_left);
}

static void S07_2026_08_24_derived_end_is_half_open_at_the_changeover_minute(void)
{
    /* The exact mutation-conscious case: a `<` -> `<=` (or vice versa)
     * slip in the derived-end comparison would let both NGHTMRE and
     * Sullivan King show up as "now" at the changeover minute, or
     * neither. Same half-open contract as an explicit end_min (PR #9's
     * ruling) — this proves it composes with the derivation. */
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("NGHTMRE", 0, DAY_A, 1290, -1, false));
    /* A third set after Sullivan King so ITS derived end is also a real
     * next-set derivation (1410), not the last-of-day day-boundary
     * fallback — keeps this test purely about the half-open boundary,
     * not conflated with the separate last-set/pct_valid case. */
    pack_add(&p, mk_set("Sullivan King", 0, DAY_A, 1350, -1, false));
    pack_add(&p, mk_set("Excision", 0, DAY_A, 1410, -1, false));

    ff_now_row_t rows[4];

    /* One minute before the derived boundary: NGHTMRE still "now". */
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 1349, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_STRING("NGHTMRE", rows[0].set->artist);

    /* Exactly at the derived boundary (1350): NGHTMRE is gone, Sullivan
     * King alone is "now" at pct 0 — the starting set wins, same as the
     * explicit-end zero-gap changeover. */
    n = ff_sched_now_playing(&p, DAY_A, 1350, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_STRING("Sullivan King", rows[0].set->artist);
    TEST_ASSERT_TRUE(rows[0].pct_valid);
    TEST_ASSERT_EQUAL_UINT8(0, rows[0].pct_done);
}

static void S07_2026_08_24_last_set_of_day_is_live_with_pct_invalid(void)
{
    fp_pack_t p;
    pack_init(&p);
    /* Only set on this stage/day with a known start: nothing to derive
     * an end from. */
    pack_add(&p, mk_set("Excision", 0, DAY_A, 1410, -1, false)); /* 23:30 */

    ff_now_row_t rows[4];

    /* Shortly after starting: live, pct genuinely unknown. */
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 1420, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_STRING("Excision", rows[0].set->artist);
    TEST_ASSERT_FALSE(rows[0].pct_valid);
    /* mins_left counts down to the festival day window's own end
     * (1800), the one boundary this module actually knows — NOT to a
     * fabricated set duration. */
    TEST_ASSERT_EQUAL_INT16(380, rows[0].mins_left);

    /* Last live minute of the day window (1799): still "now". */
    n = ff_sched_now_playing(&p, DAY_A, 1799, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_FALSE(rows[0].pct_valid);

    /* At the day window's own end (1800, FF_SCHED_FESTIVAL_DAY_START_MIN
     * + FF_SCHED_DAY_SPAN_MIN): stops counting as "now" — half-open,
     * same as every other boundary in this module. Do NOT invent a
     * duration past this point either. */
    n = ff_sched_now_playing(&p, DAY_A, (int16_t)(FF_SCHED_FESTIVAL_DAY_START_MIN + FF_SCHED_DAY_SPAN_MIN), rows, 4);
    TEST_ASSERT_EQUAL_UINT8(0, n);
}

static void S07_2026_08_24_derivation_is_scoped_to_same_stage_only(void)
{
    /* A later start on a DIFFERENT stage, same day, must never supply a
     * derived end for this stage's last set — that would silently
     * borrow another stage's schedule. */
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Excision", 0, DAY_A, 1410, -1, false));   /* canyon, 23:30, last of night */
    pack_add(&p, mk_set("Later Elsewhere", 1, DAY_A, 1420, -1, false)); /* hilltop, 23:40 */

    ff_now_row_t rows[4];
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 1425, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(2, n); /* both stages concurrently live */
    TEST_ASSERT_EQUAL_STRING("Excision", rows[0].set->artist);
    TEST_ASSERT_FALSE(rows[0].pct_valid); /* still unknown — "Later Elsewhere" is a different stage */
}

static void S07_2026_08_24_derivation_is_scoped_to_same_day_only(void)
{
    /* A later start on the SAME stage but a DIFFERENT day must not
     * supply a derived end either — day_doy scoping applies to the
     * derivation exactly like every other now_playing check.
     *
     * 2026-08-25 review fixup (PR #65 finding 2): the cross-day sibling
     * MUST have a start_min LATER than the current set's — a smaller
     * one (as this test originally used, 1290 < 1410) is already
     * excluded by sched_next_stage_start's plain `start_min <=
     * s->start_min` ordering check regardless of whether the day_doy
     * filter exists, so the original fixture never actually exercised
     * the filter (removing `if (o->day_doy != s->day_doy) continue;`
     * left this test, and the whole suite, green — the exact
     * proxy-vs-property gap docs/review/code-review.md item 6 warns
     * about). 1420 is later than Excision's 1410, so this now actually
     * distinguishes "day-scoped" from "day-scoping removed": mutate the
     * day_doy filter away and this fails (pct_valid flips to true,
     * mins_left flips from the day-boundary fallback to 5). */
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Excision", 0, DAY_A, 1410, -1, false));
    pack_add(&p, mk_set("Tomorrow Early Set", 0, DAY_B, 1420, -1, false));

    ff_now_row_t rows[4];
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 1415, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_STRING("Excision", rows[0].set->artist);
    TEST_ASSERT_FALSE(rows[0].pct_valid);
    TEST_ASSERT_EQUAL_INT16(385, rows[0].mins_left); /* day-boundary fallback: 1800-1415 */
}

static void S07_2026_08_24_next_starred_allows_a_null_end_set(void)
{
    /* Before this amendment, next_starred required BOTH times known —
     * a starred set with a real start but no published end used to
     * never show up as "next" at all, even though "next" only needs a
     * start time. */
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Excision", 0, DAY_A, 1410, -1, true)); /* starred, starts-only */

    ff_next_t next;
    bool found = ff_sched_next_starred(&p, DAY_A, 1300, &next);
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_STRING("Excision", next.set->artist);
    TEST_ASSERT_EQUAL_INT16(110, next.mins_until);
}

static void S07_2026_08_24_alarm_fires_for_a_null_end_starred_set(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Excision", 0, DAY_A, 1410, -1, true)); /* T-15 at 1395 */

    ff_sched_alarm_t st;
    ff_sched_alarm_init(&st);

    TEST_ASSERT_NULL(ff_sched_alarm_tick(&st, &p, DAY_A, 1394));
    fp_set_t const *fired = ff_sched_alarm_tick(&st, &p, DAY_A, 1395);
    TEST_ASSERT_NOT_NULL(fired);
    TEST_ASSERT_EQUAL_STRING("Excision", fired->artist);
}

static void S07_2026_08_24_alarm_does_not_fire_stale_past_the_day_window_end(void)
{
    /* A null-end, last-of-day starred set whose T-15 crossing was
     * missed entirely: the "already over" check must use the derived
     * (day-window) end, not treat a null end_min as "never over". */
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Excision", 0, DAY_A, 1410, -1, true));

    ff_sched_alarm_t st;
    ff_sched_alarm_init(&st);

    /* First tick lands well past the festival day window's own end
     * (1800) — the set is unambiguously over by then even with no
     * explicit end_min. */
    TEST_ASSERT_NULL(ff_sched_alarm_tick(&st, &p, DAY_A, 1850));
}

static void S07_2026_08_24_day_tbd_false_when_a_set_has_a_start_but_no_end(void)
{
    /* The exact predicate this amendment fixes: a day where every set
     * has a PUBLISHED start but no published end must NOT read as
     * "SET TIMES TBD" — that was the literal bug (Bass Canyon 2026
     * rendered as TBD despite 82 real start times). */
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Excision", 0, DAY_A, 1410, -1, false));
    pack_add(&p, mk_set("Sullivan King", 0, DAY_A, 1350, -1, false));

    TEST_ASSERT_FALSE(ff_sched_day_tbd(&p, DAY_A));
}

static void S07_2026_08_24_day_tbd_true_when_start_is_null_even_with_a_known_end(void)
{
    /* Converse mutation check: end_min plays NO part in the TBD
     * predicate at all, in either direction — a set with a known end
     * but no known start still counts as untimed. */
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Mystery Opener", 0, DAY_A, -1, 660, false));

    TEST_ASSERT_TRUE(ff_sched_day_tbd(&p, DAY_A));
}

/* ======================================================================
 * 2026-08-25 review fixup (PR #65, finding 1) — duplicate start_min on
 * the same stage must not double-render. sched_next_stage_start already
 * excludes a tied sibling as ITS OWN derivation source (so the derived
 * effective_end value is the same regardless of which tied set you ask
 * about), but that alone doesn't stop BOTH tied sets from being
 * reported "now" for that identical window on the same stage, which
 * violates ff_sched_now_playing's own "one row per stage" contract.
 * ==================================================================== */

static void S07_2026_08_25_duplicate_start_same_stage_dedupes(void)
{
    /* Reviewer's exact repro: two sets, same stage, same day_doy, both
     * start_min=1200; a third set at 1260 supplies the derived end. */
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Duplicate A", 0, DAY_A, 1200, -1, false));
    pack_add(&p, mk_set("Duplicate B", 0, DAY_A, 1200, -1, false));
    pack_add(&p, mk_set("Next Act", 0, DAY_A, 1260, -1, false));

    ff_now_row_t rows[4];
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 1230, rows, 4);
    /* Exactly one row for this stage — "Duplicate A" wins (lower set
     * index), same "ties -> lower index" rule as next_starred/
     * alarm_tick, not two rows for one stage. */
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_STRING("Duplicate A", rows[0].set->artist);
    TEST_ASSERT_TRUE(rows[0].pct_valid);
    TEST_ASSERT_EQUAL_UINT8(50, rows[0].pct_done);
}

static void S07_2026_08_25_duplicate_start_different_stage_both_render(void)
{
    /* Negative control for the fix above: the dedupe is scoped to
     * (stage_idx, day_doy, start_min) together — two DIFFERENT stages
     * sharing a start_min is ordinary concurrent scheduling, not a
     * duplicate, and both must still render. Guards against an
     * over-broad fix that drops the stage_idx comparison. */
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Canyon Headliner", 0, DAY_A, 1200, -1, false));
    pack_add(&p, mk_set("Hilltop Headliner", 1, DAY_A, 1200, -1, false));

    ff_now_row_t rows[4];
    uint8_t n = ff_sched_now_playing(&p, DAY_A, 1230, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(2, n);
    TEST_ASSERT_EQUAL_STRING("Canyon Headliner", rows[0].set->artist);
    TEST_ASSERT_EQUAL_STRING("Hilltop Headliner", rows[1].set->artist);
}

static void S07_2026_08_24_bass_canyon_shape_fixture_is_not_tbd(void)
{
    char buf[256u * 1024u];
    char path[512];
    snprintf(path, sizeof(path), "%sbass-canyon-shape.festpack.json", FP_FIXTURE_DIR);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    size_t len = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    TEST_ASSERT_TRUE(len > 0);

    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);
    TEST_ASSERT_EQUAL_UINT16(6, pack.n_sets);

    uint16_t day = pack.start_doy;

    /* The exact bug: real published start times, every end_min null —
     * must NOT read as SET TIMES TBD. */
    TEST_ASSERT_FALSE_MESSAGE(ff_sched_day_tbd(&pack, day),
                               "starts-only real festpack shape must not render as SET TIMES TBD");

    /* 22:00 (1320): NGHTMRE mid-set on canyon (derived end from Sullivan
     * King's 22:30), Big Gigantic just starting on hilltop (derived end
     * from Dodge & Fuski's 23:00) — two stages, independently derived. */
    ff_now_row_t rows[8];
    uint8_t n = ff_sched_now_playing(&pack, day, 1320, rows, 8);
    TEST_ASSERT_EQUAL_UINT8(2, n);
    TEST_ASSERT_EQUAL_STRING("NGHTMRE", rows[0].set->artist);
    TEST_ASSERT_TRUE(rows[0].pct_valid);
    TEST_ASSERT_EQUAL_STRING("Big Gigantic", rows[1].set->artist);
    TEST_ASSERT_TRUE(rows[1].pct_valid);
    TEST_ASSERT_EQUAL_UINT8(0, rows[1].pct_done);

    /* 23:45 (1425): Excision live on canyon (last set of the night on
     * that stage), pct genuinely unknown. */
    n = ff_sched_now_playing(&pack, day, 1425, rows, 8);
    TEST_ASSERT_EQUAL_UINT8(2, n);
    TEST_ASSERT_EQUAL_STRING("Excision", rows[0].set->artist);
    TEST_ASSERT_FALSE(rows[0].pct_valid);
    TEST_ASSERT_EQUAL_STRING("Dodge & Fuski", rows[1].set->artist);
    TEST_ASSERT_FALSE(rows[1].pct_valid);

    /* The one fully-null set is still in the day's lineup (unaffected by
     * this amendment — ff_sched_day_sets lists regardless of times). */
    fp_set_t const *day_sets[16];
    uint16_t n_day = ff_sched_day_sets(&pack, day, day_sets, 16);
    TEST_ASSERT_EQUAL_UINT16(6, n_day);
    bool found_brainrack = false;
    for (uint16_t i = 0; i < n_day; i++) {
        if (strcmp(day_sets[i]->artist, "Brainrack B2B Wiley") == 0) found_brainrack = true;
    }
    TEST_ASSERT_TRUE(found_brainrack);
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

    /* Last live minute (effective_end - 1 = 1499): high but not 100
     * (half-open — same rule as the same-day case). */
    n = ff_sched_now_playing(&p, DAY_A, 1499, rows, 4);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_UINT8(98, rows[0].pct_done); /* (89*100)/90 = 98 */
    TEST_ASSERT_EQUAL_INT16(1, rows[0].mins_left);

    /* Exactly at effective_end (01:00, now_min=1500): half-open -> gone. */
    n = ff_sched_now_playing(&p, DAY_A, 1500, rows, 4);
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
     * boundary itself, same half-open start_min<=now_min<end comparison
     * as any other minute. */
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

static void S07_AC3_tie_break_equal_start_min_prefers_lower_index(void)
{
    fp_pack_t p;
    pack_init(&p);
    /* Two starred sets with an IDENTICAL start_min — locks in the
     * documented "ties -> lower set index" contract. A `<` -> `<=`
     * mutation in the tie-break comparison would flip this to prefer
     * the higher index and this test would catch it. */
    pack_add(&p, mk_set("Lower Index", 0, DAY_A, 700, 760, true));
    pack_add(&p, mk_set("Higher Index", 1, DAY_A, 700, 760, true));

    ff_next_t next;
    bool found = ff_sched_next_starred(&p, DAY_A, 600, &next);
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_STRING("Lower Index", next.set->artist);
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

    /* alarm=NULL: caller with no live alarm state (e.g. UI editing the
     * pack) — pure in-memory star flip only, per ff_sched.h. */
    TEST_ASSERT_FALSE(p.sets[0].starred);
    ff_sched_toggle_star(&p, 0, NULL);
    TEST_ASSERT_TRUE(p.sets[0].starred);
    ff_sched_toggle_star(&p, 0, NULL);
    TEST_ASSERT_FALSE(p.sets[0].starred);

    /* Out-of-range index: no-op, no crash. */
    ff_sched_toggle_star(&p, 999, NULL);
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

static void S07_AC4_tie_break_equal_start_min_prefers_lower_index(void)
{
    fp_pack_t p;
    pack_init(&p);
    /* Two starred sets with an IDENTICAL start_min, both due on the same
     * tick — locks in "ties -> lower set index"; each call fires exactly
     * one, lower index first. A `<` -> `<=` mutation in the tie-break
     * comparison would flip this ordering and this test would catch it. */
    pack_add(&p, mk_set("Lower Index", 0, DAY_A, 900, 960, true));
    pack_add(&p, mk_set("Higher Index", 1, DAY_A, 900, 960, true));

    ff_sched_alarm_t st;
    ff_sched_alarm_init(&st);

    fp_set_t const *first = ff_sched_alarm_tick(&st, &p, DAY_A, 890);
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_STRING("Lower Index", first->artist);

    fp_set_t const *second = ff_sched_alarm_tick(&st, &p, DAY_A, 890);
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_EQUAL_STRING("Higher Index", second->artist);
}

static void S07_AC4_unstar_restar_rearms(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Star", 0, DAY_A, 900, 960, true));

    ff_sched_alarm_t st;
    ff_sched_alarm_init(&st);

    /* Fires once at T-15. */
    TEST_ASSERT_NOT_NULL(ff_sched_alarm_tick(&st, &p, DAY_A, 885));
    TEST_ASSERT_NULL(ff_sched_alarm_tick(&st, &p, DAY_A, 886));

    /* Fat-finger: un-star (threading the live alarm state through, so
     * the fired-bit clears immediately per ff_sched.h's ## Amendments
     * note), then re-star, still within the T-15 window. */
    ff_sched_toggle_star(&p, 0, &st);
    TEST_ASSERT_FALSE(p.sets[0].starred);
    ff_sched_toggle_star(&p, 0, &st);
    TEST_ASSERT_TRUE(p.sets[0].starred);

    /* Re-arm: fires again since we're still within the due window —
     * this is the "un-star must clear the fired bit" guarantee, not an
     * accident of tick timing. */
    fp_set_t const *refired = ff_sched_alarm_tick(&st, &p, DAY_A, 887);
    TEST_ASSERT_NOT_NULL(refired);
    TEST_ASSERT_EQUAL_STRING("Star", refired->artist);
}

static void S07_AC4_unstar_without_alarm_arg_still_rearms_on_next_tick(void)
{
    fp_pack_t p;
    pack_init(&p);
    pack_add(&p, mk_set("Star", 0, DAY_A, 900, 960, true));

    ff_sched_alarm_t st;
    ff_sched_alarm_init(&st);

    TEST_ASSERT_NOT_NULL(ff_sched_alarm_tick(&st, &p, DAY_A, 885));

    /* Un-star/re-star WITHOUT threading `st` through (alarm=NULL) — the
     * caller has no live alarm state handy. ff_sched_alarm_tick's own
     * self-clear-on-unstarred sweep still catches this on the next tick
     * while the set is unstarred. */
    ff_sched_toggle_star(&p, 0, NULL);
    TEST_ASSERT_FALSE(p.sets[0].starred);
    TEST_ASSERT_NULL(ff_sched_alarm_tick(&st, &p, DAY_A, 886)); /* unstarred: nothing fires, but bit clears */
    ff_sched_toggle_star(&p, 0, NULL);
    TEST_ASSERT_TRUE(p.sets[0].starred);

    fp_set_t const *refired = ff_sched_alarm_tick(&st, &p, DAY_A, 887);
    TEST_ASSERT_NOT_NULL(refired);
    TEST_ASSERT_EQUAL_STRING("Star", refired->artist);
}

static void S07_AC4_star_after_t15_crossing_fires_on_next_tick(void)
{
    fp_pack_t p;
    pack_init(&p);
    /* Not starred at fixture time: the user stars it late. */
    pack_add(&p, mk_set("LateStar", 0, DAY_A, 900, 960, false));

    ff_sched_alarm_t st;
    ff_sched_alarm_init(&st);

    /* At T-15, unstarred: nothing fires (and nothing to fire). */
    TEST_ASSERT_NULL(ff_sched_alarm_tick(&st, &p, DAY_A, 885));

    /* Star it late, at T-10 — after the T-15 threshold already passed. */
    ff_sched_toggle_star(&p, 0, &st);
    TEST_ASSERT_TRUE(p.sets[0].starred);

    /* Fires on the very next tick: no "already-seen-unstarred" latch
     * suppresses a late star. */
    fp_set_t const *fired = ff_sched_alarm_tick(&st, &p, DAY_A, 890);
    TEST_ASSERT_NOT_NULL(fired);
    TEST_ASSERT_EQUAL_STRING("LateStar", fired->artist);
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
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);
    TEST_ASSERT_TRUE_MESSAGE(pack.n_sets > 0, "fixture unexpectedly has no sets");

    /* festival.start ("2026-09-18") is a day every set's "day" can be
     * compared against; use the festival's own start_doy so this test
     * doesn't hardcode a day-of-year computation. */
    uint16_t day = pack.start_doy;

    TEST_ASSERT_TRUE_MESSAGE(ff_sched_day_tbd(&pack, day),
                              "expected the real Lost Lands fixture to still be all-null (SET TIMES TBD) — "
                              "if this now fails, the fixture has been updated with real set times and "
                              "S07-now-face.md's AC5/AC6 goldens (lineup_tbd.json) likely need attention too");

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
    RUN_TEST(S07_AC1_pct_done_leaves_now_exactly_at_end_half_open);
    RUN_TEST(S07_AC1_zero_gap_changeover_single_row);
    RUN_TEST(S07_AC1_pct_done_midpoint);
    RUN_TEST(S07_AC1_null_start_sets_never_appear);
    RUN_TEST(S07_AC1_a_known_start_with_null_end_still_appears_now);
    RUN_TEST(S07_AC1_max_cap_truncates_in_pack_order);
    RUN_TEST(S07_AC1_day_scoping_excludes_other_days);

    RUN_TEST(S07_2026_08_24_null_end_derives_from_next_same_stage_start);
    RUN_TEST(S07_2026_08_24_derived_end_is_half_open_at_the_changeover_minute);
    RUN_TEST(S07_2026_08_24_last_set_of_day_is_live_with_pct_invalid);
    RUN_TEST(S07_2026_08_24_derivation_is_scoped_to_same_stage_only);
    RUN_TEST(S07_2026_08_24_derivation_is_scoped_to_same_day_only);
    RUN_TEST(S07_2026_08_24_next_starred_allows_a_null_end_set);
    RUN_TEST(S07_2026_08_24_alarm_fires_for_a_null_end_starred_set);
    RUN_TEST(S07_2026_08_24_alarm_does_not_fire_stale_past_the_day_window_end);
    RUN_TEST(S07_2026_08_24_day_tbd_false_when_a_set_has_a_start_but_no_end);
    RUN_TEST(S07_2026_08_24_day_tbd_true_when_start_is_null_even_with_a_known_end);
    RUN_TEST(S07_2026_08_25_duplicate_start_same_stage_dedupes);
    RUN_TEST(S07_2026_08_25_duplicate_start_different_stage_both_render);
    RUN_TEST(S07_2026_08_24_bass_canyon_shape_fixture_is_not_tbd);

    RUN_TEST(S07_AC2_midnight_crossing_set_is_now_at_0030);
    RUN_TEST(S07_AC2_midnight_crossing_pct_done_across_midnight);
    RUN_TEST(S07_AC2_midnight_crossing_set_not_visible_on_next_day_doy);
    RUN_TEST(S07_AC2_0600_boundary_both_sides);

    RUN_TEST(S07_AC3_picks_earliest_future_starred);
    RUN_TEST(S07_AC3_false_when_none_starred);
    RUN_TEST(S07_AC3_false_when_starred_sets_are_past_or_live);
    RUN_TEST(S07_AC3_ignores_null_time_starred_sets);
    RUN_TEST(S07_AC3_ignores_other_days);
    RUN_TEST(S07_AC3_tie_break_equal_start_min_prefers_lower_index);

    RUN_TEST(test_toggle_star_flips_and_is_bounds_checked);

    RUN_TEST(S07_AC4_fires_once_at_t15_crossing);
    RUN_TEST(S07_AC4_idempotent_no_refire_on_retick);
    RUN_TEST(S07_AC4_two_stars_five_min_apart_fire_in_order);
    RUN_TEST(S07_AC4_both_due_in_one_tick_fire_in_start_order_across_calls);
    RUN_TEST(S07_AC4_does_not_fire_for_a_set_already_ended);
    RUN_TEST(S07_AC4_ignores_unstarred_and_null_time_sets);
    RUN_TEST(S07_AC4_reinit_clears_fired_state);
    RUN_TEST(S07_AC4_tie_break_equal_start_min_prefers_lower_index);
    RUN_TEST(S07_AC4_unstar_restar_rearms);
    RUN_TEST(S07_AC4_unstar_without_alarm_arg_still_rearms_on_next_tick);
    RUN_TEST(S07_AC4_star_after_t15_crossing_fires_on_next_tick);

    RUN_TEST(S07_AC5_all_null_pack_now_playing_is_empty);
    RUN_TEST(S07_AC5_all_null_pack_next_starred_is_false);
    RUN_TEST(S07_AC5_day_tbd_true_when_all_null);
    RUN_TEST(S07_AC5_day_tbd_false_when_any_known_time);
    RUN_TEST(S07_AC5_day_tbd_false_when_day_has_no_sets);
    RUN_TEST(test_day_sets_lists_all_including_null_times_in_pack_order);
    RUN_TEST(S07_AC5_real_lost_lands_fixture_is_all_null_tbd);

    return UNITY_END();
}
