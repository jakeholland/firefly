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

/* 2026-09-07 [api] presence-heard-vs-position: FF_CREW_LOST_MS widened
 * 10min -> 20min (1200000ms) — see ff_crew.h's doc comment on the
 * constant. Renamed from the old *_600000ms_* names to match. */
static void S02_AC1_freshness_exactly_1200000ms_is_stale(void)
{
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.has_pos = true;
    m.pos_age_ms = 0;
    TEST_ASSERT_EQUAL(FF_FRESH_STALE, ff_crew_freshness(&m, 1200000u));
}

static void S02_AC1_freshness_just_over_1200000ms_is_lost(void)
{
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.has_pos = true;
    m.pos_age_ms = 0;
    TEST_ASSERT_EQUAL(FF_FRESH_LOST, ff_crew_freshness(&m, 1200001u));
}

static void S02_AC1_freshness_never_when_no_pos_ever(void)
{
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.has_pos = false;
    TEST_ASSERT_EQUAL(FF_FRESH_NEVER, ff_crew_freshness(&m, 999999u));
}

static void S02_AC1_freshness_just_under_1200000ms_is_stale(void)
{
    /* Symmetric to S02_AC1_freshness_exactly_1200000ms_is_stale: the
     * STALE side immediately below the LOST boundary, mirroring the
     * just-under-45s LIVE-side test above. */
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.has_pos = true;
    m.pos_age_ms = 0;
    TEST_ASSERT_EQUAL(FF_FRESH_STALE, ff_crew_freshness(&m, 1199999u));
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
/* AC2 — upsert basics (find-or-create, existing-id stability)          */
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

/* ------------------------------------------------------------------- */
/* AC10 — bounded unpaired-LRU roster eviction (2026-09-11 S02          */
/* amendment, issue #266). Supersedes the old fixed no-eviction policy  */
/* the pre-amendment AC2 tests used to pin (see git history/PR body for */
/* the retired S02_AC2_ninth_... / S02_AC2_set_paired_cannot_exceed...   */
/* tests this group replaces).                                          */
/* ------------------------------------------------------------------- */

static void S02_AC10a_ninth_stranger_evicts_lru_unpaired_and_succeeds(void)
{
    /* Fill the roster with FF_CREW_MAX never-paired strangers, each
     * heard at a distinct, increasing timestamp (id 1 heard first/
     * oldest, id FF_CREW_MAX heard last/newest). Upserting a genuinely
     * new id must now succeed - proof this isn't just "eviction is
     * possible", it's "eviction is what actually happens on a full,
     * all-stranger roster", the exact scenario issue #266 reports. */
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    for (uint32_t i = 1; i <= FF_CREW_MAX; i++) {
        ff_crew_on_heard(&c, i, i * 1000u, true);
    }
    TEST_ASSERT_EQUAL_UINT8(FF_CREW_MAX, c.count);

    fc.t = (FF_CREW_MAX + 1u) * 1000u; /* "now", for the eviction's own age math */
    ff_crew_member_t *ninth = ff_crew_upsert(&c, 999u);
    TEST_ASSERT_NOT_NULL_MESSAGE(ninth, "a full-of-strangers roster must admit a genuinely new node");
    TEST_ASSERT_EQUAL_UINT32(999u, ninth->node_id);
    TEST_ASSERT_EQUAL_UINT8(FF_CREW_MAX, c.count); /* still 8 - reused a slot, didn't grow */

    /* id 1 (oldest last_heard_ms) is the one that should be gone. */
    TEST_ASSERT_NULL(ff_crew_find(&c, 1u));
    /* Every other stranger (2..8) is untouched. */
    for (uint32_t i = 2; i <= FF_CREW_MAX; i++) {
        TEST_ASSERT_NOT_NULL(ff_crew_find(&c, i));
    }
}

static void S02_AC10a_pairing_a_new_node_on_a_full_stranger_roster_succeeds(void)
{
    /* The issue's actual complaint, end to end: an all-stranger-full
     * roster must not block PAIRING a new friend. */
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    for (uint32_t i = 1; i <= FF_CREW_MAX; i++) {
        ff_crew_on_heard(&c, i, i * 1000u, true);
    }

    fc.t = (FF_CREW_MAX + 1u) * 1000u;
    uint32_t const friend_id = 0xF00Du;
    TEST_ASSERT_TRUE(ff_crew_set_paired(&c, friend_id, true));

    ff_crew_member_t const *m = ff_crew_find(&c, friend_id);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_TRUE(m->paired);
    TEST_ASSERT_EQUAL_UINT8(FF_CREW_MAX, c.count);
}

static void S02_AC10b_ninth_pairing_fails_honestly_once_eight_are_paired(void)
{
    /* The one failure case eviction leaves standing: with all
     * FF_CREW_MAX slots genuinely PAIRED, a 9th pairing attempt must
     * fail honestly (not silently evict a paired member). */
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    for (uint32_t i = 1; i <= FF_CREW_MAX; i++) {
        TEST_ASSERT_TRUE(ff_crew_set_paired(&c, i, true));
    }
    TEST_ASSERT_EQUAL_UINT8(FF_CREW_MAX, c.count);

    TEST_ASSERT_NULL(ff_crew_upsert(&c, 999u));
    TEST_ASSERT_FALSE(ff_crew_set_paired(&c, 999u, true));

    /* Nothing in the roster claims the rejected id, and every original
     * paired member is untouched (no silent overwrite, no accidental
     * unpairing). */
    for (uint8_t i = 0; i < c.count; i++) {
        TEST_ASSERT_NOT_EQUAL_UINT32(999u, c.members[i].node_id);
    }
    for (uint32_t i = 1; i <= FF_CREW_MAX; i++) {
        ff_crew_member_t const *m = ff_crew_find(&c, i);
        TEST_ASSERT_NOT_NULL(m);
        TEST_ASSERT_EQUAL_UINT32(i, m->node_id);
        TEST_ASSERT_TRUE(m->paired);
    }
}

static void S02_AC10c_paired_member_never_evicted_no_matter_how_many_strangers(void)
{
    /* Two paired members claim slots 0-1; every subsequent stranger must
     * churn through the REMAINING 6 slots only - the two paired members
     * must never move, vanish, or change identity, regardless of how
     * many distinct new strangers arrive afterward (well past the
     * roster's own capacity). */
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    TEST_ASSERT_TRUE(ff_crew_set_paired(&c, 1u, true));
    TEST_ASSERT_TRUE(ff_crew_set_paired(&c, 2u, true));

    for (uint32_t i = 0; i < 500u; i++) {
        fc.t = 1000u + i;
        ff_crew_on_heard(&c, 10000u + i, fc.t, true);
    }

    TEST_ASSERT_EQUAL_UINT8(FF_CREW_MAX, c.count); /* never grew past 8 */

    ff_crew_member_t const *m1 = ff_crew_find(&c, 1u);
    ff_crew_member_t const *m2 = ff_crew_find(&c, 2u);
    TEST_ASSERT_NOT_NULL(m1);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_TRUE(m1->paired);
    TEST_ASSERT_TRUE(m2->paired);
}

static void S02_AC10d_eviction_order_follows_last_heard_ms_strictly(void)
{
    /* A deliberately NON-insertion-order fixture: id 5 was heard FIRST
     * (oldest) even though it was upserted last, so an implementation
     * that (wrongly) evicts by slot index or insertion order rather than
     * last_heard_ms would evict the wrong id. Fill order: 1,2,3,4,5 but
     * heard-time order (oldest->newest): 5,3,1,4,2. */
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    ff_crew_on_heard(&c, 1u, 300u, true); /* 3rd oldest */
    ff_crew_on_heard(&c, 2u, 500u, true); /* newest */
    ff_crew_on_heard(&c, 3u, 200u, true); /* 2nd oldest */
    ff_crew_on_heard(&c, 4u, 400u, true); /* 4th oldest */
    ff_crew_on_heard(&c, 5u, 100u, true); /* oldest */
    TEST_ASSERT_EQUAL_UINT8(5u, c.count);

    /* Pad to FF_CREW_MAX with three more, newer than all of the above. */
    ff_crew_on_heard(&c, 6u, 600u, true);
    ff_crew_on_heard(&c, 7u, 700u, true);
    ff_crew_on_heard(&c, 8u, 800u, true);
    TEST_ASSERT_EQUAL_UINT8(FF_CREW_MAX, c.count);

    /* Both replacement admissions go through ff_crew_on_heard (not a bare
     * ff_crew_upsert) so every occupant carries real heard evidence
     * throughout - this test isolates last_heard_ms ORDERING; the
     * separate never-heard-is-most-evictable rule has its own test
     * right below. */
    fc.t = 900u;
    ff_crew_on_heard(&c, 900u, 900u, true); /* evicts 5 (oldest: t=100) */
    TEST_ASSERT_NOT_NULL(ff_crew_find(&c, 900u));
    TEST_ASSERT_NULL(ff_crew_find(&c, 5u));
    TEST_ASSERT_NOT_NULL(ff_crew_find(&c, 3u)); /* next-oldest, still present */

    fc.t = 901u;
    ff_crew_on_heard(&c, 901u, 901u, true); /* evicts 3 (now oldest: t=200) */
    TEST_ASSERT_NOT_NULL(ff_crew_find(&c, 901u));
    TEST_ASSERT_NULL(ff_crew_find(&c, 3u));
    TEST_ASSERT_NOT_NULL(ff_crew_find(&c, 1u)); /* next-oldest after that, still present */
}

static void S02_AC10d_never_heard_occupant_is_evicted_before_any_heard_one(void)
{
    /* A slot created purely via ff_crew_upsert (never ff_crew_on_heard)
     * has has_heard == false and must be treated as MORE evictable than
     * any occupant with a real, however-old, last_heard_ms. */
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    /* id 1: heard a long time ago (t=1) - real evidence, just old. */
    ff_crew_on_heard(&c, 1u, 1u, true);
    /* ids 2..8: upserted directly, never heard. */
    for (uint32_t i = 2; i <= FF_CREW_MAX; i++) {
        TEST_ASSERT_NOT_NULL(ff_crew_upsert(&c, i));
    }
    TEST_ASSERT_EQUAL_UINT8(FF_CREW_MAX, c.count);

    fc.t = 100000u;
    TEST_ASSERT_NOT_NULL(ff_crew_upsert(&c, 999u));

    /* id 1 (has_heard == true, just old) must survive; one of the
     * never-heard occupants must be the one that's gone. */
    TEST_ASSERT_NOT_NULL(ff_crew_find(&c, 1u));
    bool any_never_heard_evicted = false;
    for (uint32_t i = 2; i <= FF_CREW_MAX; i++) {
        if (ff_crew_find(&c, i) == NULL) {
            any_never_heard_evicted = true;
        }
    }
    TEST_ASSERT_TRUE(any_never_heard_evicted);
}

static void S02_AC10e_evicting_a_stranger_with_a_position_drops_it_cleanly(void)
{
    /* The evicted occupant had a full record - position, RSSI, status,
     * heard timestamp. After eviction, the REUSED slot must read exactly
     * like a brand-new one: nothing about the old occupant leaks through. */
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    for (uint32_t i = 1; i <= FF_CREW_MAX; i++) {
        ff_crew_on_heard(&c, i, i * 1000u, true);
    }
    /* id 1 (the future eviction victim - oldest heard) gets a rich record. */
    ff_crew_on_position(&c, 1u, (ff_latlon_t){39.9, -82.4}, 1000u, FF_CREW_POS_META_NONE);
    fc.t = 1000u;
    ff_crew_on_rssi(&c, 1u, -55);
    ff_crew_member_t *victim_before = ff_crew_upsert(&c, 1u);
    strcpy(victim_before->status, "RAGING");
    victim_before->battery_pct = 42;

    fc.t = (FF_CREW_MAX + 1u) * 1000u;
    ff_crew_member_t *fresh = ff_crew_upsert(&c, 999u);
    TEST_ASSERT_NOT_NULL(fresh);

    /* The reused slot reads exactly like a brand-new one. */
    TEST_ASSERT_FALSE(fresh->has_pos);
    TEST_ASSERT_FALSE(fresh->has_heard);
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, fresh->rssi_dbm);
    TEST_ASSERT_EQUAL_INT8(-1, fresh->battery_pct);
    TEST_ASSERT_EQUAL_STRING("", fresh->status);
    TEST_ASSERT_FALSE(fresh->paired);

    /* And its RSSI trend history is gone too - not just the scalar
     * rssi_dbm field, the whole ring buffer used to compute the trend. */
    TEST_ASSERT_EQUAL_INT8(0, ff_crew_rssi_trend(&c, 999u, fc.t));

    /* The evicted id, if it ever comes back, starts fresh - not a
     * resurrection of its old record. */
    TEST_ASSERT_NULL(ff_crew_find(&c, 1u));
}

