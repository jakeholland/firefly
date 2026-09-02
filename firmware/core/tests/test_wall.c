/**
 * test_wall.c — S16 slice b0: wall-clock derivation.
 *
 * Criteria covered (docs/specs/S16-app-shell.md, "Acceptance criteria"):
 *   AC12  — FF_WALL_UNKNOWN before any timestamp (and what it gates);
 *           a NodeInfo `last_heard` latches the offset (the bootstrap
 *           path); (day_doy, now_min) then resolves per ff_sched's
 *           festival-day mapping, including 01:00 local -> previous
 *           day_doy at now_min == 1500.
 *   AC12b — a timestamp outside the plausibility window
 *           [FF_WALL_EPOCH_FLOOR, FF_WALL_EPOCH_CEILING) is rejected and
 *           leaves src == FF_WALL_UNKNOWN, with no pack loaded, and
 *           cannot overwrite a good latch; once latched, a reading
 *           disagreeing by > 30 s re-latches and one disagreeing by less
 *           does not.
 *   AC12c — offset resolution order: an assumed pack offset does not
 *           outrank a set settings offset, a stated one does, and with
 *           neither set the answer is UNKNOWN rather than a guess.
 *           Includes the ff_settings_t UTC-offset field's [api]
 *           round-trip through ff_settings_load/save.
 *
 * The rest of AC12 — "the Now face renders its unknown-time state" — is
 * a render assertion and belongs to the face's goldens, not to this
 * pure-math unit. What IS asserted here is the half this module owns:
 * the UNKNOWN state is reachable, is sticky until a plausible timestamp
 * arrives, and carries no usable now_min for quiet hours or the water
 * nudge to consume.
 *
 * Reference timestamps below are Lost Lands 2026 (Sep 18-20, EDT =
 * UTC-240), cross-checked against Python's datetime before being frozen
 * here. Sep 18 2026 is day-of-year 261, Sep 19 is 262.
 *
 * The civil-date arithmetic additionally has a differential test against
 * Python's datetime over a large random sweep —
 * firmware/tools/dev/wall_crosscheck.py. It is not a ctest because it
 * needs a compiler and an interpreter at once; run it after touching the
 * date math. Hand-written expectations in this file share their author's
 * misconceptions with the code; that harness does not.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "ff_settings.h"
#include "ff_store.h"
#include "ff_wall.h"

/* Drift guard. ff_wall.h's FF_WALL_DAY_START_MIN duplicates
 * ff_sched.h's FF_SCHED_FESTIVAL_DAY_START_MIN because core cannot
 * include festpack (docs/ARCHITECTURE.md's one-way edge; see ff_wall.h's
 * placement note). This test binary is the one build unit that can see
 * both headers, so the equality is checked at compile time here rather
 * than trusted. If ff_sched ever moves the festival-day boundary, this
 * fails to compile instead of silently mapping every festival day off by
 * hours. */
#include "ff_sched.h"
_Static_assert(FF_WALL_DAY_START_MIN == FF_SCHED_FESTIVAL_DAY_START_MIN,
               "ff_wall's festival-day boundary drifted from ff_sched's");

void setUp(void) {}
void tearDown(void) {}

/* Lost Lands 2026, EDT. */
#define EDT ((int16_t)-240)

/* 2026-09-19T05:00:00Z == 01:00 EDT Sep 19 -> festival day Sep 18 (261), now_min 1500. */
#define T_SEP19_0100_EDT ((int64_t)1789794000)
/* 2026-09-19T09:59:00Z == 05:59 EDT Sep 19 -> festival day Sep 18 (261), now_min 1799. */
#define T_SEP19_0559_EDT ((int64_t)1789811940)
/* 2026-09-19T10:00:00Z == 06:00 EDT Sep 19 -> festival day Sep 19 (262), now_min 360. */
#define T_SEP19_0600_EDT ((int64_t)1789812000)
/* 2026-09-19T18:30:00Z == 14:30 EDT Sep 19 -> festival day Sep 19 (262), now_min 870. */
#define T_SEP19_1430_EDT ((int64_t)1789842600)
/* 2026-09-18T22:00:00Z == 18:00 EDT Sep 18 -> festival day Sep 18 (261), now_min 1080. */
#define T_SEP18_1800_EDT ((int64_t)1789768800)
/* 2027-01-01T05:00:00Z == 01:00 EDT Jan 1 2027 -> festival day Dec 31 2026 (365). */
#define T_JAN01_0100_EDT ((int64_t)1798779600)

/* A pack that parsed OK and STATES its offset. */
static ff_wall_offset_cfg_t cfg_pack_stated(int16_t off)
{
    ff_wall_offset_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.pack_loaded = true;
    c.pack_offset_min = off;
    c.pack_offset_assumed = false;
    return c;
}

/* A pack that parsed OK but whose offset is fp_parse's -240 default —
 * utc_offset_min populated, utc_offset_assumed true (fp_pack.c:523). */
static ff_wall_offset_cfg_t cfg_pack_assumed(void)
{
    ff_wall_offset_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.pack_loaded = true;
    c.pack_offset_min = -240;
    c.pack_offset_assumed = true;
    return c;
}

static ff_wall_offset_cfg_t cfg_none(void)
{
    ff_wall_offset_cfg_t c;
    memset(&c, 0, sizeof(c));
    return c;
}

/* ---------------------------------------------------------------------
 * AC12 — UNKNOWN is the starting state, and what it gates.
 * ------------------------------------------------------------------- */

static void S16_AC12_before_any_timestamp_src_is_unknown(void)
{
    ff_wall_state_t st;
    ff_wall_init(&st);

    ff_wall_offset_cfg_t cfg = cfg_pack_stated(EDT); /* offset known, time is not */

    ff_wall_t w = ff_wall_now(&st, 12345u, &cfg);
    TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, w.src);

    /* Every other field is meaningless AND zeroed — no boot time, no
     * 00:00, nothing a careless consumer could render as a clock. */
    TEST_ASSERT_EQUAL_UINT16(0, w.day_doy);
    TEST_ASSERT_EQUAL_INT16(0, w.now_min);
    TEST_ASSERT_FALSE(w.offset_assumed);

    /* Sticky: ticking the monotonic clock forward never invents a time. */
    for (uint32_t t = 0; t < 100000u; t += 5000u) {
        TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_wall_now(&st, t, &cfg).src);
    }

    int64_t unix_s = 0;
    TEST_ASSERT_FALSE(ff_wall_unix_now(&st, 12345u, &unix_s));
}

static void S16_AC12_unknown_time_gates_quiet_hours_and_water_nudge(void)
{
    /* The rule this asserts: in FF_WALL_UNKNOWN there is no honest
     * now_min, so a caller must not evaluate quiet hours and must not
     * tick the water nudge. Modelled here the way the shell will do it
     * (b1) — a single `src == FF_WALL_UNKNOWN` guard — and the assertion
     * is that the guard holds for a whole simulated night and that the
     * nudge therefore never fires. */
    ff_wall_state_t st;
    ff_wall_init(&st);
    ff_wall_offset_cfg_t cfg = cfg_pack_stated(EDT);

    ff_settings_t s;
    memset(&s, 0, sizeof(s));
    s.water_min = 1; /* the most eager possible nudge */

    ff_water_state_t water;
    ff_water_state_init(&water);

    bool nudged = false;
    int quiet_evaluations = 0;

    for (uint32_t t = 0; t < 6u * 60u * 60u * 1000u; t += 60000u) { /* 6 h of ticks */
        ff_wall_t w = ff_wall_now(&st, t, &cfg);
        if (w.src == FF_WALL_UNKNOWN) {
            continue; /* the honesty guard: nothing downstream may run */
        }
        quiet_evaluations++;
        (void)ff_quiet_now(&s, w.now_min);
        nudged = nudged || ff_water_tick(&water, &s, w.now_min);
    }

    TEST_ASSERT_EQUAL_INT(0, quiet_evaluations);
    TEST_ASSERT_FALSE(nudged);

    /* And the same loop, once a timestamp has latched, does run — so the
     * gate above is the UNKNOWN state, not a broken loop. */
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_LATCHED, ff_wall_observe(&st, T_SEP19_1430_EDT, 0u, FF_WALL_TRUST_BOOTSTRAP));
    ff_wall_t w = ff_wall_now(&st, 60000u, &cfg);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    (void)ff_quiet_now(&s, w.now_min);
}

