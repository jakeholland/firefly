/**
 * test_demofeed.c — S23 (slice a) demo-feed generator acceptance criteria.
 *
 * Test names mirror docs/specs/S23-demo-feed.md. The spec's AC1 (seeded PRNG,
 * deterministic tick over a fixed schedule) is the load-bearing contract for
 * this slice and gets the bulk of the coverage; the brief's additional
 * determinism/bounds/distribution/cap/guard requirements each get their own
 * named, MEASURED test. AC2/AC3/AC4/AC5/AC6 are app/clock/gating/festpack
 * concerns owned by slices (b)/(c)/(d) — see the PR body.
 *
 * Proxy-check discipline (AGENTS.md standing brief, code-review.md item 6):
 *  - Determinism is asserted between TWO SEPARATELY-init'd states, never a
 *    state compared to itself (which would pass trivially).
 *  - The "touches every kind / every member" test runs a long enough horizon
 *    that stuck output would fail, and asserts on measured counts.
 *  - The cap test compares the capped run's full multiset against an
 *    uncapped reference — proving nothing was dropped or duplicated, not just
 *    that <= max came back per call.
 */
#include <string.h>

#include "unity.h"

#include "ff_demofeed.h"

void setUp(void) {}
void tearDown(void) {}

/* Scratch space big enough to drain any single tick in these tests. */
#define CAP 64u

/* Collect the ENTIRE stream from `s` up to now_ms into `buf` (draining with
 * repeated ticks so a small `max` never truncates the record). Returns the
 * count. Asserts we never overflow the caller's buffer. */
static uint32_t collect(ff_demofeed_t *s, uint32_t now_ms, uint8_t max,
                        ff_demo_event_t *buf, uint32_t buf_cap)
{
    ff_demo_event_t tmp[CAP];
    uint32_t total = 0u;
    for (;;) {
        uint8_t n = ff_demofeed_tick(s, now_ms, tmp, max);
        if (n == 0u) {
            break;
        }
        for (uint8_t i = 0u; i < n; i++) {
            TEST_ASSERT_TRUE_MESSAGE(total < buf_cap, "collect buffer overflow");
            buf[total++] = tmp[i];
        }
    }
    return total;
}

static bool events_equal(const ff_demo_event_t *a, const ff_demo_event_t *b)
{
    return a->type == b->type && a->member_idx == b->member_idx &&
           a->kind == b->kind && a->text_ref == b->text_ref &&
           a->at_ms == b->at_ms;
}

/* --- White-box seam for the gap-endpoint tests (below) ---------------
 *
 * `ff_demofeed_gap` is (correctly) a private static in ff_demofeed.c and is
 * NOT widened into the public API just for testing. Instead we reach the
 * exact endpoints deterministically: we replicate the generator's xorshift32
 * (its PRNG is not under test here — only the gap FORMULA is) purely to SEARCH
 * for a seed whose draw lands on a chosen residue, then feed that seed to the
 * real ff_demofeed_init and assert the IMPL's resulting gap. Because the seed
 * is fixed by this reference PRNG (independent of any mutation to the gap
 * formula), a ±1 off-by-one in ff_demofeed_gap moves the impl's gap off the
 * pinned endpoint and the assert fires.
 *
 * xorshift32 is a bijection over the 2^32-1 non-zero states, so every residue
 * class mod `span` is reachable; the linear search always terminates. */