static void S02_AC10f_fuzz_smoke_10k_random_ops_never_corrupts_invariants(void)
{
    /* Deterministic PRNG (no external dependency, reproducible across
     * runs/platforms) driving a bounded id space (0..31, well over
     * FF_CREW_MAX so both hits and misses/evictions are exercised) with
     * a mix of heard/pair/unpair operations. `desired_paired[id]`
     * mirrors this loop's OWN last pair/unpair decision for `id`
     * (independent of whatever core actually did) so the invariant
     * checked every single iteration is exactly issue #266's promise:
     * a currently-paired id can never vanish or read unpaired because
     * of RF noise - only this loop's own explicit unpair can do that. */
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    uint32_t rng = 0x20260911u; /* fixed seed - reproducible */
    bool desired_paired[32];
    memset(desired_paired, 0, sizeof(desired_paired));

    for (uint32_t iter = 0; iter < 10000u; iter++) {
        /* xorshift32 */
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;

        uint32_t const id = rng % 32u;
        uint32_t const op = (rng >> 8) % 3u;
        fc.t = 1u + iter; /* strictly increasing "now" */

        if (op == 0) {
            ff_crew_on_heard(&c, id, fc.t, (rng & 1u) != 0u);
        } else if (op == 1) {
            if (ff_crew_set_paired(&c, id, true)) {
                desired_paired[id] = true;
            }
            /* A failure here means "roster full of 8 already-paired
             * members" (AC10b) - id's desired state is left unchanged
             * (still whatever it was), which is correct: this op didn't
             * happen. */
        } else {
            ff_crew_set_paired(&c, id, false); /* unpairing always succeeds */
            desired_paired[id] = false;
        }

        /* Invariants, checked EVERY iteration, not just at the end -
         * corruption that self-heals before a final-only check would
         * otherwise go unnoticed. */
        TEST_ASSERT_TRUE_MESSAGE(c.count <= FF_CREW_MAX, "count exceeded FF_CREW_MAX");

        for (uint8_t i = 0; i < c.count; i++) {
            for (uint8_t j = (uint8_t)(i + 1u); j < c.count; j++) {
                TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(c.members[i].node_id, c.members[j].node_id,
                                                      "duplicate node_id in roster");
            }
        }

        for (uint32_t did = 0; did < 32u; did++) {
            if (!desired_paired[did]) {
                continue;
            }
            ff_crew_member_t const *m = ff_crew_find(&c, did);
            TEST_ASSERT_NOT_NULL_MESSAGE(m, "a currently-paired id vanished from the roster");
            TEST_ASSERT_TRUE_MESSAGE(m->paired, "a currently-paired id was silently unpaired");
        }
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
/* S29 — ff_crew_on_heard: overwrite semantics + find-or-create parity   */
/* ------------------------------------------------------------------- */

static void S29_on_heard_find_or_creates_slot(void)
{
    ff_crew_t c;
    ff_crew_init(&c, NULL);

    TEST_ASSERT_NULL(ff_crew_find(&c, 9u));
    ff_crew_on_heard(&c, 9u, 1000u, true);

    ff_crew_member_t const *m = ff_crew_find(&c, 9u);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_TRUE(m->has_heard);
    TEST_ASSERT_EQUAL_UINT32(1000u, m->last_heard_ms);
    TEST_ASSERT_TRUE(m->heard_direct);
}

static void S29_on_heard_existing_slot_is_reused_not_duplicated(void)
{
    ff_crew_t c;
    ff_crew_init(&c, NULL);

    ff_crew_member_t *upserted = ff_crew_upsert(&c, 9u);
    ff_crew_on_heard(&c, 9u, 1000u, true);

    TEST_ASSERT_EQUAL_UINT8(1u, c.count);
    TEST_ASSERT_EQUAL_PTR(upserted, ff_crew_find(&c, 9u));
}

static void S29_on_heard_latest_sighting_wins_direct_to_relay(void)
{
    ff_crew_t c;
    ff_crew_init(&c, NULL);

    ff_crew_on_heard(&c, 9u, 1000u, true);
    ff_crew_on_heard(&c, 9u, 5000u, false);

    ff_crew_member_t const *m = ff_crew_find(&c, 9u);
    TEST_ASSERT_TRUE(m->has_heard);
    TEST_ASSERT_EQUAL_UINT32(5000u, m->last_heard_ms);
    TEST_ASSERT_FALSE(m->heard_direct); /* the LATEST sighting, not the direct one, wins */
}

static void S29_on_heard_latest_sighting_wins_relay_to_direct(void)
{
    ff_crew_t c;
    ff_crew_init(&c, NULL);

    ff_crew_on_heard(&c, 9u, 1000u, false);
    ff_crew_on_heard(&c, 9u, 5000u, true);

    ff_crew_member_t const *m = ff_crew_find(&c, 9u);
    TEST_ASSERT_TRUE(m->has_heard);
    TEST_ASSERT_EQUAL_UINT32(5000u, m->last_heard_ms);
    TEST_ASSERT_TRUE(m->heard_direct);
}

static void S29_on_heard_null_crew_is_safe(void)
{
    ff_crew_on_heard(NULL, 9u, 1000u, true); /* must not crash */
}

static void S29_on_heard_leaves_rssi_untouched(void)
{
    /* ff_crew_on_heard is a DIFFERENT fact than ff_crew_on_rssi — a relay
     * sighting must never touch rssi_dbm/rssi_age_ms (see ff_crew.h's
     * doc comment: only ff_crew_on_rssi's direct-only contract owns
     * those fields). */
    ff_crew_t c;
    ff_crew_init(&c, NULL);

    ff_crew_member_t *m = ff_crew_upsert(&c, 9u);
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, m->rssi_dbm); /* never-direct sentinel */

    ff_crew_on_heard(&c, 9u, 1000u, false);
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, m->rssi_dbm); /* still untouched */
    TEST_ASSERT_TRUE(m->has_heard);
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
/* ff_crew_select_node — S10's "GO force-selects the flare sender" seam  */
/* (docs/specs/S10-flare.md, dated amendment 2026-09-02). The direct     */
/* unit coverage; the end-to-end "flare from a non-first-paired member,  */
/* GO, radar target == sender" regression lives in                      */
/* app/tests/test_intent.c against the real ff_shell wiring, since the   */
/* actual call site (ff_flare_go + this function, back to back) is in   */
/* ff_shell.c, not in core.                                              */
/* ------------------------------------------------------------------- */

static void S10_select_node_jumps_directly_to_a_non_adjacent_paired_member(void)
{
    /* The exact shape of the bug this exists to fix: three paired
     * members, the first-paired one (A) is the self-healed selection by
     * default, and the node that needs to become selected (C) is
     * neither the current selection NOR "next" from it. */
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    ff_crew_upsert(&c, 1u); /* A */
    ff_crew_upsert(&c, 2u); /* B */
    ff_crew_upsert(&c, 3u); /* C */
    ff_crew_set_paired(&c, 1u, true);
    ff_crew_set_paired(&c, 2u, true);
    ff_crew_set_paired(&c, 3u, true);

    ff_crew_member_t *sel = ff_crew_selected(&c);
    TEST_ASSERT_EQUAL_UINT32(1u, sel->node_id); /* A, self-healed default */

    ff_crew_select_node(&c, 3u); /* C flared; jump straight to C */
    sel = ff_crew_selected(&c);
    TEST_ASSERT_NOT_NULL(sel);
    TEST_ASSERT_EQUAL_UINT32(3u, sel->node_id);
}

static void S10_select_node_unknown_id_leaves_selection_untouched(void)
{
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    ff_crew_upsert(&c, 1u);
    ff_crew_set_paired(&c, 1u, true);

    ff_crew_select_node(&c, 0xDEADBEEFu); /* never heard of this node */

    ff_crew_member_t *sel = ff_crew_selected(&c);
    TEST_ASSERT_NOT_NULL(sel);
    TEST_ASSERT_EQUAL_UINT32(1u, sel->node_id); /* unchanged */
}

static void S10_select_node_unpaired_id_leaves_selection_untouched(void)
{
    /* A stranger's node_id existing in the roster (merely heard) must
     * never become a valid radar-face selection — same rule
     * ff_crew_selected's own self-heal already enforces. */
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    ff_crew_upsert(&c, 1u);
    ff_crew_upsert(&c, 2u);
    ff_crew_set_paired(&c, 1u, true);
    ff_crew_set_paired(&c, 2u, false); /* heard, not crew */

    ff_crew_select_node(&c, 2u);

    ff_crew_member_t *sel = ff_crew_selected(&c);
    TEST_ASSERT_NOT_NULL(sel);
    TEST_ASSERT_EQUAL_UINT32(1u, sel->node_id); /* unchanged */
}

static void S10_select_node_null_crew_is_safe(void)
{
    ff_crew_select_node(NULL, 1u); /* must not crash */
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

/* ------------------------------------------------------------------- */
/* 2026-09-06 [api] crew long names — ff_crew_display_name             */
/* ------------------------------------------------------------------- */

static void LONGNAME_display_name_prefers_long_when_present(void)
{
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    strcpy(m.name, "TAYL");
    strcpy(m.long_name, "Taylor");

    TEST_ASSERT_EQUAL_STRING("Taylor", ff_crew_display_name(&m));
}

static void LONGNAME_display_name_falls_back_to_short_when_long_empty(void)
{
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    strcpy(m.name, "TAYL");
    /* m.long_name left "" by the memset above. */

    TEST_ASSERT_EQUAL_STRING("TAYL", ff_crew_display_name(&m));
}

static void LONGNAME_display_name_both_empty_is_empty(void)
{
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));

    TEST_ASSERT_EQUAL_STRING("", ff_crew_display_name(&m));
}