/* ---------------------------------------------------------------------
 * AC12 — the bootstrap: a NodeInfo `last_heard` latches the offset.
 * ------------------------------------------------------------------- */

static void S16_AC12_nodeinfo_last_heard_latches_the_offset(void)
{
    ff_wall_state_t st;
    ff_wall_init(&st);
    ff_wall_offset_cfg_t cfg = cfg_pack_stated(EDT);

    /* mc_nodeinfo_t.last_heard is uint32_t unix seconds (mc_client.h:107)
     * and is populated unconditionally on the on_node path
     * (mc_client.c:230), including during the want_config replay — which
     * is why it, and not rx_time, is the bootstrap source. rx_time rides
     * only live over-the-air packets: mc_client.c:222 hardcodes
     * has_rx_time = false for NodeInfo. Both are plain unix seconds
     * here, so both are expressible through this one entry point. */
    uint32_t last_heard = (uint32_t)T_SEP19_1430_EDT;

    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_LATCHED, ff_wall_observe(&st, (int64_t)last_heard, 8000u, FF_WALL_TRUST_BOOTSTRAP));

    ff_wall_t w = ff_wall_now(&st, 8000u, &cfg);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_EQUAL_UINT16(262, w.day_doy);
    TEST_ASSERT_EQUAL_INT16(870, w.now_min); /* 14:30 */
    TEST_ASSERT_FALSE(w.offset_assumed);
}

static void S16_AC12_wall_advances_with_the_monotonic_clock(void)
{
    ff_wall_state_t st;
    ff_wall_init(&st);
    ff_wall_offset_cfg_t cfg = cfg_pack_stated(EDT);

    ff_wall_observe(&st, T_SEP19_1430_EDT, 1000u, FF_WALL_TRUST_BOOTSTRAP);

    /* 90 monotonic minutes later -> 16:00 local, same festival day. */
    ff_wall_t w = ff_wall_now(&st, 1000u + 90u * 60u * 1000u, &cfg);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_EQUAL_UINT16(262, w.day_doy);
    TEST_ASSERT_EQUAL_INT16(960, w.now_min);
}

static void S16_AC12_monotonic_wraparound_is_handled(void)
{
    ff_wall_state_t st;
    ff_wall_init(&st);
    ff_wall_offset_cfg_t cfg = cfg_pack_stated(EDT);

    /* Latch 30 s before the uint32_t millisecond counter wraps, then ask
     * 60 s later — i.e. 30 s past the wrap. Unsigned subtraction makes
     * this a 60000 ms delta, not a 4.29e9 one. */
    uint32_t latch_ms = 0xFFFFFFFFu - 30000u;
    uint32_t query_ms = latch_ms + 60000u; /* wraps deliberately */
    TEST_ASSERT_TRUE(query_ms < latch_ms); /* the wrap really happened */

    ff_wall_observe(&st, T_SEP19_1430_EDT, latch_ms, FF_WALL_TRUST_BOOTSTRAP);

    ff_wall_t w = ff_wall_now(&st, query_ms, &cfg);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_EQUAL_UINT16(262, w.day_doy);
    TEST_ASSERT_EQUAL_INT16(871, w.now_min); /* 14:30 + 60 s = 14:31 */
}

/* ---------------------------------------------------------------------
 * AC12 — the ff_sched festival-day mapping.
 * ------------------------------------------------------------------- */

static void S16_AC12_local_mapping_follows_ff_sched_festival_day(void)
{
    struct {
        int64_t unix_s;
        uint16_t want_doy;
        int16_t want_now_min;
        char const *what;
    } const cases[] = {
        /* The headline case from the spec: 01:00 local belongs to the
         * PREVIOUS day_doy at now_min == 1500. */
        {T_SEP19_0100_EDT, 261, 1500, "01:00 -> previous festival day"},
        /* The boundary minute either side of the 06:00 roll. */
        {T_SEP19_0559_EDT, 261, 1799, "05:59 -> previous day, top of the window"},
        {T_SEP19_0600_EDT, 262, 360, "06:00 -> the day rolls"},
        /* An ordinary afternoon and an ordinary evening. */
        {T_SEP19_1430_EDT, 262, 870, "14:30"},
        {T_SEP18_1800_EDT, 261, 1080, "18:00"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint16_t doy = 0;
        int16_t now_min = 0;
        TEST_ASSERT_TRUE_MESSAGE(ff_wall_split_local(cases[i].unix_s, EDT, &doy, &now_min),
                                 cases[i].what);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(cases[i].want_doy, doy, cases[i].what);
        TEST_ASSERT_EQUAL_INT16_MESSAGE(cases[i].want_now_min, now_min, cases[i].what);

        /* ff_sched.h's window: now_min is always in [360, 1800). */
        TEST_ASSERT_TRUE(now_min >= FF_SCHED_FESTIVAL_DAY_START_MIN);
        TEST_ASSERT_TRUE(now_min < FF_SCHED_FESTIVAL_DAY_START_MIN + FF_SCHED_DAY_SPAN_MIN);
    }
}

static void S16_AC12_festival_day_rolls_back_across_the_year_boundary(void)
{
    /* 01:00 local on Jan 1 2027 is festival day Dec 31 2026 — day-of-year
     * 365, not 0 and not 366 (2026 is not a leap year). The "previous
     * day" step has to cross a year, which a naive `doy - 1` gets wrong. */
    uint16_t doy = 0;
    int16_t now_min = 0;
    TEST_ASSERT_TRUE(ff_wall_split_local(T_JAN01_0100_EDT, EDT, &doy, &now_min));
    TEST_ASSERT_EQUAL_UINT16(365, doy);
    TEST_ASSERT_EQUAL_INT16(1500, now_min);
}

static void S16_AC12_every_minute_of_a_festival_day_maps_into_the_window(void)
{
    /* Sweep a full 24 h at minute granularity: exactly one day_doy
     * transition, and now_min covers [360, 1800) once, in order. */
    int64_t base = T_SEP19_0600_EDT; /* 06:00 EDT, start of festival day 262 */
    uint16_t prev_doy = 0;
    int16_t prev_now_min = -1;
    int transitions = 0;

    for (int m = 0; m < 1440; m++) {
        uint16_t doy = 0;
        int16_t now_min = 0;
        TEST_ASSERT_TRUE(ff_wall_split_local(base + (int64_t)m * 60, EDT, &doy, &now_min));
        TEST_ASSERT_EQUAL_INT16((int16_t)(360 + m), now_min);
        TEST_ASSERT_EQUAL_UINT16(262, doy);
        if (m > 0 && doy != prev_doy) {
            transitions++;
        }
        TEST_ASSERT_TRUE(m == 0 || now_min == prev_now_min + 1);
        prev_doy = doy;
        prev_now_min = now_min;
    }
    TEST_ASSERT_EQUAL_INT(0, transitions); /* one festival day, start to end */

    /* And the very next minute rolls to the next festival day at 360. */
    uint16_t doy = 0;
    int16_t now_min = 0;
    TEST_ASSERT_TRUE(ff_wall_split_local(base + 1440 * 60, EDT, &doy, &now_min));
    TEST_ASSERT_EQUAL_UINT16(263, doy);
    TEST_ASSERT_EQUAL_INT16(360, now_min);
}

/* ---------------------------------------------------------------------
 * AC12b — the plausibility gate and the re-latch.
 * ------------------------------------------------------------------- */

static void S16_AC12b_timestamp_before_epoch_floor_is_rejected(void)
{
    /* Deliberately with NO pack loaded, per the spec: the primary gate
     * must work during the want_config handshake, before any pack
     * exists. The offset source here is a settings value. */
    ff_wall_state_t st;
    ff_wall_init(&st);

    ff_wall_offset_cfg_t cfg = cfg_none();
    cfg.settings_offset_set = true;
    cfg.settings_offset_min = EDT;

    /* The uncorrected-RTC shapes: the protobuf default, the Unix epoch,
     * a 2016-era RTC, and the last second before the floor. */
    int64_t const rtc_lies[] = {0, 1, 1451606400 /* 2016-01-01 */, FF_WALL_EPOCH_FLOOR - 1};

    for (size_t i = 0; i < sizeof(rtc_lies) / sizeof(rtc_lies[0]); i++) {
        TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_REJECTED, ff_wall_observe(&st, rtc_lies[i], 1000u, FF_WALL_TRUST_BOOTSTRAP));
        TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_wall_now(&st, 1000u, &cfg).src);
    }

    /* The floor itself is a time ("any timestamp BEFORE it" is not). */
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_LATCHED, ff_wall_observe(&st, FF_WALL_EPOCH_FLOOR, 1000u, FF_WALL_TRUST_BOOTSTRAP));
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, ff_wall_now(&st, 1000u, &cfg).src);
}

