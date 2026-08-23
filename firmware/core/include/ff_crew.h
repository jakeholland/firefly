/**
 * ff_crew.h — core/crew: the crew model & freshness state machine.
 *
 * Spec: docs/specs/S02-core-crew.md
 *
 * The domain model the whole UI projects: who is my crew, where were they
 * last, how much do we trust it. Owns the honesty rules (CLAUDE.md: "unknown
 * = explicitly unknown... never fake freshness, positions, or times").
 *
 * Pure C11, no I/O, zero heap allocation — `ff_crew_t` is a plain struct
 * (fixed-size arrays only) safe to put on the stack or in a static; every
 * "now" the module needs is either passed explicitly by the caller or read
 * once via the injected `ff_clock_t` (see `ff_crew_init`), never cached
 * beyond that call.
 *
 * ## Deviations from the spec's interface sketch (see PR for detail)
 *  - `pos_age_ms` / `rssi_age_ms`: despite the name (kept verbatim from the
 *    spec's field list), these store the *absolute* clock timestamp of the
 *    last position/RSSI update, not a live-updating duration — core has no
 *    background ticker to keep a duration field fresh without a call every
 *    frame, and `ff_crew_on_position` already takes an explicit
 *    `rx_time_ms` from the caller (who read it off the injected clock at
 *    receive time). Actual elapsed age is `now_ms - m->pos_age_ms`
 *    (unsigned subtraction — wraparound-safe, matching ff_clock_t's own
 *    documented convention), which is exactly what `ff_crew_freshness`,
 *    `ff_crew_close_range`, and `ff_fmt_age` do. This is why those
 *    functions all take `now_ms` explicitly rather than reading it off
 *    `m` directly.
 *  - `ff_crew_select_rally()` (referenced in prose by docs/specs/S08's "S06
 *    selection can target landmarks: ff_crew_select_rally()") is **not**
 *    implemented here. A rally point is a landmark (lat/lon + name, no
 *    node_id/RSSI/battery/pairing) — it doesn't fit `ff_crew_member_t`,
 *    and `ff_crew_selected()` returning `ff_crew_member_t *` has no honest
 *    way to represent "the rally is selected". S08's own wording puts the
 *    combined crew+landmark selection cursor at S06 (radar face), which
 *    can hold a small tagged union of {crew member, rally point}; folding
 *    that here would mean core/crew reaching into S04's rally-point type.
 *    Deferred to S06/S08 — flagged in the PR per AGENTS.md's "spec gap"
 *    rule rather than guessed at.
 *  - RSSI trend history (the 5 s smoothing window) is *not* part of
 *    `ff_crew_member_t` — that struct's fields are copied verbatim from
 *    the spec's interface sketch. The history ring buffers live inside
 *    `ff_crew_t` instead, indexed in parallel with `members[]`, and are
 *    read by `ff_crew_rssi_trend()`.
 */
#ifndef FF_CREW_H
#define FF_CREW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ff_clock.h"
#include "ff_latlon.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Max crew slots (paired + merely-heard, no eviction — see AC2). */
#define FF_CREW_MAX 8

/* Freshness thresholds (docs/specs/S02-core-crew.md, "Behavior" section):
 * LIVE: pos_age < 45s. STALE: 45s - 10min (closed interval: both boundary
 * values land in STALE, since LIVE and LOST are both strict inequalities).
 * LOST: pos_age > 10min. */
#define FF_CREW_LIVE_MS ((uint32_t)45u * 1000u)
#define FF_CREW_LOST_MS ((uint32_t)600u * 1000u)

/* Close-range predicate thresholds (S02 spec, consumed by S06). */
#define FF_CREW_CLOSE_RANGE_M         30.0f
#define FF_CREW_CLOSE_RANGE_RSSI_AGE_MS ((uint32_t)10u * 1000u)
#define FF_CREW_CLOSE_RANGE_RSSI_DBM  (-60)

/* RSSI trend: smoothed delta over a 5s window, split into an "older" and
 * "newer" half so a handful of noisy single-sample dBm wobbles average
 * out. FF_CREW_RSSI_TREND_THRESHOLD_DBM is a product judgment call (not
 * spec-numeric): the spec's own fixtures (monotonic rise/fall vs. +-2dBm
 * noise) leave several dB of margin around it either way. */
#define FF_CREW_RSSI_TREND_WINDOW_MS ((uint32_t)5u * 1000u)
#define FF_CREW_RSSI_TREND_THRESHOLD_DBM 1.5
/* Ring-buffer capacity per member for the trend window. 16 comfortably
 * covers several samples/sec over a 5s window with headroom; below that
 * rate, oldest samples are simply aged out of the window at read time,
 * not evicted early. No Meshtastic RSSI report rate gets remotely close
 * to 16 samples/5s, but note the cap-vs-window interaction for the
 * record: a burst above ~3.2 samples/sec would ring-evict a sample
 * before it naturally ages out of the 5s window. */
