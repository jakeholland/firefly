/**
 * ff_demofeed.h — S23 (slice a): the deterministic demo-feed generator.
 *
 * Spec: docs/specs/S23-demo-feed.md (this slice: "core `ff_demofeed` — the
 * seeded generator + event/presence schedule, pure C11 + Unity tests"). The
 * live demo clock, the CONFIG_FF_DEMO_LIVE switch, the app apply-loop through
 * ff_wiring, and the demo festpack string table are slices (b)/(c)/(d) — NOT
 * here. This module is ONLY the pure event source.
 *
 * ## What it is
 * A seeded pseudo-random generator that, given the current demo-clock time,
 * emits the synthetic crew events that have come DUE since the last tick. It
 * is the drop-in stand-in for the radio in demo mode: instead of decoding
 * real packets it invents a reproducible stream of "so-and-so signalled" /
 * "so-and-so was heard" events for the app to feed through the SAME core
 * structures (ff_feed_t / ff_crew_t) the real mesh wiring feeds.
 *
 * ## Pure, deterministic, zero heap, no clock of its own
 * Pure C11, no I/O, no LVGL, no allocation, and — critically — it never reads
 * a clock: every entry point that needs "now" takes an explicit `uint32_t
 * now_ms` (the demo clock, owned by the app), the same "explicit now_ms in,
 * no hidden clock" shape ff_flare/ff_radar use. `ff_demofeed_t` is a plain
 * fixed-size struct, safe on the stack or in a static; zero-initialize or
 * call `ff_demofeed_init` before use.
 *
 * The PRNG is a seeded **xorshift32** (never `rand()`, never a time/`Date`
 * read). Determinism is the contract: the same
 * `(seed, epoch_ms, member_count, now_ms call sequence)` produces a
 * BYTE-IDENTICAL event stream — so the generator is unit-testable and
 * golden-safe (S23 AC1/AC6). Because the schedule state advances only when
 * events are actually consumed, calling `ff_demofeed_tick` once at time T
 * yields exactly the same events (same order, same fields) as stepping to T
 * in many small ticks: no double-emit, no skip.
 *
 * ## Content-free by design (honest-data guardrail, S23 AC5)
 * The generator invents NO strings and knows NO real node ids. An event
 * carries only:
 *   - `member_idx` — WHICH demo crew member (0..member_count-1). The APP owns
 *     the idx -> node_id mapping (seeded from the demo festpack); the
 *     generator stays decoupled from real identities.
 *   - `kind` — for a SIGNAL, a feed kind (ff_feed_kind_t: PULSE/TEXT/RALLY/
 *     STATUS/FLARE), reusing the S08 feed enum so the app pushes it verbatim.
 *   - `text_ref` — for a SIGNAL, a small opaque index (0..FF_DEMOFEED_TEXT_REF_COUNT-1)
 *     the app resolves against ITS OWN demo string table. The generator
 *     fabricates no text.
 *   - `at_ms` — the demo-clock time the event is scheduled for.
 * A PRESENCE_POKE carries only `member_idx` + `at_ms` (a "member was heard
 * now" pulse the app applies to refresh that member's rssi_age, so presence
 * drifts LIVE->STALE->LOST and recovers). `kind`/`text_ref` are 0 on a poke.
 *
 * ## Two independent seeded schedules
 * Signals arrive on a bounded, jittered interval (each gap is a fresh seeded
 * draw in [FF_DEMOFEED_SIGNAL_MIN_MS, FF_DEMOFEED_SIGNAL_MAX_MS]); presence
 * pokes run on their own, shorter cadence ([FF_DEMOFEED_POKE_MIN_MS,
 * FF_DEMOFEED_POKE_MAX_MS]). At each step the generator fires whichever of the
 * two is due next (signal wins an exact tie), so the emitted stream is in
 * non-decreasing `at_ms` order and the RNG-consumption order depends only on
 * the schedule — never on `now_ms` or on `max`.
 */
#ifndef FF_DEMOFEED_H
#define FF_DEMOFEED_H

#include <stdbool.h>
#include <stdint.h>

#include "ff_feed.h" /* ff_feed_kind_t — reused verbatim for SIGNAL events */