static void S16_AC12b_timestamp_at_or_after_epoch_ceiling_is_rejected(void)
{
    /* The top half of the window, mirroring the floor test above. Also
     * with no pack loaded: both bounds must hold during the want_config
     * handshake. */
    ff_wall_state_t st;
    ff_wall_init(&st);

    ff_wall_offset_cfg_t cfg = cfg_none();
    cfg.settings_offset_set = true;
    cfg.settings_offset_min = EDT;

    /* Year 2100, the 2038-rollover neighbourhood, and INT32_MAX seconds —
     * all of which a corrupt or hostile node can put in `last_heard`. */
    int64_t const impossible[] = {4102444800LL /* 2100-01-01 */, 2147483647LL, FF_WALL_EPOCH_CEILING};

    for (size_t i = 0; i < sizeof(impossible) / sizeof(impossible[0]); i++) {
        TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_REJECTED, ff_wall_observe(&st, impossible[i], 1000u, FF_WALL_TRUST_BOOTSTRAP));
        TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_wall_now(&st, 1000u, &cfg).src);
    }

    /* Half-open, so the last second below the ceiling is still a time. */
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_LATCHED, ff_wall_observe(&st, FF_WALL_EPOCH_CEILING - 1, 1000u, FF_WALL_TRUST_BOOTSTRAP));
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, ff_wall_now(&st, 1000u, &cfg).src);
}

static void S16_AC12b_far_future_reading_cannot_overwrite_a_good_latch(void)
{
    /* The attack the ceiling exists for (PR #37 review, D1). `unix_s`
     * arrives as mc_nodeinfo_t.last_heard — straight off the radio, from
     * an unpaired node, with no handshake. Without an upper bound, the
     * re-latch path would accept a year-2100 reading, DESTROY a correct
     * latch, and keep asserting FF_WALL_MESH over it. And since
     * ff_wall_t carries no year, it would render as an ordinary festival
     * evening rather than as anything visibly wrong. */
    ff_wall_state_t st;
    ff_wall_init(&st);
    ff_wall_offset_cfg_t cfg = cfg_pack_stated(EDT);

    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_LATCHED, ff_wall_observe(&st, T_SEP19_1430_EDT, 1000u, FF_WALL_TRUST_BOOTSTRAP));

    int64_t const hostile[] = {4102444800LL /* 2100 */, FF_WALL_EPOCH_CEILING, 0, 1451606400LL};

    for (size_t i = 0; i < sizeof(hostile) / sizeof(hostile[0]); i++) {
        TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_REJECTED, ff_wall_observe(&st, hostile[i], 2000u, FF_WALL_TRUST_BOOTSTRAP));

        /* The good latch is byte-for-byte intact and still answers. */
        ff_wall_t w = ff_wall_now(&st, 2000u, &cfg);
        TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
        TEST_ASSERT_EQUAL_UINT16(262, w.day_doy);
        TEST_ASSERT_EQUAL_INT16(870, w.now_min); /* still 14:30 — 1 s of monotonic elapsed */
    }
}

static void S16_AC12b_split_local_enforces_the_same_window(void)
{
    /* ff_wall_split_local documents the window as part of its public
     * contract, and D1 makes that check load-bearing rather than merely
     * belt-and-braces: the two entry points must not be able to disagree
     * about what counts as a time. (PR #37 review, D4.) */
    uint16_t doy = 0;
    int16_t now_min = 0;

    TEST_ASSERT_FALSE(ff_wall_split_local(FF_WALL_EPOCH_FLOOR - 1, EDT, &doy, &now_min));
    TEST_ASSERT_FALSE(ff_wall_split_local(0, EDT, &doy, &now_min));
    TEST_ASSERT_FALSE(ff_wall_split_local(FF_WALL_EPOCH_CEILING, EDT, &doy, &now_min));
    TEST_ASSERT_FALSE(ff_wall_split_local(4102444800LL, EDT, &doy, &now_min));

    /* Both inclusive/exclusive boundaries, matching ff_wall_observe. */
    TEST_ASSERT_TRUE(ff_wall_split_local(FF_WALL_EPOCH_FLOOR, EDT, &doy, &now_min));
    TEST_ASSERT_TRUE(ff_wall_split_local(FF_WALL_EPOCH_CEILING - 1, EDT, &doy, &now_min));
}

static void S16_AC12b_rejection_never_disturbs_a_good_latch(void)
{
    ff_wall_state_t st;
    ff_wall_init(&st);
    ff_wall_offset_cfg_t cfg = cfg_pack_stated(EDT);

    ff_wall_observe(&st, T_SEP19_1430_EDT, 1000u, FF_WALL_TRUST_BOOTSTRAP);

    /* A node with no last_heard (0 = unknown, mc_client.h:107) must not
     * be able to knock the puck back into UNKNOWN. */
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_REJECTED, ff_wall_observe(&st, 0, 2000u, FF_WALL_TRUST_BOOTSTRAP));

    ff_wall_t w = ff_wall_now(&st, 2000u, &cfg);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_EQUAL_INT16(870, w.now_min);
}

static void S16_AC12b_disagreement_over_30s_relatches(void)
{
    ff_wall_state_t st;
    ff_wall_init(&st);

    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_LATCHED, ff_wall_observe(&st, T_SEP19_1430_EDT, 1000u, FF_WALL_TRUST_BOOTSTRAP));

    /* 10 monotonic seconds later the mesh reports a time 31 s further on
     * than the latch predicts — a step, not jitter. */
    uint32_t later_ms = 1000u + 10000u;
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_RELATCHED,
                          ff_wall_observe(&st, T_SEP19_1430_EDT + 10 + 31, later_ms, FF_WALL_TRUST_TRUSTED));

    int64_t unix_s = 0;
    TEST_ASSERT_TRUE(ff_wall_unix_now(&st, later_ms, &unix_s));
    TEST_ASSERT_EQUAL_INT64(T_SEP19_1430_EDT + 41, unix_s);

    /* Backwards steps too — GPS lock can correct in either direction. */
    uint32_t later2_ms = later_ms + 10000u;
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_RELATCHED,
                          ff_wall_observe(&st, T_SEP19_1430_EDT + 41 + 10 - 31, later2_ms, FF_WALL_TRUST_TRUSTED));
    TEST_ASSERT_TRUE(ff_wall_unix_now(&st, later2_ms, &unix_s));
    TEST_ASSERT_EQUAL_INT64(T_SEP19_1430_EDT + 20, unix_s);
}

static void S16_AC12b_disagreement_within_30s_does_not_relatch(void)
{
    ff_wall_state_t st;
    ff_wall_init(&st);

    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_LATCHED, ff_wall_observe(&st, T_SEP19_1430_EDT, 1000u, FF_WALL_TRUST_BOOTSTRAP));

    uint32_t later_ms = 1000u + 10000u;

    /* Exactly at the tolerance, both directions: agreement, and the
     * latch is deliberately left alone rather than chasing the sample. */
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_AGREED,
                          ff_wall_observe(&st, T_SEP19_1430_EDT + 10 + 30, later_ms, FF_WALL_TRUST_BOOTSTRAP));
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_AGREED,
                          ff_wall_observe(&st, T_SEP19_1430_EDT + 10 - 30, later_ms, FF_WALL_TRUST_BOOTSTRAP));
    /* And ordinary transport jitter. */
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_AGREED, ff_wall_observe(&st, T_SEP19_1430_EDT + 10 + 2, later_ms, FF_WALL_TRUST_BOOTSTRAP));

    int64_t unix_s = 0;
    TEST_ASSERT_TRUE(ff_wall_unix_now(&st, later_ms, &unix_s));
    TEST_ASSERT_EQUAL_INT64(T_SEP19_1430_EDT + 10, unix_s); /* unmoved */
}