#define FF_CREW_RSSI_HIST_CAP 16

typedef enum { FF_FRESH_LIVE, FF_FRESH_STALE, FF_FRESH_LOST, FF_FRESH_NEVER } ff_freshness_t;

typedef struct {
    uint32_t node_id;   /* Meshtastic node num */
    char     name[16];  /* short name, crew-visible; "" until known */
    char     initial;   /* display letter; '\0' until known */
    uint8_t  color_idx; /* index into theme crew palette; app-assigned */
    bool     paired;    /* in my crew (vs merely heard) */

    ff_latlon_t pos;
    uint32_t    pos_age_ms; /* absolute rx clock timestamp — see header note above */
    bool        has_pos;

    int8_t   battery_pct; /* -1 unknown */
    char     status[20];  /* free-text status ("RAGING"), "" if unset */

    int16_t  rssi_dbm;    /* last direct-packet RSSI, INT16_MIN if never direct */
    uint32_t rssi_age_ms; /* absolute rx clock timestamp — see header note above */
} ff_crew_member_t;

/**
 * ff_crew_t — the whole crew roster. Fully-defined (not opaque) so callers
 * can put it on the stack or in a static with zero heap allocation.
 * Zero-initialize or call `ff_crew_init` before use.
 */
typedef struct {
    ff_clock_t const *clock;
    ff_crew_member_t  members[FF_CREW_MAX];
    uint8_t count;
    int8_t  selected_slot; /* -1 = no current selection */

    /* RSSI history ring buffers, one per member slot (parallel-indexed
     * with `members`) — see the header-comment deviation note. */
    struct {
        uint32_t t_ms;
        int16_t  rssi_dbm;
    } rssi_hist[FF_CREW_MAX][FF_CREW_RSSI_HIST_CAP];
    uint8_t rssi_hist_count[FF_CREW_MAX];
    uint8_t rssi_hist_head[FF_CREW_MAX];
} ff_crew_t;

/* AC8 — zero heap allocation. There is no way to statically assert "never
 * calls malloc" in C, but there IS a way to catch the most likely
 * regression (someone adding a heap-shaped or unexpectedly huge field):
 * pin the struct to a sane upper bound so any such change fails the build
 * loudly instead of silently. */
_Static_assert(sizeof(ff_crew_t) < 8192,
               "ff_crew_t grew unexpectedly large - check for accidental "
               "heap-shaped fields; zero-heap-allocation is an S02 AC");

/**
 * ff_crew_init — zero a crew roster and bind the clock it will use for
 * timestamps it must generate internally (currently: `ff_crew_on_rssi`,
 * which — unlike `ff_crew_on_position` — has no explicit rx-time
 * parameter to take instead).
 */
void ff_crew_init(ff_crew_t *c, ff_clock_t const *clock);

/**
 * ff_crew_upsert — find-or-create a member slot for `node_id`.
 *
 * Existing id returns the same slot (pointer stable across calls — the
 * backing array never moves or shrinks). A brand-new id gets a freshly
 * zeroed slot (unpaired, no position, battery/RSSI sentinels set) with
 * `node_id` filled in; the caller fills in name/initial/color_idx/etc.
 * once known (e.g. from the Meshtastic nodeDB — crew doesn't know names).
 *
 * Returns NULL if the roster is full (`FF_CREW_MAX` distinct ids already
 * present) and `node_id` isn't one of them — fixed policy, no eviction in
 * v1, even if some occupied slots are unpaired (AC2).
 */
ff_crew_member_t *ff_crew_upsert(ff_crew_t *c, uint32_t node_id);

/**
 * ff_crew_find — read-only lookup: `node_id`'s existing slot, or NULL if
 * it has none. NEVER creates a slot and NEVER mutates `*c` in any way —
 * unlike `ff_crew_upsert`'s find-**or-create** contract, this is pure
 * find (S08 PR #25 code review, MEDIUM finding: `ff_wiring.c` was the
 * first live call path that let untrusted RF input consume the roster's
 * fixed `FF_CREW_MAX` slots just by asking "is this sender paired?" —
 * `ff_crew_upsert`'s create-on-miss behavior meant a flood of packets
 * from distinct never-before-heard node ids could permanently fill every
 * slot before any of them were ever paired, since v1 has no eviction).
 * Callers that only need to ask "do I already know this id, and is it
 * paired?" — without the side effect of claiming a slot for it — should
 * use this, not `ff_crew_upsert`. Pairing a genuinely new node still
 * goes through `ff_crew_upsert`/`ff_crew_set_paired` as before (an
 * explicit user pairing action, never inbound radio traffic).
 */
ff_crew_member_t const *ff_crew_find(ff_crew_t const *c, uint32_t node_id);

/**
 * ff_crew_set_paired — mark `node_id` paired/unpaired (in-crew vs.
 * merely-heard). Find-or-creates the slot (same no-eviction-when-full
 * policy as `ff_crew_upsert`); a no-op if the roster is full and
 * `node_id` isn't already present.
 */