static void LONGNAME_display_name_never_synthesizes_from_short(void)
{
    /* Honest-data guard: a member with ONLY a short name must not have a
     * long name invented from it — the short name itself is returned
     * verbatim, not e.g. capitalized/expanded. */
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    strcpy(m.name, "KEV");

    char const *got = ff_crew_display_name(&m);
    TEST_ASSERT_EQUAL_STRING("KEV", got);
    TEST_ASSERT_NOT_EQUAL(0, strcmp(got, "Kevin")); /* never guessed */
}

static void LONGNAME_display_name_null_member_is_safe_empty(void)
{
    TEST_ASSERT_EQUAL_STRING("", ff_crew_display_name(NULL));
}

/* ------------------------------------------------------------------- */
/* HEARD — presence-heard-vs-position (2026-09-07 [api] S02 amendment)  */
/* ------------------------------------------------------------------- */

static void HEARD_never_before_first_on_heard(void)
{
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    TEST_ASSERT_FALSE(m.has_heard);
    TEST_ASSERT_EQUAL(FF_CREW_PRESENCE_NEVER, ff_crew_presence(&m, 999999u));
}

static void HEARD_null_member_is_never(void)
{
    TEST_ASSERT_EQUAL(FF_CREW_PRESENCE_NEVER, ff_crew_presence(NULL, 12345u));
}