static void S16_AC12b_pre_gps_lock_offset_is_corrected_by_relatch(void)
{
    /* The scenario the re-latch exists for, end to end. The comms brain
     * boots with an uncorrected RTC reading an hour slow, hands that out
     * during the want_config handshake, then GPS locks and its clock
     * STEPS. A latch-once implementation would assert FF_WALL_MESH over
     * a time that is confidently, permanently wrong. */
    ff_wall_state_t st;
    ff_wall_init(&st);
    ff_wall_offset_cfg_t cfg = cfg_pack_stated(EDT);

    int64_t rtc_slow = T_SEP19_1430_EDT - 3600; /* reports 13:30 local */
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_LATCHED, ff_wall_observe(&st, rtc_slow, 5000u, FF_WALL_TRUST_BOOTSTRAP));
    TEST_ASSERT_EQUAL_INT16(810, ff_wall_now(&st, 5000u, &cfg).now_min); /* 13:30 — wrong */

    /* 20 s later, GPS locks and the reported time jumps forward an hour. */
    uint32_t after_lock_ms = 5000u + 20000u;
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_RELATCHED,
                          ff_wall_observe(&st, T_SEP19_1430_EDT + 20, after_lock_ms, FF_WALL_TRUST_TRUSTED));

    ff_wall_t w = ff_wall_now(&st, after_lock_ms, &cfg);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_EQUAL_UINT16(262, w.day_doy);
    TEST_ASSERT_EQUAL_INT16(870, w.now_min); /* 14:30 — corrected */
}

static void S16_AC12b_expired_latch_reads_unknown_not_a_wrapped_time(void)
{
    ff_wall_state_t st;
    ff_wall_init(&st);
    ff_wall_offset_cfg_t cfg = cfg_pack_stated(EDT);

    ff_wall_observe(&st, T_SEP19_1430_EDT, 0u, FF_WALL_TRUST_BOOTSTRAP);

    /* Inside the window the answer stands. */
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, ff_wall_now(&st, FF_WALL_LATCH_MAX_AGE_MS, &cfg).src);

    /* Past it, a uint32_t monotonic delta can no longer be told apart
     * from a wrapped one, so the honest answer is UNKNOWN — not a time
     * that is silently ~49 days out. */
    TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_wall_now(&st, FF_WALL_LATCH_MAX_AGE_MS + 1u, &cfg).src);

    /* A backwards-moving monotonic clock lands in the same place rather
     * than producing a far-future time. */
    ff_wall_init(&st);
    ff_wall_observe(&st, T_SEP19_1430_EDT, 1000000u, FF_WALL_TRUST_BOOTSTRAP);
    TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_wall_now(&st, 500000u, &cfg).src);

    /* And a fresh plausible reading re-latches out of that state. */
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_RELATCHED,
                          ff_wall_observe(&st, T_SEP19_1430_EDT, 500000u, FF_WALL_TRUST_BOOTSTRAP));
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, ff_wall_now(&st, 500000u, &cfg).src);
}

static void S16_AC12b_backwards_clock_detection_bound_is_exactly_as_documented(void)
{
    /* ff_clock_t promises a monotonically nondecreasing clock, so a
     * backwards step is a platform contract violation. When one happens
     * anyway, detection holds only below FF_WALL_BACKWARD_DETECT_LIMIT_MS
     * (= 2^32 - FF_WALL_LATCH_MAX_AGE_MS). This test pins BOTH sides,
     * including the negative — an earlier revision of the header claimed
     * a blanket guarantee, and on this module an overstated guarantee is
     * worse than a precisely stated limit (PR #37 review, D2). If the
     * age limit is ever retuned, the failing side of this test is the
     * thing that forces the header's number to be retuned with it. */
    ff_wall_offset_cfg_t cfg = cfg_pack_stated(EDT);
    /* Latch at the top of the lap so both cases below are a plain
     * smaller-than-the-latch clock reading, not themselves a wrap. */
    uint32_t const latch_ms = 0xFFFFFFFFu;

    /* Just inside the bound: detected, and the answer degrades to
     * UNKNOWN rather than to a wrong time. */
    {
        ff_wall_state_t st;
        ff_wall_init(&st);
        ff_wall_observe(&st, T_SEP19_1430_EDT, latch_ms, FF_WALL_TRUST_BOOTSTRAP);
        uint32_t back = latch_ms - (FF_WALL_BACKWARD_DETECT_LIMIT_MS - 1u);
        TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_wall_now(&st, back, &cfg).src);
    }

    /* At the bound and beyond: NOT detected. The wrapped delta lands back
     * inside the accepted window and reads as a forward jump. This is the
     * documented residual blind spot — asserted, not glossed over. */
    {
        ff_wall_state_t st;
        ff_wall_init(&st);
        ff_wall_observe(&st, T_SEP19_1430_EDT, latch_ms, FF_WALL_TRUST_BOOTSTRAP);
        uint32_t back = latch_ms - FF_WALL_BACKWARD_DETECT_LIMIT_MS;
        TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, ff_wall_now(&st, back, &cfg).src);
    }

    /* The complement identity the two constants must satisfy: an age
     * limit and a backwards-detection limit that together span exactly
     * one uint32_t lap. Neither can be retuned without the other. */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)0x100000000ULL,
                             (uint64_t)FF_WALL_LATCH_MAX_AGE_MS + (uint64_t)FF_WALL_BACKWARD_DETECT_LIMIT_MS);
}

/* ---------------------------------------------------------------------
 * S18 slice a — trust-gated re-latch (docs/specs/S18-wall-clock-trust.md,
 * issue #49). `ff_wall_observe` grows a trust tier: establishing accepts
 * any tier, a disagreeing re-latch of a FRESH latch requires TRUSTED,
 * AGREED and the expired-latch branch both stay trust-blind.
 * ------------------------------------------------------------------- */

static void S18_AC1_bootstrap_from_unpaired_still_works(void)
{
    /* Cold start: an empty state, a BOOTSTRAP-tier reading (exactly what
     * an unpaired/never-heard node offers), and nothing else has ever
     * latched. Establishing must accept it regardless of tier. */
    ff_wall_state_t st;
    ff_wall_init(&st);
    ff_wall_offset_cfg_t cfg = cfg_pack_stated(EDT);

    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_LATCHED,
                          ff_wall_observe(&st, T_SEP19_1430_EDT, 1000u, FF_WALL_TRUST_BOOTSTRAP));

    ff_wall_t const w = ff_wall_now(&st, 1000u, &cfg);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_EQUAL_INT16(870, w.now_min); /* 14:30 */
    TEST_ASSERT_EQUAL_UINT32(0u, ff_wall_trust_rejected_count(&st)); /* nothing was ever refused */
}

static void S18_AC2_second_stranger_cannot_move_an_already_latched_clock(void)
{
    /* The exact #49 scenario, at the core layer: a first unpaired
     * stranger bootstraps the latch (necessarily — that is what
     * BOOTSTRAP is for), and a SECOND unpaired stranger then disagrees by
     * more than the delta. The headline fix: REJECTED, and the latch —
     * established off a stranger in the first place — is untouched. This
     * must hold specifically because the ORIGINATING latch was itself
     * BOOTSTRAP-tier: the gate conditions on the incoming observation's
     * tier, never on what tier established the latch (ff_wall_state_t
     * tracks no provenance — the S18 spec is explicit that an upgrade
     * path here is a hole, not a feature). */
    ff_wall_state_t st;
    ff_wall_init(&st);
    ff_wall_offset_cfg_t cfg = cfg_pack_stated(EDT);

    /* Stranger #1 bootstraps. */
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_LATCHED,
                          ff_wall_observe(&st, T_SEP19_1430_EDT, 1000u, FF_WALL_TRUST_BOOTSTRAP));
    int16_t const now_min_before = ff_wall_now(&st, 11000u, &cfg).now_min;

    /* Stranger #2, ten monotonic seconds later, reports a time two hours
     * behind the latch — issue #49's measured repro shape (rx_time two
     * hours behind, now_min shifted from 1200 to 1080). Comfortably past
     * FF_WALL_RELATCH_DELTA_S (30 s), and still BOOTSTRAP tier: unpaired,
     * never heard from before. */
    int64_t const stranger2_unix = T_SEP19_1430_EDT + 10 - 7200;
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_REJECTED,
                          ff_wall_observe(&st, stranger2_unix, 11000u, FF_WALL_TRUST_BOOTSTRAP));

    /* now_min is UNCHANGED — the whole point. Every crew member's
     * freshness on the Radar face is measured against this same value. */
    ff_wall_t const w = ff_wall_now(&st, 11000u, &cfg);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_EQUAL_INT16(now_min_before, w.now_min);

    int64_t unix_s = 0;
    TEST_ASSERT_TRUE(ff_wall_unix_now(&st, 11000u, &unix_s));
    TEST_ASSERT_EQUAL_INT64(T_SEP19_1430_EDT + 10, unix_s); /* stranger #1's prediction, untouched */

    /* AC5: the rejection is counted, not a silent no-op. */
    TEST_ASSERT_EQUAL_UINT32(1u, ff_wall_trust_rejected_count(&st));
}

