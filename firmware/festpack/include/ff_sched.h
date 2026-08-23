/**
 * ff_sched.h — festival schedule engine: now-playing, next-starred, the
 * per-day lineup/TBD flag, and the T-15 pre-set alarm.
 *
 * Spec: docs/specs/S07-now-face.md, slices (a) engine + (c) alarm +
 * star persistence. Slice (b) — the Now face render — is UI (LVGL
 * screen + goldens) and is NOT implemented here.
 *
 * ---------------------------------------------------------------------
 * MODULE PLACEMENT — deviation from the spec's stated header path.
 * ---------------------------------------------------------------------
 * docs/specs/S07-now-face.md's "Interface" section names this
 * `core/include/ff_sched.h`. It lives in `firmware/festpack/` instead
 * (this file is festpack/include/ff_sched.h; the implementation is
 * festpack/src/ff_sched.c). Rationale:
 *
 *   - ff_sched's entire public surface is expressed in festpack types
 *     (fp_pack_t, fp_set_t — see fp_pack.h). It has no domain logic that
 *     exists independent of the festpack schema; every function here
 *     takes a `fp_pack_t const *` as its first real argument.
 *   - docs/ARCHITECTURE.md's layering rule is "core has no I/O... libs
 *     may depend on core" and the repo layout lists festpack as a lib
 *     that depends on core (fp_pack.c already links ff-core for
 *     ff_geo_project). It does NOT say core may depend on festpack —
 *     doing that (putting ff_sched.h in core/ but #include-ing fp_pack.h)
 *     would invert that dependency edge and force every ff-core consumer
 *     (including targets that never touch festival data) to also pull in
 *     ff-festpack + ff-jsmn to link.
 *   - Living beside fp_pack.h keeps ff-festpack's public surface
 *     self-consistent (one input type, one lib for "things that operate
 *     on a loaded pack") and keeps ff-core dependency-free of the pack
 *     schema. The cost is that the app links two libs (ff-core,
 *     ff-festpack) for schedule features instead of one — but anything
 *     touching fp_pack_t already needs ff-festpack regardless, so this
 *     changes nothing for real callers.
 *
 * Flagged per AGENTS.md ("if a spec is ambiguous, note the
 * interpretation in the PR body") — this is a placement call, not a
 * behavior ambiguity; the PR body repeats this rationale.
 *
 * ---------------------------------------------------------------------
 * Purity contract
 * ---------------------------------------------------------------------
 * Pure C11: no I/O, no dynamic allocation, no clock reads, no notion of
 * user settings. Callers own all storage and are responsible for:
 *
 *   - Resolving wall-clock time into a (day_doy, now_min) pair (see
 *     "Festival day" below) — this module never reads a clock.
 *   - Quiet-hours suppression of the alarm (S11's ff_quiet_now). This
 *     module is deliberately settings-free: no ff_settings_t dependency
 *     and no notion of quiet hours, so it stays unit-testable and usable
 *     without dragging in S11. ff_sched_alarm_tick() ALWAYS reports a
 *     T-15 crossing when one occurs; the CALLER decides whether to
 *     actually alert (haptic pattern + Now face banner) based on
 *     ff_quiet_now(), and should call ff_sched_alarm_tick() every tick
 *     regardless of quiet hours so the per-set "already fired" bit gets
 *     set — a crossing that happened to land in quiet hours must not
 *     re-fire once quiet hours end.
 *   - Persisting a toggled star (ff_sched_toggle_star only mutates the
 *     in-memory fp_pack_t; S11's ff_store_t is the caller's problem).
 *
 * ---------------------------------------------------------------------
 * Festival day / "now_min" contract
 * ---------------------------------------------------------------------
 * `now_min` is minutes since local midnight of the calendar day that
 * `day_doy` names — EXCEPT the festival day rolls at 06:00 local, not
 * at midnight (festival days run past midnight into the small hours).
 * So a festival day spans now_min in [360, 1800): 06:00 on day_doy's
 * calendar date through 05:59 the *next* calendar date. Concretely, a
 * caller resolving wall-clock time into (day_doy, now_min) must map
 * clock minutes [0, 360) on a calendar date to the PREVIOUS festival
 * day (day_doy - 1) with now_min in [1440, 1800), not to that date's
 * day_doy with now_min in [0, 360).
 *
 * This module trusts the caller's (day_doy, now_min) pair and does no
 * clock/date math of its own — it only needs this invariant to make
 * sense of a set's stored start_min/end_min (each always 0..1439 or -1,
 * per fp_pack.h) against a now_min that may legitimately exceed 1440.
 *
 * FF_SCHED_FESTIVAL_DAY_START_MIN / FF_SCHED_DAY_SPAN_MIN below name
 * that window; nothing in this module's logic special-cases the 06:00
 * boundary itself (a set starting exactly at 06:00 is handled by the
 * same start_min <= now_min comparison as any other minute) — the
 * boundary only matters to whoever computes (day_doy, now_min).
 *
 * ---------------------------------------------------------------------
 * Midnight-crossing sets
 * ---------------------------------------------------------------------
 * A set with end_min < start_min (e.g. 23:30 -> 01:00, stored as
 * start_min=1410, end_min=60) runs past midnight. Per spec it is
 * attributed to its start day (fp_set_t.day_doy, set at parse time from
 * the pack's "day" field — see fp_pack.c) and, for all now/pct/alarm
 * math in this module, its EFFECTIVE end is end_min + 1440 (here, 1500)
 * rather than the raw end_min.
 *
 * ---------------------------------------------------------------------
 * "Now" window is half-open: [start_min, effective_end)
 * ---------------------------------------------------------------------
 * A set counts as playing for start_min <= now_min < effective_end —
 * inclusive at the start, EXCLUSIVE at the end. pct_done starts at 0%
 * at start_min and increases monotonically but never reaches a literal
 * 100% while the set is still "now" (it caps at whatever the last live
 * minute computes to); the set leaves "now" exactly at effective_end,
 * the same instant a back-to-back set on the same stage starting then
 * becomes "now". This is a deliberate spec ruling (see
 * docs/specs/S07-now-face.md's ## Amendments, PR #9 review) that
 * supersedes an earlier inclusive-both-ends design: at a zero-gap
 * festival changeover (stage 0's set A ends at the same minute stage
 * 0's set B starts — completely normal scheduling), an inclusive end
 * made both A and B "now" simultaneously on the same stage, violating
 * the "one row per stage" contract ff_sched_now_playing() and the Now
 * face layout both depend on. Half-open fixes that for free: at the
 * changeover minute, A's `now_min >= effective_end` excludes it the
 * same instant B's `now_min >= start_min` includes it — the starting
 * set always wins, with no explicit per-stage dedup needed.
 */