static void HEARD_on_heard_finds_or_creates_and_sets_fields(void)
{
    fake_clock_t fc = {0};
    ff_clock_t clk = make_clock(&fc);
    ff_crew_t c;
    ff_crew_init(&c, &clk);

    ff_crew_on_heard(&c, 7u, 5000u, true);
    ff_crew_member_t const *m = ff_crew_find(&c, 7u);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_TRUE(m->has_heard);
    TEST_ASSERT_EQUAL_UINT32(5000u, m->last_heard_ms);
}

static void HEARD_on_heard_null_crew_is_safe(void)
{
    ff_crew_on_heard(NULL, 7u, 5000u, true); /* must not crash */
}

static void HEARD_freshly_heard_is_heard(void)
{
    /* age 0, and just under the HEARD/STALE boundary. */
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.has_heard = true;
    m.last_heard_ms = 1000u;
    TEST_ASSERT_EQUAL(FF_CREW_PRESENCE_HEARD, ff_crew_presence(&m, 1000u));
    TEST_ASSERT_EQUAL(FF_CREW_PRESENCE_HEARD, ff_crew_presence(&m, 1000u + FF_CREW_HEARD_LIVE_MS - 1u));
}

static void HEARD_exactly_heard_live_ms_is_stale(void)
{
    /* Strict inequality on the HEARD side (age < FF_CREW_HEARD_LIVE_MS,
     * per ff_crew.h): the boundary value itself lands in STALE. */
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.has_heard = true;
    m.last_heard_ms = 0u;
    TEST_ASSERT_EQUAL(FF_CREW_PRESENCE_STALE, ff_crew_presence(&m, FF_CREW_HEARD_LIVE_MS));
}