static void S18_AC3_a_trusted_source_disagreeing_relatches(void)
{
    /* Mirror of AC2, with the SAME shape of disagreement, but from a
     * TRUSTED source (shell-classified: a paired member, or self's own
     * GPS-disciplined clock) — the backwards-GPS-step correction case
     * must stay reachable. */
    ff_wall_state_t st;
    ff_wall_init(&st);
    ff_wall_offset_cfg_t cfg = cfg_pack_stated(EDT);

    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_LATCHED,
                          ff_wall_observe(&st, T_SEP19_1430_EDT, 1000u, FF_WALL_TRUST_BOOTSTRAP));

    int64_t const trusted_unix = T_SEP19_1430_EDT + 10 - 7200;
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_RELATCHED,
                          ff_wall_observe(&st, trusted_unix, 11000u, FF_WALL_TRUST_TRUSTED));

    ff_wall_t const w = ff_wall_now(&st, 11000u, &cfg);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_EQUAL_INT16(750, w.now_min); /* 12:30 — the corrected, two-hours-back time */

    /* A TRUSTED relatch is not a "rejection" by any name. */
    TEST_ASSERT_EQUAL_UINT32(0u, ff_wall_trust_rejected_count(&st));
}

static void S18_AC5_trust_rejected_count_counts_only_the_trust_gate(void)
{
    /* The proxy risk named in the standing brief: a counter that
     * increments on EVERY rejection (including plausibility-window
     * rejections, which have nothing to do with trust and fire during
     * ordinary handshake noise) would be useless for "is a stranger
     * trying to move my clock" and would also false-alarm constantly.
     * Pin that it increments ONLY for the disagreeing-fresh-latch/
     * insufficient-tier case. */
    ff_wall_state_t st;
    ff_wall_init(&st);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_wall_trust_rejected_count(&st));

    /* A plausibility-window rejection before anything has latched: not
     * counted — this is a different gate entirely. */
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_REJECTED,
                          ff_wall_observe(&st, 0 /* unknown */, 1000u, FF_WALL_TRUST_BOOTSTRAP));
    TEST_ASSERT_EQUAL_UINT32(0u, ff_wall_trust_rejected_count(&st));

    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_LATCHED,
                          ff_wall_observe(&st, T_SEP19_1430_EDT, 1000u, FF_WALL_TRUST_BOOTSTRAP));

    /* An AGREED observation, even from a BOOTSTRAP source: not counted —
     * nothing was refused, nothing moved. */
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_AGREED,
                          ff_wall_observe(&st, T_SEP19_1430_EDT + 10, 11000u, FF_WALL_TRUST_BOOTSTRAP));
    TEST_ASSERT_EQUAL_UINT32(0u, ff_wall_trust_rejected_count(&st));

    /* A plausibility-window rejection against an EXISTING good latch:
     * still not counted — implausible input, not a trust failure. */
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_REJECTED,
                          ff_wall_observe(&st, FF_WALL_EPOCH_CEILING, 12000u, FF_WALL_TRUST_BOOTSTRAP));
    TEST_ASSERT_EQUAL_UINT32(0u, ff_wall_trust_rejected_count(&st));

    /* The actual trust-gate rejection: counted, exactly once. */
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_REJECTED,
                          ff_wall_observe(&st, T_SEP19_1430_EDT + 10 + 7200, 13000u, FF_WALL_TRUST_BOOTSTRAP));
    TEST_ASSERT_EQUAL_UINT32(1u, ff_wall_trust_rejected_count(&st));

    /* And a second one accumulates rather than saturating or resetting. */
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_REJECTED,
                          ff_wall_observe(&st, T_SEP19_1430_EDT + 10 - 7200, 14000u, FF_WALL_TRUST_BOOTSTRAP));
    TEST_ASSERT_EQUAL_UINT32(2u, ff_wall_trust_rejected_count(&st));

    /* NULL `st` reads as 0, not a crash. */
    TEST_ASSERT_EQUAL_UINT32(0u, ff_wall_trust_rejected_count(NULL));
}

static void S18_expired_latch_relatch_stays_trust_blind(void)
{
    /* The one branch S18 slice a deliberately does NOT gate (design-
     * review PR #88): once the latch has expired (or the monotonic clock
     * moved backwards), ANY plausible reading re-latches — including a
     * bare BOOTSTRAP-tier one overwriting a latch that was previously
     * TRUSTED. If a future change "fixes" this into a gated branch, this
     * test is the regression pin: it would start returning REJECTED
     * instead of RELATCHED. */
    ff_wall_state_t st;
    ff_wall_init(&st);
    ff_wall_offset_cfg_t cfg = cfg_pack_stated(EDT);

    /* A TRUSTED source establishes and holds the latch. */
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_LATCHED,
                          ff_wall_observe(&st, T_SEP19_1430_EDT, 1000u, FF_WALL_TRUST_TRUSTED));

    /* Let it expire (past FF_WALL_LATCH_MAX_AGE_MS). */
    uint32_t const expired_ms = 1000u + FF_WALL_LATCH_MAX_AGE_MS + 1u;
    TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_wall_now(&st, expired_ms, &cfg).src);

    /* A disagreeing BOOTSTRAP reading still re-latches — the expired
     * branch never reaches the trust gate at all. */
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_RELATCHED,
                          ff_wall_observe(&st, T_SEP19_1430_EDT + 3600, expired_ms, FF_WALL_TRUST_BOOTSTRAP));

    ff_wall_t const w = ff_wall_now(&st, expired_ms, &cfg);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_EQUAL_INT16(930, w.now_min); /* 15:30 — the new BOOTSTRAP value took effect */
    TEST_ASSERT_EQUAL_UINT32(0u, ff_wall_trust_rejected_count(&st)); /* never touched the gate */
}

/* ---------------------------------------------------------------------
 * S18 slice c (#40) — pack-derived plausibility window. Here: the core
 * seam (the default window, the ff_wall_set_window setter, the exposed
 * ff_wall_unix_from_doy civil-date math, and the build-date proximity
 * guard). The festpack->window derivation itself is app-layer and lives
 * in test_wall_window.c; the shell wiring is in test_shell.c.
 * ------------------------------------------------------------------- */

static void S18c_AC1_default_window_is_the_fixed_bootstrap_window(void)
{
    /* A fresh state gates on the FIXED window — this is what MUST hold
     * during the want_config handshake before any pack loads (AC1). A
     * timestamp just inside each fixed bound is accepted; the bounds
     * themselves behave exactly as the pre-slice-c gate did. */
    ff_wall_state_t st;
    ff_wall_init(&st);
    TEST_ASSERT_EQUAL_INT64(FF_WALL_EPOCH_FLOOR, st.win_floor);
    TEST_ASSERT_EQUAL_INT64(FF_WALL_EPOCH_CEILING, st.win_ceiling);

    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_LATCHED,
                          ff_wall_observe(&st, FF_WALL_EPOCH_FLOOR, 1000u, FF_WALL_TRUST_BOOTSTRAP));

    ff_wall_state_t below;
    ff_wall_init(&below);
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_REJECTED,
                          ff_wall_observe(&below, FF_WALL_EPOCH_FLOOR - 1, 1000u, FF_WALL_TRUST_BOOTSTRAP));

    ff_wall_state_t at_ceiling;
    ff_wall_init(&at_ceiling);
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_REJECTED,
                          ff_wall_observe(&at_ceiling, FF_WALL_EPOCH_CEILING, 1000u, FF_WALL_TRUST_BOOTSTRAP));
}