#ifndef FF_SCHED_H
#define FF_SCHED_H

#include <stdbool.h>
#include <stdint.h>

#include "fp_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 06:00 local — the festival day rolls here, not at midnight. See the
 * "Festival day / now_min contract" note above; this constant documents
 * the boundary, it is not consulted anywhere in ff_sched.c. */
#define FF_SCHED_FESTIVAL_DAY_START_MIN 360
/* One festival day = 1440 minutes, offset by the above: valid now_min
 * for a given day_doy runs [FF_SCHED_FESTIVAL_DAY_START_MIN,
 * FF_SCHED_FESTIVAL_DAY_START_MIN + FF_SCHED_DAY_SPAN_MIN). */
#define FF_SCHED_DAY_SPAN_MIN 1440

/** One row of ff_sched_now_playing() output: a live set on some stage. */
typedef struct {
    fp_set_t const *set; /* points into the caller's fp_pack_t; valid as long as it is */
    int16_t mins_left;   /* effective_end - now_min; > 0 while "now" (half-open end — see above) */
    uint8_t pct_done;    /* 0 at start_min, monotonically increasing, never reaches 100 while "now" */
} ff_now_row_t;

/** Output of ff_sched_next_starred(): the next upcoming starred set. */
typedef struct {
    fp_set_t const *set; /* points into the caller's fp_pack_t; NULL iff the call returned false */
    int16_t mins_until;  /* start_min - now_min; > 0 (strictly future). 0 if the call returned false */
} ff_next_t;