void ff_crew_set_paired(ff_crew_t *c, uint32_t node_id, bool paired);

/**
 * ff_crew_on_position — record a position fix for `node_id`, received at
 * `rx_time_ms` (caller-supplied, e.g. read off the injected clock at the
 * moment the packet arrived). Find-or-creates the slot. Positions never
 * expire out of the model (CLAUDE.md: "honest data over pretty data") —
 * this always overwrites with the latest fix; staleness is a read-time
 * computation (`ff_crew_freshness`), never a reason to hide or drop data.
 */
void ff_crew_on_position(ff_crew_t *c, uint32_t node_id, ff_latlon_t p, uint32_t rx_time_ms);

/**
 * ff_crew_on_rssi — record a direct-packet RSSI sample for `node_id`,
 * timestamped internally via the clock injected in `ff_crew_init`.
 * Find-or-creates the slot, and feeds the sample into that member's
 * trend-window history (see `ff_crew_rssi_trend`).
 */
void ff_crew_on_rssi(ff_crew_t *c, uint32_t node_id, int16_t rssi_dbm);

/**
 * ff_crew_freshness — classify how much to trust `m`'s position, as of
 * `now_ms`.
 *
 * NEVER if no fix has ever arrived (`has_pos == false`) — the only path
 * out of NEVER is `ff_crew_on_position`, which always lands directly on
 * LIVE (age 0 at the instant of the fix). Otherwise LIVE/STALE/LOST per
 * the thresholds above, computed as `now_ms - m->pos_age_ms` (unsigned
 * subtraction, wraparound-safe).
 */
ff_freshness_t ff_crew_freshness(ff_crew_member_t const *m, uint32_t now_ms);

/**
 * ff_crew_close_range — true if `m` counts as "close range" (S06 face:
 * shows the up-close UI treatment), as of `now_ms`:
 *
 *   distance_m < 30m  OR  (rssi_age < 10s AND rssi_dbm > -60dBm)
 *
 * `distance_m` is caller-computed (crew doesn't know "my" position — that
 * lives in settings/geo); a negative value is treated as "distance
 * unknown", i.e. the distance leg of the OR is false. The RSSI leg is
 * false whenever `m->rssi_dbm == INT16_MIN` (never had a direct packet).
 */
bool ff_crew_close_range(ff_crew_member_t const *m, float distance_m, uint32_t now_ms);

/**
 * ff_crew_rssi_trend — hot/cold direction of `node_id`'s RSSI over the
 * trailing 5s window, as of `now_ms`: the sign of (average of the newer
 * 2.5s half) minus (average of the older 2.5s half), thresholded at
 * `FF_CREW_RSSI_TREND_THRESHOLD_DBM` to absorb single-sample noise.
 *
 * Returns +1 rising (getting closer), -1 falling (getting farther), 0
 * flat/noisy/unknown-node/not-enough-history-in-both-halves-of-the-window.
 */
int8_t ff_crew_rssi_trend(ff_crew_t const *c, uint32_t node_id, uint32_t now_ms);

/**
 * ff_crew_selected — the current radar-face selection, self-healing: if
 * the previously-selected member vanished from consideration (became
 * unpaired), this advances to the first paired member instead. NULL if
 * no member is paired.
 */
ff_crew_member_t *ff_crew_selected(ff_crew_t *c);

/**
 * ff_crew_select_next — advance the radar-face selection to the next
 * paired member, wrapping around; skips unpaired members entirely. No-op
 * if no member is paired.
 */
void ff_crew_select_next(ff_crew_t *c);

/**
 * ff_fmt_distance — format `meters` honoring the unit system, writing
 * e.g. "320 m" / "1.1 km" / "980 ft" / "0.6 mi" into `buf` (NUL-terminated,
 * truncated to fit `n` if needed).
 *
 * Metric: meters (rounded, no decimal) under 1km, then km (1 decimal) —
 * the 1km boundary itself shows km ("1.0 km"), not "1000 m".
 * Imperial: feet (rounded, no decimal) under 1000ft, then miles (1
 * decimal) — the 1000ft boundary itself shows mi, not "1000 ft".
 */
void ff_fmt_distance(char *buf, size_t n, float meters, bool imperial);

/**
 * ff_fmt_age — format `age_ms` as a coarse human age, writing e.g.
 * "8 SEC" / "4 MIN" / "2 HR" into `buf` (NUL-terminated, truncated to fit
 * `n` if needed).
 *
 * Seconds under 60s, then minutes under 60min, then hours — each boundary
 * itself rolls to the next unit ("60 SEC" is never printed; that instant
 * prints "1 MIN", and likewise "60 MIN" prints "1 HR"), matching the
 * freshness thresholds' inclusive-toward-the-next-state convention.
 */
void ff_fmt_age(char *buf, size_t n, uint32_t age_ms);

#ifdef __cplusplus
}
#endif

#endif /* FF_CREW_H */