static void S18c_set_window_tightens_and_refuses_degenerate(void)
{
    ff_wall_state_t st;
    ff_wall_init(&st);

    /* Install a tight window well inside the fixed one. */
    int64_t const floor_s = FF_WALL_EPOCH_FLOOR + 1000000;
    int64_t const ceiling_s = floor_s + 5000000;
    TEST_ASSERT_TRUE(ff_wall_set_window(&st, floor_s, ceiling_s));
    TEST_ASSERT_EQUAL_INT64(floor_s, st.win_floor);
    TEST_ASSERT_EQUAL_INT64(ceiling_s, st.win_ceiling);

    /* A value inside the fixed window but below the tightened floor is now
     * rejected — the tighten took effect. */
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_REJECTED,
                          ff_wall_observe(&st, FF_WALL_EPOCH_FLOOR + 1, 1000u, FF_WALL_TRUST_BOOTSTRAP));
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_LATCHED,
                          ff_wall_observe(&st, floor_s + 1, 1000u, FF_WALL_TRUST_BOOTSTRAP));

    /* A degenerate (empty/inverted) window is REFUSED and leaves the
     * window exactly as it was — never install a gate that rejects every
     * reading. */
    TEST_ASSERT_FALSE(ff_wall_set_window(&st, ceiling_s, floor_s)); /* inverted */
    TEST_ASSERT_FALSE(ff_wall_set_window(&st, floor_s, floor_s));   /* empty */
    TEST_ASSERT_EQUAL_INT64(floor_s, st.win_floor);   /* unchanged */
    TEST_ASSERT_EQUAL_INT64(ceiling_s, st.win_ceiling);
    TEST_ASSERT_FALSE(ff_wall_set_window(NULL, floor_s, ceiling_s)); /* NULL st */

    /* Resetting to the fixed bounds is an ordinary valid set. */
    TEST_ASSERT_TRUE(ff_wall_set_window(&st, FF_WALL_EPOCH_FLOOR, FF_WALL_EPOCH_CEILING));
    TEST_ASSERT_EQUAL_INT64(FF_WALL_EPOCH_FLOOR, st.win_floor);
    TEST_ASSERT_EQUAL_INT64(FF_WALL_EPOCH_CEILING, st.win_ceiling);
}

static void S18c_set_window_moves_only_the_window_not_the_latch(void)
{
    /* Narrowing the window must never retroactively un-latch a good time:
     * only FUTURE observations are gated. Latch on the fixed window, then
     * tighten to a window that EXCLUDES the already-latched value; the
     * derived time is unchanged. */
    ff_wall_state_t st;
    ff_wall_init(&st);
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_LATCHED,
                          ff_wall_observe(&st, FF_WALL_EPOCH_FLOOR + 100, 1000u, FF_WALL_TRUST_BOOTSTRAP));

    int64_t before = 0, after = 0;
    TEST_ASSERT_TRUE(ff_wall_unix_now(&st, 1000u, &before));

    /* A window that starts well above the latched value. */
    TEST_ASSERT_TRUE(ff_wall_set_window(&st, FF_WALL_EPOCH_FLOOR + 1000000, FF_WALL_EPOCH_CEILING));
    TEST_ASSERT_TRUE(ff_wall_unix_now(&st, 1000u, &after));
    TEST_ASSERT_EQUAL_INT64(before, after); /* latch untouched */
}

static void S18c_unix_from_doy_is_the_shared_civil_date_math(void)
{
    /* doy 1 of 1970 is the epoch; doy is linear so +1 doy is +86400 s. The
     * pack->window derivation is computed against exactly this, so the two
     * cannot drift. (Fuller anchor coverage is in test_wall_window.c.) */
    TEST_ASSERT_EQUAL_INT64(0, ff_wall_unix_from_doy(1970, 1));
    TEST_ASSERT_EQUAL_INT64(86400, ff_wall_unix_from_doy(1970, 2));
    /* 2026-09-18 00:00 UTC (doy 261) — the frozen Lost Lands start anchor,
     * = U_EVENING-style refs minus their time-of-day. */
    TEST_ASSERT_EQUAL_INT64((int64_t)1789689600, ff_wall_unix_from_doy(2026, 261));
}

static void S18c_AC4_build_date_proximity_guard_fires_within_12_months(void)
{
    /* AC4: the guard is a proximity alarm driven by a SYNTHETIC build date,
     * not the real clock — deterministic, never calendar-flaky. It fires
     * once the build date is within FF_WALL_CEILING_WARN_LEAD_S (12 months)
     * of the ceiling, and is off before that. */
    int64_t const lead = FF_WALL_CEILING_WARN_LEAD_S;

    /* Comfortably clear: two years out — off. */
    TEST_ASSERT_FALSE(ff_wall_ceiling_deadline_near(FF_WALL_EPOCH_CEILING - 2 * lead));
    /* Six months out — inside the 12-month lead, fires. */
    TEST_ASSERT_TRUE(ff_wall_ceiling_deadline_near(FF_WALL_EPOCH_CEILING - lead / 2));

    /* The threshold is inclusive at exactly 12 months before the ceiling. */
    TEST_ASSERT_TRUE(ff_wall_ceiling_deadline_near(FF_WALL_EPOCH_CEILING - lead));
    /* One second before the threshold — still off. */
    TEST_ASSERT_FALSE(ff_wall_ceiling_deadline_near(FF_WALL_EPOCH_CEILING - lead - 1));

    /* Past the ceiling entirely — very much on. */
    TEST_ASSERT_TRUE(ff_wall_ceiling_deadline_near(FF_WALL_EPOCH_CEILING + 1));
}

/* ---------------------------------------------------------------------
 * AC12c — offset resolution order.
 * ------------------------------------------------------------------- */

static void S16_AC12c_assumed_pack_offset_does_not_outrank_set_settings(void)
{
    ff_wall_offset_cfg_t cfg = cfg_pack_assumed(); /* fp_parse's -240 guess */
    cfg.settings_offset_set = true;
    cfg.settings_offset_min = -420; /* the user configured MDT */

    int16_t off = 0;
    bool assumed = true;
    TEST_ASSERT_TRUE(ff_wall_resolve_offset(&cfg, &off, &assumed));
    TEST_ASSERT_EQUAL_INT16(-420, off);
    TEST_ASSERT_FALSE(assumed);

    /* The same through the composed call: a parser default must not be
     * able to silently move the puck two time zones east. */
    ff_wall_state_t st;
    ff_wall_init(&st);
    ff_wall_observe(&st, T_SEP19_1430_EDT, 0u, FF_WALL_TRUST_BOOTSTRAP);

    ff_wall_t w = ff_wall_now(&st, 0u, &cfg);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_EQUAL_INT16(690, w.now_min); /* 11:30 MDT, not 14:30 EDT */
    TEST_ASSERT_FALSE(w.offset_assumed);
}

static void S16_AC12c_stated_pack_offset_outranks_settings(void)
{
    ff_wall_offset_cfg_t cfg = cfg_pack_stated(EDT); /* the pack SAYS -240 */
    cfg.settings_offset_set = true;
    cfg.settings_offset_min = -420;

    int16_t off = 0;
    bool assumed = true;
    TEST_ASSERT_TRUE(ff_wall_resolve_offset(&cfg, &off, &assumed));
    TEST_ASSERT_EQUAL_INT16(EDT, off);
    TEST_ASSERT_FALSE(assumed);
}

static void S16_AC12c_assumed_pack_offset_is_used_when_settings_unset(void)
{
    ff_wall_offset_cfg_t cfg = cfg_pack_assumed();

    int16_t off = 0;
    bool assumed = false;
    TEST_ASSERT_TRUE(ff_wall_resolve_offset(&cfg, &off, &assumed));
    TEST_ASSERT_EQUAL_INT16(-240, off);
    TEST_ASSERT_TRUE(assumed); /* usable, but flagged as a guess */

    ff_wall_state_t st;
    ff_wall_init(&st);
    ff_wall_observe(&st, T_SEP19_1430_EDT, 0u, FF_WALL_TRUST_BOOTSTRAP);
    ff_wall_t w = ff_wall_now(&st, 0u, &cfg);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_TRUE(w.offset_assumed);
}