/**
 * ff_sched_now_playing — every set on `day_doy` currently live at
 * `now_min` (start_min <= now_min < effective_end, half-open, both
 * times known — see the "Now window" note above), one row per matching
 * set, written in pack order (firmware/festpack sets[] order, which is
 * schedule order within a day). At a zero-gap same-stage changeover the
 * starting set alone is "now" — see the half-open note.
 *
 * Writes up to `max` rows to `out` and returns the number written.
 * Rows beyond `max` are silently dropped in pack order — size `max` to
 * the venue's real concurrent-stage count (FP_MAX_STAGES=12 comfortably
 * covers any real festival) rather than treating it as a hard limit on
 * how many sets a day can have.
 *
 * Sets with a null start_min or end_min are never returned here — they
 * only ever appear in ff_sched_day_sets()'s lineup.
 */
uint8_t ff_sched_now_playing(fp_pack_t const *p, uint16_t day_doy, int16_t now_min,
                              ff_now_row_t out[], uint8_t max);

/**
 * ff_sched_next_starred — the earliest-starting starred set on
 * `day_doy` that has not started yet (start_min > now_min, both times
 * known). Ties (identical start_min) resolve to the lower set index
 * (pack order). A set that is already playing or already over is never
 * "next" here even if starred (see ff_sched_now_playing for "currently
 * live").
 *
 * Returns true and fills `*out` if such a set exists. Returns false and
 * zeroes `*out` (set=NULL, mins_until=0) if nothing is starred, nothing
 * starred falls on this day, or every starred set on this day has
 * already started, already finished, or has a null start/end time.
 */
bool ff_sched_next_starred(fp_pack_t const *p, uint16_t day_doy, int16_t now_min, ff_next_t *out);

/**
 * ff_sched_alarm_t — caller-owned tick state for ff_sched_alarm_tick().
 * This module holds no globals (per docs/ARCHITECTURE.md), so the
 * per-set "already fired" bookkeeping lives here. Declared here (ahead
 * of ff_sched_toggle_star below, which optionally takes one) rather
 * than just above ff_sched_alarm_tick.
 *
 * Zero-initialize (or call ff_sched_alarm_init) before the first tick,
 * and again whenever the underlying fp_pack_t's set list changes
 * (a fresh pack load, a schema update) — this struct only tracks fired
 * state by array index and has no way to detect on its own that the set
 * at a given index is no longer the set it fired for.
 */
typedef struct {
    /* One bit per fp_pack_t.sets[] index: bit i set means sets[i] has
     * already fired its T-15 alarm under this state. Sized to
     * FP_MAX_SETS so any loaded pack's indices fit. */
    uint8_t fired[(FP_MAX_SETS + 7) / 8];
} ff_sched_alarm_t;

/** ff_sched_alarm_init — clear all fired-bits (nothing has alarmed yet). */
void ff_sched_alarm_init(ff_sched_alarm_t *st);

/**
 * ff_sched_toggle_star — flip fp_set_t.starred for p->sets[set_idx].
 *
 * Bounds-checked: a no-op if set_idx >= p->n_sets (defensive; embedded
 * callers should not need to pre-validate an index sourced from a UI
 * list). Pure in-memory mutation — persisting the change (S11's
 * ff_store_t) is the caller's job, same as quiet hours: this module has
 * no I/O.
 *
 * `alarm` is optional (pass NULL if the caller doesn't have live alarm
 * state handy, e.g. UI code that only edits the pack). When non-NULL
 * AND this call transitions the set from starred to unstarred, its
 * fired-bit in `alarm` is cleared immediately, so a later re-star
 * re-arms the T-15 alert right away — deliberate "fat-finger recovery"
 * per docs/specs/S07-now-face.md's ## Amendments: un-star by accident,
 * re-star, still get alerted. (ff_sched_alarm_tick() also self-clears
 * any unstarred set's fired-bit as it scans, so re-arming still
 * eventually happens on the next tick even if `alarm` is NULL here —
 * passing it through just makes the re-arm immediate rather than
 * tick-delayed.) Starring a previously-unstarred set never touches
 * `alarm` — there is nothing to clear on that transition.
 */