#ifdef __cplusplus
extern "C" {
#endif

/* --- Interval bounds (inclusive), in ms. Documented so tests can assert
 *     every generated gap lands within them. --------------------------- */

/** Signal inter-arrival gap: 20s..90s (bounded jittered, seeded). */
#define FF_DEMOFEED_SIGNAL_MIN_MS ((uint32_t)20000u)
#define FF_DEMOFEED_SIGNAL_MAX_MS ((uint32_t)90000u)

/** Presence-poke cadence: 10s..45s (shorter than signals, so presence
 *  stays lively but still drifts between pokes). */
#define FF_DEMOFEED_POKE_MIN_MS ((uint32_t)10000u)
#define FF_DEMOFEED_POKE_MAX_MS ((uint32_t)45000u)

/** Number of distinct text-ref slots a SIGNAL can carry (0-based). The app's
 *  demo string table resolves these; the generator only picks an index. */
#define FF_DEMOFEED_TEXT_REF_COUNT ((uint8_t)16u)

/** How many feed kinds a SIGNAL draws from (mirrors ff_feed_kind_t's 5
 *  values FEED_PULSE..FEED_FLARE — static-asserted against the enum in
 *  ff_demofeed.c so the two never silently drift). */
#define FF_DEMOFEED_KIND_COUNT ((uint8_t)5u)

typedef enum {
    FF_DEMO_EVENT_SIGNAL,        /* an incoming signal: kind + text_ref meaningful */
    FF_DEMO_EVENT_PRESENCE_POKE, /* a "heard now" pulse: kind/text_ref are 0 */
} ff_demo_event_type_t;

/**
 * ff_demo_event_t — one synthetic event. Content-free (see header top): the
 * app maps `member_idx` -> node_id and `text_ref` -> string.
 */
typedef struct {
    ff_demo_event_type_t type;
    uint8_t              member_idx; /* 0..member_count-1 */
    ff_feed_kind_t       kind;       /* SIGNAL only; 0 (FEED_PULSE) on a poke */
    uint8_t              text_ref;   /* SIGNAL only; 0..FF_DEMOFEED_TEXT_REF_COUNT-1 */
    uint32_t             at_ms;      /* demo-clock time the event is scheduled for */
} ff_demo_event_t;

/**
 * ff_demofeed_t — the whole generator state. Fully-defined (not opaque),
 * same "put it on the stack / inspect in tests" convention as the other core
 * structs. Treat the fields as private: use `ff_demofeed_init` /
 * `ff_demofeed_tick`, not direct mutation.
 *
 * `rng` is the xorshift32 state (never 0). `next_signal_ms` / `next_poke_ms`
 * are the ABSOLUTE demo-clock times the next signal / next poke are due; both
 * are seeded one gap past `epoch_ms` in init, so nothing fires before then.
 */
typedef struct {
    uint32_t rng;            /* xorshift32 state, invariant: != 0 */
    uint32_t epoch_ms;       /* seeded epoch (demo clock's zero) */
    uint32_t next_signal_ms; /* absolute due time of the next signal */
    uint32_t next_poke_ms;   /* absolute due time of the next presence poke */
    uint8_t  member_count;   /* demo crew size; 0 => generator emits nothing */
} ff_demofeed_t;

/**
 * ff_demofeed_init — seed the generator.
 *
 * @param s            state to initialize (no-op if NULL).
 * @param seed         PRNG seed. 0 is remapped to a fixed non-zero default
 *                     (xorshift32 cannot leave a 0 state), so a 0 seed is
 *                     still deterministic — just not distinct from that
 *                     default.
 * @param epoch_ms     the demo clock's zero. The first signal and first poke
 *                     are each scheduled one seeded gap AFTER this, so no
 *                     event is ever due at or before `epoch_ms`.
 * @param member_count number of demo crew members events may reference. If 0,
 *                     the generator is inert: `ff_demofeed_tick` always
 *                     returns 0 (no member to index).
 */
void ff_demofeed_init(ff_demofeed_t *s, uint32_t seed, uint32_t epoch_ms,
                      uint8_t member_count);

/**
 * ff_demofeed_tick — emit the events that have come due at or before `now_ms`
 * since the last tick, up to `max`, into `out`.
 *
 * Writes the due events in non-decreasing `at_ms` order and returns how many
 * were written (0..max). If more than `max` events are due, the excess stay
 * PENDING — the schedule state is only advanced for events actually emitted —
 * and are returned by the next tick(s); nothing is dropped or duplicated. A
 * poke and a signal due at the same instant emit signal-first.
 *
 * `now_ms` is the caller's demo clock and is expected to be non-decreasing
 * across calls (comparisons are wraparound-safe within the demo run). Calling
 * once at T is equivalent to stepping to T in many small ticks.
 *
 * @return number of events written to `out`, in [0, max]. 0 if `s` or `out`
 *         is NULL, if `max` is 0, if `member_count` is 0, or if nothing is
 *         yet due.
 */
uint8_t ff_demofeed_tick(ff_demofeed_t *s, uint32_t now_ms,
                         ff_demo_event_t *out, uint8_t max);

#ifdef __cplusplus
}
#endif

#endif /* FF_DEMOFEED_H */