static void HEARD_exactly_heard_lost_ms_is_stale(void)
{
    /* Inclusive toward STALE at the LOST boundary too (age <=
     * FF_CREW_HEARD_LOST_MS), mirroring ff_crew_freshness' own
     * inclusive-toward-STALE convention. */
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.has_heard = true;
    m.last_heard_ms = 0u;
    TEST_ASSERT_EQUAL(FF_CREW_PRESENCE_STALE, ff_crew_presence(&m, FF_CREW_HEARD_LOST_MS));
}

static void HEARD_just_over_heard_lost_ms_is_lost(void)
{
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.has_heard = true;
    m.last_heard_ms = 0u;
    TEST_ASSERT_EQUAL(FF_CREW_PRESENCE_LOST, ff_crew_presence(&m, FF_CREW_HEARD_LOST_MS + 1u));
}

static void HEARD_presence_is_independent_of_position(void)
{
    /* The owner's whole "why are we LOST?" bug, as a predicate test: a
     * member heard 30s ago but with NO position at all (has_pos ==
     * false, hence ff_crew_freshness == NEVER) must read HEARD, not
     * NEVER/LOST — the two axes never conflate. */
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.has_pos = false;
    m.has_heard = true;
    m.last_heard_ms = 0u;
    uint32_t const now = 30u * 1000u;

    TEST_ASSERT_EQUAL(FF_FRESH_NEVER, ff_crew_freshness(&m, now));
    TEST_ASSERT_EQUAL(FF_CREW_PRESENCE_HEARD, ff_crew_presence(&m, now));
}