void ff_sched_toggle_star(fp_pack_t *p, uint16_t set_idx, ff_sched_alarm_t *alarm);

/**
 * ff_sched_day_sets — every set attributed to `day_doy` (by
 * fp_set_t.day_doy — i.e. by start day, per the midnight-crossing rule
 * above), in pack order, REGARDLESS of whether its times are known.
 * This is the per-day lineup scroll list: spec says sets with null
 * times are listed here even though they are excluded from
 * now/next/alarms.
 *
 * Writes up to `max` pointers to `out` and returns the number written
 * (same "silently drop past max, in pack order" contract as
 * ff_sched_now_playing — pass max=FP_MAX_SETS if the caller needs every
 * set on the day guaranteed to fit).
 */
uint16_t ff_sched_day_sets(fp_pack_t const *p, uint16_t day_doy,
                            fp_set_t const *out[], uint16_t max);

/**
 * ff_sched_day_tbd — true iff `day_doy` has at least one set AND none
 * of them have both start_min and end_min known. This is the "SET TIMES
 * TBD" banner condition (spec: "When all times are null (current Lost
 * Lands state) the face shows the day lineup + 'SET TIMES TBD'
 * banner").
 *
 * False if `day_doy` has zero sets (nothing to flag as TBD) or if at
 * least one set on the day has known times.
 */
bool ff_sched_day_tbd(fp_pack_t const *p, uint16_t day_doy);

/**
 * ff_sched_alarm_tick — advance the alarm state and report at most one
 * newly-due starred-set alert.
 *
 * A starred set on `day_doy` (both times known) is "due" once
 * start_min - now_min <= 15 (the T-15 crossing) AND the set has not
 * already ended (now_min <= effective_end, so a long gap between ticks
 * can still "catch up" a missed crossing, but never fires a stale alert
 * for a set that's fully over) AND it has not already fired under `st`.
 *
 * Among every due-and-unfired set this call finds, only the one with
 * the smallest start_min fires (ties -> lower set index); at most one
 * set fires per call. So two starred sets 5 minutes apart cross their
 * own T-15 threshold on separate ticks (or, if a tick spans both
 * crossings at once, on separate *calls* to this function) and fire in
 * start order.
 *
 * Every call also clears the fired-bit of any set that is currently
 * NOT starred (across the whole pack, not just `day_doy`) — an
 * unstarred set carries no fired memory. Combined with
 * ff_sched_toggle_star's immediate clear-on-unstar, this guarantees a
 * re-starred set re-arms: either instantly (if the caller threaded
 * `alarm` into ff_sched_toggle_star) or on the very next tick after
 * re-starring (if it didn't). See docs/specs/S07-now-face.md's
 * ## Amendments.
 *
 * Returns a pointer into `p->sets[]` for the fired set (and marks its
 * fired-bit in `st`, so it never re-fires from this `st` again unless
 * un-starred and re-starred in between), or NULL if nothing is newly
 * due. Idempotent for an unchanged star/time state: re-ticking with the
 * same or a later `now_min` and no newly-due set returns NULL every
 * time.
 *
 * This function has no notion of quiet hours or haptics — see the
 * header-level purity note; the caller must still call this every tick
 * regardless of whether it intends to actually alert. It does not
 * require `now_min` to be strictly monotonic across calls, but the
 * "fires exactly once" guarantee assumes the caller only moves
 * `now_min` forward — rewinding the clock across a fire boundary does
 * not un-fire an already-fired set.
 */
fp_set_t const *ff_sched_alarm_tick(ff_sched_alarm_t *st, fp_pack_t const *p,
                                     uint16_t day_doy, int16_t now_min);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCHED_H */