static void S16_AC12c_no_pack_and_no_settings_is_unknown(void)
{
    ff_wall_offset_cfg_t cfg = cfg_none();

    int16_t off = 99;
    bool assumed = true;
    TEST_ASSERT_FALSE(ff_wall_resolve_offset(&cfg, &off, &assumed));
    TEST_ASSERT_EQUAL_INT16(99, off); /* untouched on failure */

    /* Even with a perfectly good latched timestamp: knowing the absolute
     * instant is not knowing the local time. UNKNOWN, not a defaulted
     * guess and not UTC. */
    ff_wall_state_t st;
    ff_wall_init(&st);
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_LATCHED, ff_wall_observe(&st, T_SEP19_1430_EDT, 0u, FF_WALL_TRUST_BOOTSTRAP));

    int64_t unix_s = 0;
    TEST_ASSERT_TRUE(ff_wall_unix_now(&st, 0u, &unix_s)); /* the instant IS known */
    TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_wall_now(&st, 0u, &cfg).src);
}

static void S16_AC12c_settings_offset_of_zero_is_a_real_utc_offset(void)
{
    /* The reason utc_offset_set exists at all: 0 is legitimately UTC, so
     * the value alone cannot encode absence. */
    ff_wall_offset_cfg_t cfg = cfg_none();
    cfg.settings_offset_min = 0;

    int16_t off = 99;
    bool assumed = false;
    TEST_ASSERT_FALSE(ff_wall_resolve_offset(&cfg, &off, &assumed)); /* unset */

    cfg.settings_offset_set = true;
    TEST_ASSERT_TRUE(ff_wall_resolve_offset(&cfg, &off, &assumed)); /* set to UTC */
    TEST_ASSERT_EQUAL_INT16(0, off);
    TEST_ASSERT_FALSE(assumed);
}

static void S16_AC12c_unloaded_pack_is_not_a_stated_utc_offset(void)
{
    /* fp_parse memsets *out on any failure, so a zeroed fp_pack_t reads
     * as utc_offset_min = 0 with utc_offset_assumed = false — i.e. as a
     * deliberately STATED offset of UTC. pack_loaded is what stops that
     * phantom from outranking a real settings value. */
    ff_wall_offset_cfg_t cfg = cfg_none();
    cfg.pack_loaded = false;
    cfg.pack_offset_min = 0;
    cfg.pack_offset_assumed = false;
    cfg.settings_offset_set = true;
    cfg.settings_offset_min = EDT;

    int16_t off = 0;
    bool assumed = true;
    TEST_ASSERT_TRUE(ff_wall_resolve_offset(&cfg, &off, &assumed));
    TEST_ASSERT_EQUAL_INT16(EDT, off);
    TEST_ASSERT_FALSE(assumed);
}

static void S16_AC12c_out_of_range_offset_falls_through(void)
{
    /* A corrupt persisted blob or a bad pack must not be able to produce
     * a wrong local time; the source is skipped, not clamped. */
    ff_wall_offset_cfg_t cfg = cfg_pack_stated(9000); /* nonsense */
    cfg.settings_offset_set = true;
    cfg.settings_offset_min = EDT;

    int16_t off = 0;
    bool assumed = true;
    TEST_ASSERT_TRUE(ff_wall_resolve_offset(&cfg, &off, &assumed));
    TEST_ASSERT_EQUAL_INT16(EDT, off);

    /* Nothing usable left at all -> UNKNOWN. */
    ff_wall_offset_cfg_t bad = cfg_pack_stated(-9000);
    bad.settings_offset_set = true;
    bad.settings_offset_min = 32000;
    TEST_ASSERT_FALSE(ff_wall_resolve_offset(&bad, &off, &assumed));

    /* The real-world extremes stay usable. */
    uint16_t doy = 0;
    int16_t now_min = 0;
    TEST_ASSERT_TRUE(ff_wall_split_local(T_SEP19_1430_EDT, FF_WALL_OFFSET_MIN_LO, &doy, &now_min));
    TEST_ASSERT_TRUE(ff_wall_split_local(T_SEP19_1430_EDT, FF_WALL_OFFSET_MIN_HI, &doy, &now_min));
    TEST_ASSERT_FALSE(ff_wall_split_local(T_SEP19_1430_EDT, FF_WALL_OFFSET_MIN_HI + 1, &doy, &now_min));
}

/* ---------------------------------------------------------------------
 * AC12c — the ff_settings_t field itself ([api]).
 * ------------------------------------------------------------------- */

#define MOCK_SLOT_CAP 256

typedef struct {
    bool has_value;
    size_t len;
    uint8_t data[MOCK_SLOT_CAP];
    char key[64];
} mock_store_io_t;

static int mock_get(void *io, char const *key, void *buf, size_t n)
{
    mock_store_io_t *m = (mock_store_io_t *)io;
    if (!m->has_value || strcmp(m->key, key) != 0 || n < m->len) {
        return -1;
    }
    memcpy(buf, m->data, m->len);
    return (int)m->len;
}

static int mock_set(void *io, char const *key, void const *buf, size_t n)
{
    mock_store_io_t *m = (mock_store_io_t *)io;
    if (n > MOCK_SLOT_CAP) {
        return -1;
    }
    memcpy(m->data, buf, n);
    m->len = n;
    m->has_value = true;
    snprintf(m->key, sizeof(m->key), "%s", key);
    return 0;
}

static void S16_AC12c_settings_utc_offset_round_trips_through_the_store(void)
{
    mock_store_io_t io;
    memset(&io, 0, sizeof(io));
    ff_store_t store = {mock_get, mock_set, &io};

    /* Defaults: unset, which is what keeps a never-configured puck
     * honestly UNKNOWN rather than guessing a zone. */
    ff_settings_t fresh;
    memset(&fresh, 0xAA, sizeof(fresh));
    ff_settings_load(&fresh, &store);
    TEST_ASSERT_FALSE(fresh.utc_offset_set);
    TEST_ASSERT_EQUAL_INT16(0, fresh.utc_offset_min);

    /* A configured offset survives save -> load... */
    ff_settings_t out = fresh;
    out.utc_offset_set = true;
    out.utc_offset_min = -420;
    ff_settings_save(&out, &store);

    ff_settings_t in;
    memset(&in, 0xAA, sizeof(in));
    ff_settings_load(&in, &store);
    TEST_ASSERT_TRUE(in.utc_offset_set);
    TEST_ASSERT_EQUAL_INT16(-420, in.utc_offset_min);
    TEST_ASSERT_EQUAL_MEMORY(&out, &in, sizeof(ff_settings_t));

    /* ...including a deliberately-set UTC, which the flag is what makes
     * distinguishable from "never configured". */
    out.utc_offset_min = 0;
    ff_settings_save(&out, &store);
    ff_settings_load(&in, &store);
    TEST_ASSERT_TRUE(in.utc_offset_set);
    TEST_ASSERT_EQUAL_INT16(0, in.utc_offset_min);

    /* And it feeds the resolver directly — the two structs' fields line
     * up by name so slice b1's wiring is a copy, not a translation. */
    ff_wall_offset_cfg_t cfg = cfg_none();
    cfg.settings_offset_set = in.utc_offset_set;
    cfg.settings_offset_min = in.utc_offset_min;

    int16_t off = 99;
    bool assumed = true;
    TEST_ASSERT_TRUE(ff_wall_resolve_offset(&cfg, &off, &assumed));
    TEST_ASSERT_EQUAL_INT16(0, off);
    TEST_ASSERT_FALSE(assumed);
}

/* ---------------------------------------------------------------------
 * Defensive: NULL arguments never produce a time.
 * ------------------------------------------------------------------- */

static void S16_AC12_null_inputs_read_as_unknown(void)
{
    ff_wall_state_t st;
    ff_wall_init(&st);
    ff_wall_observe(&st, T_SEP19_1430_EDT, 0u, FF_WALL_TRUST_BOOTSTRAP);
    ff_wall_offset_cfg_t cfg = cfg_pack_stated(EDT);

    TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_wall_now(NULL, 0u, &cfg).src);
    TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_wall_now(&st, 0u, NULL).src);
    TEST_ASSERT_EQUAL_INT(FF_WALL_OBS_REJECTED, ff_wall_observe(NULL, T_SEP19_1430_EDT, 0u, FF_WALL_TRUST_BOOTSTRAP));

    int64_t unix_s = 0;
    TEST_ASSERT_FALSE(ff_wall_unix_now(NULL, 0u, &unix_s));
    TEST_ASSERT_FALSE(ff_wall_unix_now(&st, 0u, NULL));

    ff_wall_init(NULL); /* must not crash */
}

