/**
 * ff_demofeed.c — S23 (slice a) deterministic demo-feed generator.
 * See ff_demofeed.h for the full contract. Pure C11, no I/O, no heap.
 */
#include "ff_demofeed.h"

#include <stddef.h> /* NULL */

/* The generator's kind draw must stay in lockstep with ff_feed_kind_t: the
 * enum's 5 values (FEED_PULSE..FEED_FLARE) are exactly the range we pick from.
 * If someone adds a feed kind, this fires at compile time so FF_DEMOFEED_KIND_COUNT
 * (and any distribution test) gets revisited rather than silently under-covering. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(FF_DEMOFEED_KIND_COUNT == (uint8_t)(FEED_FLARE + 1),
               "FF_DEMOFEED_KIND_COUNT out of sync with ff_feed_kind_t");
#endif

/* Non-zero fallback seed. xorshift32's state must never be 0 (it is an
 * absorbing state that only ever produces 0), so a caller's seed of 0 is
 * remapped to this. Value is arbitrary but fixed => still deterministic. */
#define FF_DEMOFEED_DEFAULT_SEED ((uint32_t)0x1FEF1E5Fu)

/* ------------------------------------------------------------------- */
/* PRNG — xorshift32 (Marsaglia). NOT rand(); no time read.            */
/* ------------------------------------------------------------------- */
static uint32_t ff_demofeed_xs32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Uniform-ish draw in [lo, hi] inclusive. lo <= hi is a caller invariant
 * (all our bounds are compile-time constants that satisfy it). The modulo
 * bias is negligible for the demo's small spans and is documented in the
 * header. */
static uint32_t ff_demofeed_gap(uint32_t *rng, uint32_t lo, uint32_t hi)
{
    uint32_t span = (hi - lo) + 1u; /* bounds guarantee hi >= lo, span >= 1 */
    return lo + (ff_demofeed_xs32(rng) % span);
}

/* Wraparound-safe "is `a` at-or-before `b`" for demo-clock timestamps that
 * stay within 2^31 ms (~24 days) of each other — true for two live schedule
 * deadlines. */
static bool ff_demofeed_time_le(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) <= 0;
}

/* Wraparound-safe "has now reached (at or past) the deadline". */
static bool ff_demofeed_due(uint32_t deadline, uint32_t now)
{
    return (int32_t)(now - deadline) >= 0;
}

/* ------------------------------------------------------------------- */
/* lifecycle                                                            */
/* ------------------------------------------------------------------- */
void ff_demofeed_init(ff_demofeed_t *s, uint32_t seed, uint32_t epoch_ms,
                      uint8_t member_count)
{
    if (s == NULL) {
        return;
    }

    s->rng = (seed != 0u) ? seed : FF_DEMOFEED_DEFAULT_SEED;
    s->epoch_ms = epoch_ms;
    s->member_count = member_count;

    /* Seed the first gap for each schedule. Draw order is fixed (signal gap
     * first, then poke gap) so the whole stream is reproducible. Both land
     * strictly after epoch_ms, so nothing is due at or before the epoch. */
    s->next_signal_ms = epoch_ms + ff_demofeed_gap(&s->rng,
                                                   FF_DEMOFEED_SIGNAL_MIN_MS,
                                                   FF_DEMOFEED_SIGNAL_MAX_MS);
    s->next_poke_ms = epoch_ms + ff_demofeed_gap(&s->rng,
                                                 FF_DEMOFEED_POKE_MIN_MS,
                                                 FF_DEMOFEED_POKE_MAX_MS);
}

/* ------------------------------------------------------------------- */
/* per-event materialization                                            */
/* ------------------------------------------------------------------- */

/* Fire the next SIGNAL at its scheduled time, fill `ev`, and advance the
 * signal schedule by a fresh gap. RNG draw order is fixed: member, kind,
 * text_ref, then next-gap. */
static void ff_demofeed_emit_signal(ff_demofeed_t *s, ff_demo_event_t *ev)
{
    ev->type = FF_DEMO_EVENT_SIGNAL;
    ev->at_ms = s->next_signal_ms;
    ev->member_idx = (uint8_t)(ff_demofeed_xs32(&s->rng) % s->member_count);
    ev->kind = (ff_feed_kind_t)(ff_demofeed_xs32(&s->rng) % FF_DEMOFEED_KIND_COUNT);
    ev->text_ref = (uint8_t)(ff_demofeed_xs32(&s->rng) % FF_DEMOFEED_TEXT_REF_COUNT);

    s->next_signal_ms += ff_demofeed_gap(&s->rng, FF_DEMOFEED_SIGNAL_MIN_MS,
                                         FF_DEMOFEED_SIGNAL_MAX_MS);
}

/* Fire the next PRESENCE_POKE and advance the poke schedule. kind/text_ref
 * are held at 0 (a poke carries no content). */
static void ff_demofeed_emit_poke(ff_demofeed_t *s, ff_demo_event_t *ev)
{
    ev->type = FF_DEMO_EVENT_PRESENCE_POKE;
    ev->at_ms = s->next_poke_ms;
    ev->member_idx = (uint8_t)(ff_demofeed_xs32(&s->rng) % s->member_count);
    ev->kind = (ff_feed_kind_t)0; /* FEED_PULSE, unused on a poke */
    ev->text_ref = 0u;

    s->next_poke_ms += ff_demofeed_gap(&s->rng, FF_DEMOFEED_POKE_MIN_MS,
                                       FF_DEMOFEED_POKE_MAX_MS);
}

/* ------------------------------------------------------------------- */
/* tick                                                                 */
/* ------------------------------------------------------------------- */
uint8_t ff_demofeed_tick(ff_demofeed_t *s, uint32_t now_ms,
                         ff_demo_event_t *out, uint8_t max)
{
    if (s == NULL || out == NULL || max == 0u || s->member_count == 0u) {
        return 0u;
    }

    uint8_t count = 0u;
    while (count < max) {
        /* Which schedule is due next? Signal wins an exact tie. */
        bool signal_first = ff_demofeed_time_le(s->next_signal_ms, s->next_poke_ms);
        uint32_t due = signal_first ? s->next_signal_ms : s->next_poke_ms;

        if (!ff_demofeed_due(due, now_ms)) {
            break; /* nothing more is due at or before now_ms */
        }

        if (signal_first) {
            ff_demofeed_emit_signal(s, &out[count]);
        } else {
            ff_demofeed_emit_poke(s, &out[count]);
        }
        count++;
    }

    return count;
}