static uint32_t ref_xs32(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

/* Smallest non-zero seed whose `draw_index`-th draw (1-based; init draws the
 * signal gap 1st, the poke gap 2nd) has value % span == target. Returns 0 if
 * somehow unreachable (asserted by callers, never expected). */
static uint32_t seed_for_residue(uint32_t draw_index, uint32_t span,
                                 uint32_t target)
{
    for (uint32_t s = 1u; s != 0u; s++) {
        uint32_t x = s;
        for (uint32_t d = 0u; d < draw_index; d++) {
            x = ref_xs32(x);
        }
        if ((x % span) == target) {
            return s;
        }
    }
    return 0u;
}

/* ------------------------------------------------------------------- */
/* AC1 — seeded PRNG, deterministic stream                             */
/* ------------------------------------------------------------------- */

/* Two states init'd SEPARATELY with the SAME params, run through the SAME
 * now_ms sequence, must produce a byte-identical stream — every field of
 * every event. This is the determinism contract. */
static void S23_AC1_two_independent_states_produce_identical_stream(void)
{
    ff_demofeed_t a, b;
    ff_demofeed_init(&a, 0xC0FFEEu, 100000u, 8u);
    ff_demofeed_init(&b, 0xC0FFEEu, 100000u, 8u);

    /* A fixed, irregular now_ms schedule (not uniform steps). */
    const uint32_t schedule[] = {
        100000u, 130000u, 175000u, 175000u, 260000u,
        400000u, 401000u, 900000u, 1500000u, 3600000u,
    };

    ff_demo_event_t ea[512], eb[512];
    uint32_t na = 0u, nb = 0u;
    for (size_t k = 0u; k < sizeof(schedule) / sizeof(schedule[0]); k++) {
        na += collect(&a, schedule[k], CAP, ea + na, 512u - na);
        nb += collect(&b, schedule[k], CAP, eb + nb, 512u - nb);
    }

    TEST_ASSERT_TRUE_MESSAGE(na > 20u, "expected a substantial stream");
    TEST_ASSERT_EQUAL_UINT32(na, nb);
    for (uint32_t i = 0u; i < na; i++) {
        TEST_ASSERT_TRUE_MESSAGE(events_equal(&ea[i], &eb[i]),
                                 "streams diverged");
    }
}

/* A different seed must (over a real horizon) produce a different stream —
 * otherwise the "seed" input is a no-op and determinism above is vacuous. */
static void S23_AC1_different_seed_diverges(void)
{
    ff_demofeed_t a, b;
    ff_demofeed_init(&a, 1u, 0u, 8u);
    ff_demofeed_init(&b, 2u, 0u, 8u);

    ff_demo_event_t ea[512], eb[512];
    uint32_t na = collect(&a, 3600000u, CAP, ea, 512u);
    uint32_t nb = collect(&b, 3600000u, CAP, eb, 512u);

    TEST_ASSERT_TRUE(na > 20u && nb > 20u);

    bool any_diff = (na != nb);
    uint32_t n = (na < nb) ? na : nb;
    for (uint32_t i = 0u; i < n && !any_diff; i++) {
        if (!events_equal(&ea[i], &eb[i])) {
            any_diff = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(any_diff, "distinct seeds gave identical streams");
}

/* One big tick to T must yield exactly the same events (order + fields) as
 * stepping to T in many small increments: no double-emit, no skip. Uses two
 * separately-init'd states. */
static void S23_AC1_one_big_tick_equals_many_small_steps(void)
{
    ff_demofeed_t big, small;
    ff_demofeed_init(&big, 0xABCDu, 5000u, 6u);
    ff_demofeed_init(&small, 0xABCDu, 5000u, 6u);

    const uint32_t T = 2000000u;

    ff_demo_event_t eb[1024], es[1024];
    uint32_t nb = collect(&big, T, CAP, eb, 1024u);

    uint32_t ns = 0u;
    for (uint32_t t = 5000u; t <= T; t += 250u) {
        ns += collect(&small, t, CAP, es + ns, 1024u - ns);
    }
    ns += collect(&small, T, CAP, es + ns, 1024u - ns);

    TEST_ASSERT_TRUE_MESSAGE(nb > 30u, "expected a substantial stream");
    TEST_ASSERT_EQUAL_UINT32(nb, ns);
    for (uint32_t i = 0u; i < nb; i++) {
        TEST_ASSERT_TRUE_MESSAGE(events_equal(&eb[i], &es[i]),
                                 "big-tick vs small-step streams diverged");
    }
}

/* Nothing is due at or before the epoch, and nothing before the first
 * scheduled gap. */
static void S23_AC1_no_event_before_first_due_time(void)
{
    ff_demofeed_t s;
    ff_demofeed_init(&s, 42u, 1000000u, 4u);

    ff_demo_event_t out[CAP];

    /* At the epoch itself: nothing. */
    TEST_ASSERT_EQUAL_UINT8(0u, ff_demofeed_tick(&s, 1000000u, out, CAP));

    /* Strictly before the smaller of the two first gaps (min gap is the
     * poke's 10s): just before epoch+10s, still nothing. */
    TEST_ASSERT_EQUAL_UINT8(0u, ff_demofeed_tick(&s, 1000000u + FF_DEMOFEED_POKE_MIN_MS - 1u, out, CAP));

    /* Far enough ahead, events appear, and the earliest is strictly after
     * the epoch. */
    uint8_t n = ff_demofeed_tick(&s, 1000000u + 200000u, out, CAP);
    TEST_ASSERT_TRUE(n > 0u);
    TEST_ASSERT_TRUE_MESSAGE((int32_t)(out[0].at_ms - 1000000u) > 0,
                             "first event at or before epoch");
}

/* Emitted stream is in non-decreasing at_ms order (signal/poke interleave is
 * chronological). */
static void S23_AC1_stream_is_chronological(void)
{
    ff_demofeed_t s;
    ff_demofeed_init(&s, 7u, 0u, 5u);

    ff_demo_event_t buf[2048];
    uint32_t n = collect(&s, 5000000u, CAP, buf, 2048u);
    TEST_ASSERT_TRUE(n > 50u);
    for (uint32_t i = 1u; i < n; i++) {
        TEST_ASSERT_TRUE_MESSAGE(
            (int32_t)(buf[i].at_ms - buf[i - 1u].at_ms) >= 0,
            "at_ms went backwards");
    }
}

/* ------------------------------------------------------------------- */
/* Interval bounds — every gap within the documented range             */
/* ------------------------------------------------------------------- */
static void S23_bounds_signal_and_poke_gaps_within_documented_range(void)
{
    ff_demofeed_t s;
    ff_demofeed_init(&s, 0xBEEFu, 0u, 5u);

    ff_demo_event_t buf[4096];
    uint32_t n = collect(&s, 20000000u, CAP, buf, 4096u);
    TEST_ASSERT_TRUE_MESSAGE(n > 200u, "need a long run to trust the bounds");

    uint32_t prev_sig = 0u, prev_poke = 0u;
    bool have_sig = false, have_poke = false;
    uint32_t sig_gaps = 0u, poke_gaps = 0u;

    for (uint32_t i = 0u; i < n; i++) {
        if (buf[i].type == FF_DEMO_EVENT_SIGNAL) {
            if (have_sig) {
                uint32_t g = buf[i].at_ms - prev_sig;
                TEST_ASSERT_TRUE_MESSAGE(g >= FF_DEMOFEED_SIGNAL_MIN_MS,
                                         "signal gap below min");
                TEST_ASSERT_TRUE_MESSAGE(g <= FF_DEMOFEED_SIGNAL_MAX_MS,
                                         "signal gap above max");
                sig_gaps++;
            }
            prev_sig = buf[i].at_ms;
            have_sig = true;
        } else {
            if (have_poke) {
                uint32_t g = buf[i].at_ms - prev_poke;
                TEST_ASSERT_TRUE_MESSAGE(g >= FF_DEMOFEED_POKE_MIN_MS,
                                         "poke gap below min");
                TEST_ASSERT_TRUE_MESSAGE(g <= FF_DEMOFEED_POKE_MAX_MS,
                                         "poke gap above max");
                poke_gaps++;
            }
            prev_poke = buf[i].at_ms;
            have_poke = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(sig_gaps > 50u, "too few signal gaps measured");
    TEST_ASSERT_TRUE_MESSAGE(poke_gaps > 50u, "too few poke gaps measured");
}

/* WHITE-BOX endpoint pinning (PR #115 review, Medium finding). The observed-
 * range test above only proves gaps land INSIDE [MIN,MAX]; over the seeded run
 * the exact endpoints are essentially never sampled (~26-51ms slack), so a ±1
 * off-by-one in the gap formula survives it. These two tests drive the formula
 * to residue 0 (must give MIN exactly) and residue span-1 (must give MAX
 * exactly), and confirm MIN-1 / MAX+1 are never the result — pinning both
 * inclusive endpoints. See the seed_for_residue seam comment above.
 *
 * The signal gap is init's 1st draw, so seed_for_residue(1, ...) targets it
 * directly. member_count is irrelevant to the init-time gap (it's only used
 * when emitting), so use 1. */
static void S23_bounds_signal_gap_endpoints_inclusive(void)
{
    const uint32_t span =
        (FF_DEMOFEED_SIGNAL_MAX_MS - FF_DEMOFEED_SIGNAL_MIN_MS) + 1u;
    const uint32_t epoch = 500000u;

    /* residue 0 => gap == MIN exactly. (Catches "+1" / lo-shift mutations:
     * they would yield MIN+1.) */
    uint32_t seed_lo = seed_for_residue(1u, span, 0u);
    TEST_ASSERT_TRUE_MESSAGE(seed_lo != 0u, "no seed for signal residue 0");
    ff_demofeed_t a;
    ff_demofeed_init(&a, seed_lo, epoch, 1u);
    uint32_t gap_lo = a.next_signal_ms - epoch;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(FF_DEMOFEED_SIGNAL_MIN_MS, gap_lo,
                                     "residue 0 must map to SIGNAL_MIN exactly");
    TEST_ASSERT_TRUE_MESSAGE(gap_lo >= FF_DEMOFEED_SIGNAL_MIN_MS,
                             "signal gap below documented MIN is producible");

    /* residue span-1 => gap == MAX exactly. (Catches "(hi-lo)" span-shrink:
     * it can no longer reach MAX; and "+1": it overshoots to MAX+1.) */
    uint32_t seed_hi = seed_for_residue(1u, span, span - 1u);
    TEST_ASSERT_TRUE_MESSAGE(seed_hi != 0u, "no seed for signal residue span-1");
    ff_demofeed_t b;
    ff_demofeed_init(&b, seed_hi, epoch, 1u);
    uint32_t gap_hi = b.next_signal_ms - epoch;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(FF_DEMOFEED_SIGNAL_MAX_MS, gap_hi,
                                     "residue span-1 must map to SIGNAL_MAX exactly");
    TEST_ASSERT_TRUE_MESSAGE(gap_hi <= FF_DEMOFEED_SIGNAL_MAX_MS,
                             "signal gap above documented MAX is producible");
}

/* The poke gap is init's 2nd draw, so seed_for_residue(2, ...) targets it. */
static void S23_bounds_poke_gap_endpoints_inclusive(void)
{
    const uint32_t span =
        (FF_DEMOFEED_POKE_MAX_MS - FF_DEMOFEED_POKE_MIN_MS) + 1u;
    const uint32_t epoch = 500000u;

    uint32_t seed_lo = seed_for_residue(2u, span, 0u);
    TEST_ASSERT_TRUE_MESSAGE(seed_lo != 0u, "no seed for poke residue 0");
    ff_demofeed_t a;
    ff_demofeed_init(&a, seed_lo, epoch, 1u);
    uint32_t gap_lo = a.next_poke_ms - epoch;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(FF_DEMOFEED_POKE_MIN_MS, gap_lo,
                                     "residue 0 must map to POKE_MIN exactly");
    TEST_ASSERT_TRUE_MESSAGE(gap_lo >= FF_DEMOFEED_POKE_MIN_MS,
                             "poke gap below documented MIN is producible");

    uint32_t seed_hi = seed_for_residue(2u, span, span - 1u);
    TEST_ASSERT_TRUE_MESSAGE(seed_hi != 0u, "no seed for poke residue span-1");
    ff_demofeed_t b;
    ff_demofeed_init(&b, seed_hi, epoch, 1u);
    uint32_t gap_hi = b.next_poke_ms - epoch;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(FF_DEMOFEED_POKE_MAX_MS, gap_hi,
                                     "residue span-1 must map to POKE_MAX exactly");
    TEST_ASSERT_TRUE_MESSAGE(gap_hi <= FF_DEMOFEED_POKE_MAX_MS,
                             "poke gap above documented MAX is producible");
}

/* The observed-range test compares gaps against the SAME macros the impl
 * uses, so a macro drift from the spec would go uncaught there. Pin the
 * macros to the spec's literal bounds (docs/specs/S23-demo-feed.md). */
static void S23_bounds_interval_macros_match_spec_literals(void)
{
    TEST_ASSERT_EQUAL_UINT32(20000u, FF_DEMOFEED_SIGNAL_MIN_MS);
    TEST_ASSERT_EQUAL_UINT32(90000u, FF_DEMOFEED_SIGNAL_MAX_MS);
    TEST_ASSERT_EQUAL_UINT32(10000u, FF_DEMOFEED_POKE_MIN_MS);
    TEST_ASSERT_EQUAL_UINT32(45000u, FF_DEMOFEED_POKE_MAX_MS);
}

/* member_idx never exceeds the roster. */
static void S23_bounds_member_idx_within_roster(void)
{
    const uint8_t members = 7u;
    ff_demofeed_t s;
    ff_demofeed_init(&s, 99u, 0u, members);

    ff_demo_event_t buf[2048];
    uint32_t n = collect(&s, 6000000u, CAP, buf, 2048u);
    TEST_ASSERT_TRUE(n > 50u);
    for (uint32_t i = 0u; i < n; i++) {
        TEST_ASSERT_TRUE_MESSAGE(buf[i].member_idx < members,
                                 "member_idx out of range");
    }
}

/* text_ref and kind stay within their documented domains; pokes carry no
 * content. */
static void S23_bounds_signal_fields_and_poke_content_free(void)
{
    ff_demofeed_t s;
    ff_demofeed_init(&s, 0x1234u, 0u, 5u);

    ff_demo_event_t buf[2048];
    uint32_t n = collect(&s, 6000000u, CAP, buf, 2048u);
    TEST_ASSERT_TRUE(n > 50u);
    for (uint32_t i = 0u; i < n; i++) {
        if (buf[i].type == FF_DEMO_EVENT_SIGNAL) {
            TEST_ASSERT_TRUE(buf[i].text_ref < FF_DEMOFEED_TEXT_REF_COUNT);
            TEST_ASSERT_TRUE((uint8_t)buf[i].kind < FF_DEMOFEED_KIND_COUNT);
        } else {
            TEST_ASSERT_EQUAL_UINT8(0u, buf[i].text_ref);
            TEST_ASSERT_EQUAL_UINT8(0u, (uint8_t)buf[i].kind);
        }
    }
}

/* ------------------------------------------------------------------- */
/* Distribution — output is live, not stuck                            */
/* ------------------------------------------------------------------- */

/* Over a long horizon every feed kind AND every member_idx must appear, for
 * both event types where applicable. A generator stuck on one value passes a
 * lazy determinism test but fails this. */
static void S23_dist_every_kind_and_member_eventually_appears(void)
{
    const uint8_t members = 6u;
    ff_demofeed_t s;
    ff_demofeed_init(&s, 0x5EEDu, 0u, members);

    ff_demo_event_t buf[8192];
    uint32_t n = collect(&s, 40000000u, CAP, buf, 8192u);
    TEST_ASSERT_TRUE_MESSAGE(n > 500u, "need a long run for distribution");

    bool kind_seen[FF_DEMOFEED_KIND_COUNT] = {0};
    bool sig_member_seen[16] = {0};
    bool poke_member_seen[16] = {0};

    for (uint32_t i = 0u; i < n; i++) {
        if (buf[i].type == FF_DEMO_EVENT_SIGNAL) {
            kind_seen[(uint8_t)buf[i].kind] = true;
            sig_member_seen[buf[i].member_idx] = true;
        } else {
            poke_member_seen[buf[i].member_idx] = true;
        }
    }

    for (uint8_t k = 0u; k < FF_DEMOFEED_KIND_COUNT; k++) {
        TEST_ASSERT_TRUE_MESSAGE(kind_seen[k], "a feed kind never appeared");
    }
    for (uint8_t m = 0u; m < members; m++) {
        TEST_ASSERT_TRUE_MESSAGE(sig_member_seen[m],
                                 "a member never sent a signal");
        TEST_ASSERT_TRUE_MESSAGE(poke_member_seen[m],
                                 "a member was never heard (no poke)");
    }
}

/* Both event types are actually produced (neither schedule is dead). */
static void S23_dist_both_signals_and_pokes_emitted(void)
{
    ff_demofeed_t s;
    ff_demofeed_init(&s, 3u, 0u, 4u);

    ff_demo_event_t buf[2048];
    uint32_t n = collect(&s, 6000000u, CAP, buf, 2048u);

    uint32_t sigs = 0u, pokes = 0u;
    for (uint32_t i = 0u; i < n; i++) {
        if (buf[i].type == FF_DEMO_EVENT_SIGNAL) {
            sigs++;
        } else {
            pokes++;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(sigs > 20u, "no/too few signals");
    TEST_ASSERT_TRUE_MESSAGE(pokes > 20u, "no/too few pokes");
}

/* ------------------------------------------------------------------- */
/* Cap / pending semantics                                             */
/* ------------------------------------------------------------------- */

/* A per-call cap of `max` is respected, and excess events are NOT dropped:
 * draining with max=1 must reproduce EXACTLY the same stream (order + fields)
 * as an uncapped reference from a separately-init'd state. */
static void S23_cap_excess_pending_never_dropped_or_duplicated(void)
{
    ff_demofeed_t capped, uncapped;
    ff_demofeed_init(&capped, 0xF00Du, 0u, 5u);
    ff_demofeed_init(&uncapped, 0xF00Du, 0u, 5u);

    const uint32_t T = 3000000u; /* many events due at once on first tick */

    /* Reference: one big drain. */
    ff_demo_event_t ref[2048];
    uint32_t nref = collect(&uncapped, T, CAP, ref, 2048u);
    TEST_ASSERT_TRUE_MESSAGE(nref > 50u, "expected many pending events");

    /* Capped: at most one event per tick call. Verify each call returns <= 1
     * and reconstruct the full stream. */
    ff_demo_event_t got[2048];
    uint32_t ngot = 0u;
    for (;;) {
        ff_demo_event_t one[1];
        uint8_t k = ff_demofeed_tick(&capped, T, one, 1u);
        TEST_ASSERT_TRUE_MESSAGE(k <= 1u, "max=1 returned more than 1");
        if (k == 0u) {
            break;
        }
        TEST_ASSERT_TRUE(ngot < 2048u);
        got[ngot++] = one[0];
    }

    TEST_ASSERT_EQUAL_UINT32(nref, ngot);
    for (uint32_t i = 0u; i < nref; i++) {
        TEST_ASSERT_TRUE_MESSAGE(events_equal(&ref[i], &got[i]),
                                 "capped drain diverged from reference");
    }
}

/* A partial drain then continue: the second tick picks up exactly where the
 * first left off (no re-emit of already-returned events). */
static void S23_cap_partial_then_resume_no_reemit(void)
{
    ff_demofeed_t split, whole;
    ff_demofeed_init(&split, 0x2222u, 0u, 5u);
    ff_demofeed_init(&whole, 0x2222u, 0u, 5u);

    const uint32_t T = 1500000u;

    ff_demo_event_t ref[1024];
    uint32_t nref = collect(&whole, T, CAP, ref, 1024u);
    TEST_ASSERT_TRUE(nref > 20u);

    /* Drain in chunks of 3. */
    ff_demo_event_t got[1024];
    uint32_t ngot = 0u;
    for (;;) {
        ff_demo_event_t chunk[3];
        uint8_t k = ff_demofeed_tick(&split, T, chunk, 3u);
        TEST_ASSERT_TRUE(k <= 3u);
        if (k == 0u) {
            break;
        }
        for (uint8_t i = 0u; i < k; i++) {
            got[ngot++] = chunk[i];
        }
    }
    TEST_ASSERT_EQUAL_UINT32(nref, ngot);
    for (uint32_t i = 0u; i < nref; i++) {
        TEST_ASSERT_TRUE(events_equal(&ref[i], &got[i]));
    }
}

/* ------------------------------------------------------------------- */
/* Guards / edge cases                                                 */
/* ------------------------------------------------------------------- */
static void S23_guard_member_count_zero_emits_nothing(void)
{
    ff_demofeed_t s;
    ff_demofeed_init(&s, 5u, 0u, 0u);
    ff_demo_event_t out[CAP];
    /* Even far in the future, an empty roster produces nothing (and does not
     * divide by zero). */
    TEST_ASSERT_EQUAL_UINT8(0u, ff_demofeed_tick(&s, 10000000u, out, CAP));
}

static void S23_guard_null_and_zero_max(void)
{
    ff_demofeed_t s;
    ff_demofeed_init(&s, 5u, 0u, 4u);
    ff_demo_event_t out[CAP];

    TEST_ASSERT_EQUAL_UINT8(0u, ff_demofeed_tick(NULL, 100000u, out, CAP));
    TEST_ASSERT_EQUAL_UINT8(0u, ff_demofeed_tick(&s, 100000u, NULL, CAP));
    TEST_ASSERT_EQUAL_UINT8(0u, ff_demofeed_tick(&s, 100000u, out, 0u));

    /* init(NULL, ...) must not crash. */
    ff_demofeed_init(NULL, 1u, 0u, 4u);
}

/* Seed 0 is remapped to the fixed default and is still deterministic (two
 * seed-0 states agree), and equals an explicit default-seed init. */
static void S23_guard_seed_zero_is_deterministic_default(void)
{
    ff_demofeed_t a, b;
    ff_demofeed_init(&a, 0u, 0u, 5u);
    ff_demofeed_init(&b, 0u, 0u, 5u);

    ff_demo_event_t ea[512], eb[512];
    uint32_t na = collect(&a, 2000000u, CAP, ea, 512u);
    uint32_t nb = collect(&b, 2000000u, CAP, eb, 512u);
    TEST_ASSERT_TRUE(na > 20u);
    TEST_ASSERT_EQUAL_UINT32(na, nb);
    for (uint32_t i = 0u; i < na; i++) {
        TEST_ASSERT_TRUE(events_equal(&ea[i], &eb[i]));
    }
}

/* member_count == 1 is valid (idx always 0), no modulo-by-zero, still ticks. */
static void S23_guard_single_member_all_idx_zero(void)
{
    ff_demofeed_t s;
    ff_demofeed_init(&s, 8u, 0u, 1u);
    ff_demo_event_t buf[512];
    uint32_t n = collect(&s, 2000000u, CAP, buf, 512u);
    TEST_ASSERT_TRUE(n > 20u);
    for (uint32_t i = 0u; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT8(0u, buf[i].member_idx);
    }
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S23_AC1_two_independent_states_produce_identical_stream);
    RUN_TEST(S23_AC1_different_seed_diverges);
    RUN_TEST(S23_AC1_one_big_tick_equals_many_small_steps);
    RUN_TEST(S23_AC1_no_event_before_first_due_time);
    RUN_TEST(S23_AC1_stream_is_chronological);

    RUN_TEST(S23_bounds_signal_and_poke_gaps_within_documented_range);
    RUN_TEST(S23_bounds_signal_gap_endpoints_inclusive);
    RUN_TEST(S23_bounds_poke_gap_endpoints_inclusive);
    RUN_TEST(S23_bounds_interval_macros_match_spec_literals);
    RUN_TEST(S23_bounds_member_idx_within_roster);
    RUN_TEST(S23_bounds_signal_fields_and_poke_content_free);

    RUN_TEST(S23_dist_every_kind_and_member_eventually_appears);
    RUN_TEST(S23_dist_both_signals_and_pokes_emitted);

    RUN_TEST(S23_cap_excess_pending_never_dropped_or_duplicated);
    RUN_TEST(S23_cap_partial_then_resume_no_reemit);

    RUN_TEST(S23_guard_member_count_zero_emits_nothing);
    RUN_TEST(S23_guard_null_and_zero_max);
    RUN_TEST(S23_guard_seed_zero_is_deterministic_default);
    RUN_TEST(S23_guard_single_member_all_idx_zero);

    return UNITY_END();
}