/* ---------------------------------------------------------------------
 * S21 amendment — ff_fmt_clock, the wall-clock display formatter
 * (docs/specs/S18-wall-clock-trust.md's "display format" note; the field
 * it reads, ff_settings_t.clock_24h, is documented in ff_settings.h).
 * ------------------------------------------------------------------- */

static void S21_fmt_clock_midnight_noon_1pm_both_modes(void)
{
    char buf[FF_WALL_CLOCK_STR_LEN];

    /* Midnight: 00:00, minute 0. Both 12-hour edge cases (midnight and
     * noon) render hour-of-day 0/12 as "12", not "0". */
    ff_fmt_clock(buf, sizeof(buf), 0, true, true);
    TEST_ASSERT_EQUAL_STRING("00:00", buf);
    ff_fmt_clock(buf, sizeof(buf), 0, true, false);
    TEST_ASSERT_EQUAL_STRING("12:00 am", buf);

    /* Noon: 12:00, minute 720. */
    ff_fmt_clock(buf, sizeof(buf), 720, true, true);
    TEST_ASSERT_EQUAL_STRING("12:00", buf);
    ff_fmt_clock(buf, sizeof(buf), 720, true, false);
    TEST_ASSERT_EQUAL_STRING("12:00 pm", buf);

    /* 1 pm: 13:00, minute 780 — the first hour after noon rolls back to
     * "1" (not "13") in 12-hour form. */
    ff_fmt_clock(buf, sizeof(buf), 780, true, true);
    TEST_ASSERT_EQUAL_STRING("13:00", buf);
    ff_fmt_clock(buf, sizeof(buf), 780, true, false);
    TEST_ASSERT_EQUAL_STRING("1:00 pm", buf);

    /* The design vocabulary's own worked example: 9:46 pm, minute
     * 21*60+46 = 1306 — also the no-leading-zero-on-the-hour check
     * ("9:46", never "09:46", in 12-hour form). */
    ff_fmt_clock(buf, sizeof(buf), 1306, true, false);
    TEST_ASSERT_EQUAL_STRING("9:46 pm", buf);
    ff_fmt_clock(buf, sizeof(buf), 1306, true, true);
    TEST_ASSERT_EQUAL_STRING("21:46", buf);
}

static void S21_fmt_clock_unknown_stays_empty_both_modes(void)
{
    char buf[FF_WALL_CLOCK_STR_LEN];

    /* "" is the honest-unknown output (CLAUDE.md) — the caller/screen
     * renders it as "--:--", never an invented time; unchanged by this
     * amendment in either format. */
    memset(buf, 0xAA, sizeof(buf));
    ff_fmt_clock(buf, sizeof(buf), 780, false, true);
    TEST_ASSERT_EQUAL_STRING("", buf);

    memset(buf, 0xAA, sizeof(buf));
    ff_fmt_clock(buf, sizeof(buf), 780, false, false);
    TEST_ASSERT_EQUAL_STRING("", buf);
}

static void S21_fmt_clock_normalizes_minute_of_day(void)
{
    char buf[FF_WALL_CLOCK_STR_LEN];

    /* ff_wall_t.now_min's documented range, [360, 1800), can exceed a
     * single day (1500 falls in it) — must still normalize modulo 1440. */
    ff_fmt_clock(buf, sizeof(buf), 1500, true, true); /* 1500 - 1440 = 60 -> 01:00 */
    TEST_ASSERT_EQUAL_STRING("01:00", buf);

    /* Negative wraps the same way ff_settings.c's own ff_norm_min does. */
    ff_fmt_clock(buf, sizeof(buf), -1, true, true); /* -> 23:59 */
    TEST_ASSERT_EQUAL_STRING("23:59", buf);
}

static void S21_fmt_clock_null_and_zero_len_buf_are_a_no_op(void)
{
    char buf[FF_WALL_CLOCK_STR_LEN];
    memset(buf, 0x55, sizeof(buf));

    ff_fmt_clock(buf, 0, 780, true, true);
    TEST_ASSERT_EQUAL_HEX8(0x55, (uint8_t)buf[0]); /* n == 0: untouched */

    ff_fmt_clock(NULL, sizeof(buf), 780, true, true); /* must not crash */
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S16_AC12_before_any_timestamp_src_is_unknown);
    RUN_TEST(S16_AC12_unknown_time_gates_quiet_hours_and_water_nudge);
    RUN_TEST(S16_AC12_nodeinfo_last_heard_latches_the_offset);
    RUN_TEST(S16_AC12_wall_advances_with_the_monotonic_clock);
    RUN_TEST(S16_AC12_monotonic_wraparound_is_handled);
    RUN_TEST(S16_AC12_local_mapping_follows_ff_sched_festival_day);
    RUN_TEST(S16_AC12_festival_day_rolls_back_across_the_year_boundary);
    RUN_TEST(S16_AC12_every_minute_of_a_festival_day_maps_into_the_window);
    RUN_TEST(S16_AC12_null_inputs_read_as_unknown);

    RUN_TEST(S16_AC12b_timestamp_before_epoch_floor_is_rejected);
    RUN_TEST(S16_AC12b_timestamp_at_or_after_epoch_ceiling_is_rejected);
    RUN_TEST(S16_AC12b_far_future_reading_cannot_overwrite_a_good_latch);
    RUN_TEST(S16_AC12b_split_local_enforces_the_same_window);
    RUN_TEST(S16_AC12b_rejection_never_disturbs_a_good_latch);
    RUN_TEST(S16_AC12b_disagreement_over_30s_relatches);
    RUN_TEST(S16_AC12b_disagreement_within_30s_does_not_relatch);
    RUN_TEST(S16_AC12b_pre_gps_lock_offset_is_corrected_by_relatch);
    RUN_TEST(S16_AC12b_expired_latch_reads_unknown_not_a_wrapped_time);
    RUN_TEST(S16_AC12b_backwards_clock_detection_bound_is_exactly_as_documented);

    RUN_TEST(S18_AC1_bootstrap_from_unpaired_still_works);
    RUN_TEST(S18_AC2_second_stranger_cannot_move_an_already_latched_clock);
    RUN_TEST(S18_AC3_a_trusted_source_disagreeing_relatches);
    RUN_TEST(S18_AC5_trust_rejected_count_counts_only_the_trust_gate);
    RUN_TEST(S18_expired_latch_relatch_stays_trust_blind);

    RUN_TEST(S18c_AC1_default_window_is_the_fixed_bootstrap_window);
    RUN_TEST(S18c_set_window_tightens_and_refuses_degenerate);
    RUN_TEST(S18c_set_window_moves_only_the_window_not_the_latch);
    RUN_TEST(S18c_unix_from_doy_is_the_shared_civil_date_math);
    RUN_TEST(S18c_AC4_build_date_proximity_guard_fires_within_12_months);

    RUN_TEST(S16_AC12c_assumed_pack_offset_does_not_outrank_set_settings);
    RUN_TEST(S16_AC12c_stated_pack_offset_outranks_settings);
    RUN_TEST(S16_AC12c_assumed_pack_offset_is_used_when_settings_unset);
    RUN_TEST(S16_AC12c_no_pack_and_no_settings_is_unknown);
    RUN_TEST(S16_AC12c_settings_offset_of_zero_is_a_real_utc_offset);
    RUN_TEST(S16_AC12c_unloaded_pack_is_not_a_stated_utc_offset);
    RUN_TEST(S16_AC12c_out_of_range_offset_falls_through);
    RUN_TEST(S16_AC12c_settings_utc_offset_round_trips_through_the_store);

    RUN_TEST(S21_fmt_clock_midnight_noon_1pm_both_modes);
    RUN_TEST(S21_fmt_clock_unknown_stays_empty_both_modes);
    RUN_TEST(S21_fmt_clock_normalizes_minute_of_day);
    RUN_TEST(S21_fmt_clock_null_and_zero_len_buf_are_a_no_op);

    return UNITY_END();
}