static void HEARD_stale_position_with_fresh_heard_is_not_position_lost(void)
{
    /* The indoor-no-fix scenario from the owner's investigation: a
     * position aged well past FF_CREW_LOST_MS (position LOST), but
     * NodeInfo/telemetry keep arriving (heard fresh). Both facts are
     * independently true and honest — freshness stays LOST (position IS
     * old), presence stays HEARD (the radio IS still hearing them). */
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.has_pos = true;
    m.pos_age_ms = 0u;
    m.has_heard = true;
    m.last_heard_ms = FF_CREW_LOST_MS; /* heard again right as the old fix was already LOST-aged */
    uint32_t const now = FF_CREW_LOST_MS + 60u * 1000u; /* position now well past LOST */

    TEST_ASSERT_EQUAL(FF_FRESH_LOST, ff_crew_freshness(&m, now));
    TEST_ASSERT_EQUAL(FF_CREW_PRESENCE_HEARD, ff_crew_presence(&m, now));
}

static void HEARD_wraparound_safe(void)
{
    /* Same uint32 rollover convention as S02_AC1_freshness_handles_
     * uint32_wraparound above. */
    ff_crew_member_t m;
    memset(&m, 0, sizeof(m));
    m.has_heard = true;
    m.last_heard_ms = UINT32_MAX - 99u;

    TEST_ASSERT_EQUAL(FF_CREW_PRESENCE_HEARD, ff_crew_presence(&m, 100u)); /* true elapsed: 200ms */
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S02_AC1_freshness_just_under_45s_is_live);
    RUN_TEST(S02_AC1_freshness_exactly_45000ms_is_stale);
    RUN_TEST(S02_AC1_freshness_exactly_1200000ms_is_stale);
    RUN_TEST(S02_AC1_freshness_just_over_1200000ms_is_lost);
    RUN_TEST(S02_AC1_freshness_never_when_no_pos_ever);
    RUN_TEST(S02_AC1_freshness_just_under_1200000ms_is_stale);
    RUN_TEST(S02_AC1_freshness_handles_uint32_wraparound);

    RUN_TEST(S02_AC2_upsert_existing_id_returns_same_slot);

    RUN_TEST(S02_AC10a_ninth_stranger_evicts_lru_unpaired_and_succeeds);
    RUN_TEST(S02_AC10a_pairing_a_new_node_on_a_full_stranger_roster_succeeds);
    RUN_TEST(S02_AC10b_ninth_pairing_fails_honestly_once_eight_are_paired);
    RUN_TEST(S02_AC10c_paired_member_never_evicted_no_matter_how_many_strangers);
    RUN_TEST(S02_AC10d_eviction_order_follows_last_heard_ms_strictly);
    RUN_TEST(S02_AC10d_never_heard_occupant_is_evicted_before_any_heard_one);
    RUN_TEST(S02_AC10e_evicting_a_stranger_with_a_position_drops_it_cleanly);
    RUN_TEST(S02_AC10f_fuzz_smoke_10k_random_ops_never_corrupts_invariants);

    RUN_TEST(S02_AC3_on_position_first_fix_is_never_to_live);
    RUN_TEST(S02_AC3_on_position_age_advances_with_now_ms);

    RUN_TEST(S29_on_heard_find_or_creates_slot);
    RUN_TEST(S29_on_heard_existing_slot_is_reused_not_duplicated);
    RUN_TEST(S29_on_heard_latest_sighting_wins_direct_to_relay);
    RUN_TEST(S29_on_heard_latest_sighting_wins_relay_to_direct);
    RUN_TEST(S29_on_heard_null_crew_is_safe);
    RUN_TEST(S29_on_heard_leaves_rssi_untouched);

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

    RUN_TEST(S10_select_node_jumps_directly_to_a_non_adjacent_paired_member);
    RUN_TEST(S10_select_node_unknown_id_leaves_selection_untouched);
    RUN_TEST(S10_select_node_unpaired_id_leaves_selection_untouched);
    RUN_TEST(S10_select_node_null_crew_is_safe);

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

    RUN_TEST(LONGNAME_display_name_prefers_long_when_present);
    RUN_TEST(LONGNAME_display_name_falls_back_to_short_when_long_empty);
    RUN_TEST(LONGNAME_display_name_both_empty_is_empty);
    RUN_TEST(LONGNAME_display_name_never_synthesizes_from_short);
    RUN_TEST(LONGNAME_display_name_null_member_is_safe_empty);

    RUN_TEST(HEARD_never_before_first_on_heard);
    RUN_TEST(HEARD_null_member_is_never);
    RUN_TEST(HEARD_on_heard_finds_or_creates_and_sets_fields);
    RUN_TEST(HEARD_on_heard_null_crew_is_safe);
    RUN_TEST(HEARD_freshly_heard_is_heard);
    RUN_TEST(HEARD_exactly_heard_live_ms_is_stale);
    RUN_TEST(HEARD_exactly_heard_lost_ms_is_stale);
    RUN_TEST(HEARD_just_over_heard_lost_ms_is_lost);
    RUN_TEST(HEARD_presence_is_independent_of_position);
    RUN_TEST(HEARD_stale_position_with_fresh_heard_is_not_position_lost);
    RUN_TEST(HEARD_wraparound_safe);

    return UNITY_END();
}
