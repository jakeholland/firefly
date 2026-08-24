/**
 * ff_wall.h — wall-clock derivation: mesh-timestamp offset latch, the
 * plausibility gate, and unix -> local -> (day_doy, now_min).
 *
 * Spec: docs/specs/S16-app-shell.md, "Wall clock" section, slice b0.
 * Acceptance criteria AC12, AC12b, AC12c.
 *
 * ---------------------------------------------------------------------
 * MODULE PLACEMENT — why core/ and not app/ or festpack/
 * ---------------------------------------------------------------------
 * S16 presents wall time through `ff_shell_wall(ff_shell_t const *)`, so
 * a first reading puts the derivation in `app/`. This module is the
 * derivation only, in `core/`, and `ff_shell_wall` becomes a thin
 * accessor over it in slice b1. Rationale:
 *
 *   - It is pure math and policy: no I/O, no clock read of its own, no
 *     transport, no screens. CLAUDE.md's rule is "all logic goes in
 *     firmware/core/", and "is this timestamp a lie?" and "which day of
 *     the festival is it?" are exactly domain policy.
 *   - The spec names `fp_pack_t.utc_offset_min` as an input, which would
 *     force core -> festpack — the dependency edge ff_sched.h's own
 *     placement note rules out (docs/ARCHITECTURE.md: libs depend on
 *     core, never the reverse). So this module does NOT take a
 *     `fp_pack_t`. It takes the offset as plain values
 *     (ff_wall_offset_cfg_t), which keeps the pack-vs-settings
 *     resolution a pure function over four scalars and lets slice b1
 *     populate it from a pack in three lines. Core stays
 *     zero-dependency; festpack keeps its one-way edge.
 *   - The consequence worth stating: this module cannot itself apply the
 *     spec's *secondary* pack-event-window check, because it cannot see
 *     a pack. b1 can still layer that on for display purposes —
 *     ff_wall_unix_now() is public and returns absolute unix seconds.
 *     What b1 CANNOT do from downstream is protect the latch, because
 *     by the time it sees a value the latch has already been written
 *     (PR #37 review, D1). That is why the plausibility WINDOW below has
 *     an upper bound as well as a lower one, and why both are absolute
 *     constants needing no pack: the gate has to live at the point of
 *     observation, which is here, and it has to work during the
 *     want_config handshake before any pack exists.
 *
 * ---------------------------------------------------------------------
 * The honesty rule (the whole point of this module)
 * ---------------------------------------------------------------------
 * FF_WALL_UNKNOWN is a real, first-class state, not an error path. Until
 * an offset latches, the puck does not know what time it is and must say
 * so. There is no fallback to boot time, to 00:00, or to any plausible
 * guess anywhere in this file. In that state a caller must NOT evaluate
 * quiet hours (ff_quiet_now) and must NOT tick the water nudge
 * (ff_water_tick) — there is no honest `now_min` to hand them, and both
 * would silently accept a fabricated one. See CLAUDE.md, "honest data
 * over pretty data".
 *
 * ---------------------------------------------------------------------
 * Time bases — three of them, deliberately not interchangeable
 * ---------------------------------------------------------------------
 *   - unix seconds (int64_t here): the mesh's absolute timestamps.
 *     Sources on the wire are uint32_t (mc_nodeinfo_t.last_heard,
 *     mc_position_t.rx_time); int64_t internally so the 2038 rollover
 *     and any signed intermediate stay non-issues.
 *   - monotonic milliseconds (uint32_t): ff_clock_t.now_ms. Wraps about
 *     every 49.7 days; every delta here uses wraparound-safe unsigned
 *     subtraction (the idiom at targets/sim/live.c:245).
 *   - local minutes (int16_t now_min + uint16_t day_doy): ff_sched.h's
 *     festival-day contract.
 *
 * ---------------------------------------------------------------------
 * Composition with ff_sched / ff_settings
 * ---------------------------------------------------------------------
 * `now_min` is ff_sched.h's "Festival day / now_min contract" value, in
 * [FF_WALL_DAY_START_MIN, FF_WALL_DAY_START_MIN + 1440) = [360, 1800):
 * the festival day rolls at 06:00 local, so 01:00 local belongs to the
 * PREVIOUS calendar day's day_doy at now_min == 1500. That mapping is
 * ff_sched's contract, not this module's invention; this module only
 * implements the caller-side half ff_sched.h explicitly delegates
 * ("a caller resolving wall-clock time into (day_doy, now_min) must...").
 * A drift guard in tests/test_wall.c static-asserts FF_WALL_DAY_START_MIN
 * against FF_SCHED_FESTIVAL_DAY_START_MIN — core cannot include
 * ff_sched.h (that is the festpack edge above), so the check lives in
 * the one build unit that can see both headers.
 *
 * ff_quiet_now() and ff_water_tick() both normalize their `now_min`
 * modulo 1440 (ff_settings.c's ff_norm_min), so an ff_wall_t.now_min of
 * 1500 can be passed to them directly and reads as 01:00 — no
 * conversion, no caller-side modulo.
 */
#ifndef FF_WALL_H
#define FF_WALL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The plausibility WINDOW — [FF_WALL_EPOCH_FLOOR, FF_WALL_EPOCH_CEILING),
 * half-open, the same convention as ff_sched's "now" window.
 *
 * A unix timestamp outside it is not a time: below the floor it is an
 * uncorrected RTC, above the ceiling it is a corrupt or hostile clock.
 * Either way it is rejected outright and can never disturb an existing
 * latch; the puck stays FF_WALL_UNKNOWN rather than adopting it.
 *
 * BOTH bounds are absolute and need no pack and no festival data, which
 * is what lets the offset latch during the want_config handshake, before
 * any pack loads. The bound above matters as much as the one below:
 * `unix_s` reaches this module from `mc_nodeinfo_t.last_heard`, i.e.
 * straight off the radio from an unpaired node with no handshake. Without
 * an upper bound, one broken or hostile node could latch an arbitrary
 * future time — and, through the re-latch path, OVERWRITE an
 * already-correct latch and keep it wrong. A year-2100 timestamp renders
 * as an ordinary festival evening, because ff_wall_t carries no year;
 * that is precisely the "plausible invented clock" this slice exists to
 * prevent. (PR #37 review, D1.)
 *
 * Note this cannot be delegated to a later slice's pack-window check:
 * by the time anything downstream sees the derived value, the good latch
 * inside ff_wall_state_t has already been destroyed. The window has to
 * be enforced at the point of observation, which is here.
 *
 * FLOOR is 2026-08-01T00:00:00Z. The spec says "set to the build date";
 * a fixed constant is used rather than a __DATE__-derived one on purpose
 * — __DATE__ would make the gate move on every rebuild, break
 * reproducible builds, and make these tests' pass/fail depend on the day
 * CI ran. The property that matters is "comfortably after any plausible
 * uncorrected RTC and before any real festival timestamp", which a fixed
 * constant satisfies.
 *
 * MAINTENANCE, and it is real: an uncorrected RTC's reported time drifts
 * forward with the calendar while these constants do not, so the floor
 * decays into a weaker guard every year it is not bumped, silently — no
 * test breaks, nothing warns. Both bounds move forward together at each
 * release (the ceiling is derived from the floor, so bumping the floor
 * carries it). That promise is recorded as a release-checklist item in
 * firmware/README.md's "Release checklist" section rather than left in a
 * comment nobody greps. Deliberately NOT enforced by date-dependent
 * code: a test whose verdict depends on the day CI runs is worse than
 * the hazard it guards.
 */
#define FF_WALL_EPOCH_FLOOR ((int64_t)1785542400)

/** The window's width, floor -> ceiling: 1461 days (4 years, one leap
 *  day). Long enough to outlive the device's plausible service life
 *  against a given build; short enough that a garbage far-future
 *  timestamp has nowhere to hide. Derived rather than written out so a
 *  floor bump cannot leave the ceiling behind. */
#define FF_WALL_PLAUSIBLE_SPAN_S ((int64_t)126230400)

/** Exclusive upper bound of the plausibility window: 2030-08-01T00:00:00Z
 *  at the current floor. See FF_WALL_EPOCH_FLOOR above for the whole
 *  rationale and the bump policy. */
#define FF_WALL_EPOCH_CEILING (FF_WALL_EPOCH_FLOOR + FF_WALL_PLAUSIBLE_SPAN_S)

/**
 * FF_WALL_RELATCH_DELTA_S — a fresh reading disagreeing with the latched
 * offset by MORE than this many seconds re-latches; one disagreeing by
 * this much or less is accepted as agreement and changes nothing.
 *
 * 30 s: wide enough to swallow transport and processing jitter, far
 * narrower than any real clock step. The hazard being caught is not
 * drift (~15 s over three days, against minute-granularity consumers —
 * irrelevant) but the comms brain's clock STEPPING when GPS locks: a
 * puck that latched the pre-lock RTC offset is then confidently wrong
 * forever while asserting FF_WALL_MESH.
 */
#define FF_WALL_RELATCH_DELTA_S ((int64_t)30)

/**
 * FF_WALL_LATCH_MAX_AGE_MS — a latch older than this is expired:
 * ff_wall_unix_now() reports failure and the wall reads UNKNOWN again.
 *
 * 7 days. The monotonic clock is uint32_t milliseconds and wraps at
 * ~49.7 days, so beyond that a wrapped delta is indistinguishable from a
 * small one and the derived time would be silently wrong by ~49 days.
 *
 * The size of this window is a DIRECT TRADE against how large a
 * backwards clock step stays detectable — the two sum to the 49.7-day
 * lap, see FF_WALL_BACKWARD_DETECT_LIMIT_MS. 7 days is chosen well below
 * the available 40 because a latch that has gone a week without a single
 * plausible mesh timestamp is not something to keep trusting anyway
 * (NodeInfo traffic is continuous; a week of silence means something is
 * badly wrong), and spending the rest on backwards detection is the
 * better use of it.
 *
 * Honest limit statement: this cannot detect a wrap when NO query
 * happens for a full 49.7-day lap — with a single uint32_t monotonic
 * source and no other input, nothing can. Far outside a festival puck's
 * duty cycle, and any reconnect re-latches.
 */
#define FF_WALL_LATCH_MAX_AGE_MS ((uint32_t)604800000u) /* 7 days */

/**
 * FF_WALL_BACKWARD_DETECT_LIMIT_MS — the exact, and only, guarantee
 * about a non-monotonic `now_ms`.
 *
 * ff_clock_t documents its clock as monotonically nondecreasing, so a
 * backwards step is a platform contract violation rather than an
 * expected event. When one happens anyway, the unsigned delta becomes
 * 2^32 - step, which trips the age limit and degrades the answer to
 * UNKNOWN rather than to a wrong time — but ONLY while that stays above
 * FF_WALL_LATCH_MAX_AGE_MS. So:
 *
 *   detected     iff  step <  2^32 - FF_WALL_LATCH_MAX_AGE_MS  (~42.71 d)
 *   NOT detected      step >= that — the wrapped delta lands back inside
 *                     the accepted window and reads as a FORWARD jump.
 *
 * That residual blind spot is the same lap-length hole described above
 * and cannot be closed from a single uint32_t source. It is stated as a
 * bound rather than as a guarantee because the earlier wording claimed
 * one size larger than the mechanism has (PR #37 review, D2), and on
 * this module in particular an overstated guarantee is worse than a
 * precisely stated limit. Pinned by a test asserting BOTH sides.
 *
 * FOR SLICE b1: do not persist ff_wall_state_t across a reboot. `now_ms`
 * restarts at 0, so a restored latch reads as a ~29.7-day-old forward
 * delta and sails through the age check. Re-latch from the mesh instead
 * — it costs one NodeInfo.
 */
#define FF_WALL_BACKWARD_DETECT_LIMIT_MS ((uint32_t)(0xFFFFFFFFu - FF_WALL_LATCH_MAX_AGE_MS) + 1u)

/** Local-time offset validity, minutes east of UTC: UTC-12:00 .. UTC+14:00
 *  (the real-world range). A value outside it is corrupt — from a bad
 *  pack or a bad persisted settings blob — and is ignored as an offset
 *  source rather than used to derive a wrong local time. */
#define FF_WALL_OFFSET_MIN_LO ((int16_t)-720)
#define FF_WALL_OFFSET_MIN_HI ((int16_t)840)

/** Local minute-of-day at which the festival day rolls: 06:00.
 *  MUST equal FF_SCHED_FESTIVAL_DAY_START_MIN (festpack/include/ff_sched.h);
 *  duplicated here only because core cannot include festpack, and
 *  static-asserted equal in tests/test_wall.c. */
#define FF_WALL_DAY_START_MIN 360

/** Wall-clock source. FF_WALL_UNKNOWN is a state, not an error. */
typedef enum {
    FF_WALL_UNKNOWN = 0, /* no plausible timestamp has latched yet */
    FF_WALL_MESH,        /* derived from a mesh timestamp + monotonic elapsed */
} ff_wall_src_t;

/**
 * ff_wall_t — the resolved wall clock.
 *
 * When `src == FF_WALL_UNKNOWN`, EVERY other field is meaningless and is
 * zeroed; a consumer must render an explicit unknown-time state rather
 * than a clock, and must not feed these values to ff_quiet_now or
 * ff_water_tick.
 */
typedef struct {
    ff_wall_src_t src;
    uint16_t day_doy;     /* festival day, day-of-year 1..366 (ff_sched contract) */
    int16_t now_min;      /* [360, 1800) — minutes from midnight of day_doy's date */
    bool offset_assumed;  /* true iff the UTC offset was a defaulted guess, not stated */
} ff_wall_t;

/**
 * ff_wall_state_t — caller-owned latch state. Core holds no globals
 * (docs/ARCHITECTURE.md), so the latched reference point lives here.
 *
 * Zero-initialize, or call ff_wall_init, before the first observation.
 * Treat as opaque; the fields are exposed only so callers can embed it
 * by value.
 */
typedef struct {
    bool latched;         /* false = nothing plausible has ever been observed */
    int64_t latch_unix_s; /* the observed unix time at the latch instant */
    uint32_t latch_ms;    /* the monotonic ms at the latch instant */
} ff_wall_state_t;

/**
 * ff_wall_offset_cfg_t — the UTC-offset sources, as plain values.
 *
 * Deliberately NOT an `fp_pack_t const *` (see the placement note above)
 * and deliberately carrying `pack_loaded` explicitly: fp_parse() zeroes
 * `*out` on any failure, and a zeroed fp_pack_t has
 * `utc_offset_min == 0` with `utc_offset_assumed == false` — i.e. it
 * reads as a deliberately STATED offset of UTC. Without this flag, an
 * absent or failed pack would outrank a configured settings offset and
 * silently put the puck in London.
 */
typedef struct {
    bool pack_loaded;           /* a pack actually parsed OK; if false the two fields below are ignored */
    int16_t pack_offset_min;    /* fp_pack_t.utc_offset_min */
    bool pack_offset_assumed;   /* fp_pack_t.utc_offset_assumed */
    bool settings_offset_set;   /* ff_settings_t.utc_offset_set */
    int16_t settings_offset_min;/* ff_settings_t.utc_offset_min */
} ff_wall_offset_cfg_t;

/** What an observation did to the latch. Returned for logging/tests; a
 *  caller that does not care may ignore it. */
typedef enum {
    FF_WALL_OBS_REJECTED = 0, /* failed the plausibility gate — latch untouched */
    FF_WALL_OBS_LATCHED,      /* first plausible timestamp: the bootstrap */
    FF_WALL_OBS_RELATCHED,    /* disagreed by > FF_WALL_RELATCH_DELTA_S (or the latch had expired) */
    FF_WALL_OBS_AGREED,       /* within tolerance — latch deliberately left as-is */
} ff_wall_obs_t;

/** ff_wall_init — reset to the unlatched (UNKNOWN) state. */
void ff_wall_init(ff_wall_state_t *st);

/**
 * ff_wall_observe — offer a mesh timestamp to the latch.
 *
 * `unix_s` is unix seconds as carried on the wire, and BOTH sources the
 * shell will have are expressible here as plain values:
 *   - `mc_nodeinfo_t.last_heard` from on_node — the BOOTSTRAP source,
 *     populated unconditionally (mc_client.c:230) including during the
 *     want_config replay, so the offset latches during the handshake;
 *   - `mc_position_t.rx_time` from on_position, when has_rx_time — live
 *     over-the-air packets only, and therefore never able to bootstrap:
 *     mc_client.c:222 hardcodes has_rx_time = false on the NodeInfo path
 *     because rx_time is a MeshPacket field.
 * `rx_ms` is the monotonic clock at the moment of receipt.
 *
 * Gate: `unix_s` outside [FF_WALL_EPOCH_FLOOR, FF_WALL_EPOCH_CEILING) is
 * rejected and the latch is left exactly as it was. The lower bound
 * covers `last_heard == 0` ("unknown", per mc_client.h:107) for free and
 * is the uncorrected-pre-GPS-lock RTC case; the upper bound stops an
 * untrusted node from latching — or overwriting a good latch with — an
 * arbitrary future time. Rejection can never un-latch a good latch, in
 * either direction.
 *
 * Re-latch, don't latch once: once latched, a reading is compared
 * against what the latch predicts for `rx_ms`, and re-latches when they
 * differ by more than FF_WALL_RELATCH_DELTA_S in EITHER direction (the
 * GPS step can go either way). An agreeing reading deliberately does not
 * move the latch — chasing every agreeing sample would only add jitter,
 * and drift is immaterial at minute granularity.
 *
 * Returns what happened; a NULL `st` returns FF_WALL_OBS_REJECTED.
 */
ff_wall_obs_t ff_wall_observe(ff_wall_state_t *st, int64_t unix_s, uint32_t rx_ms);

/**
 * ff_wall_unix_now — absolute unix seconds at monotonic time `now_ms`.
 *
 * Returns false (and leaves `*out_unix_s` untouched) when nothing has
 * latched, or when the latch has aged past FF_WALL_LATCH_MAX_AGE_MS —
 * both are "we do not know", never a guess. `now_ms - latch_ms` is
 * wraparound-safe unsigned subtraction.
 */
bool ff_wall_unix_now(ff_wall_state_t const *st, uint32_t now_ms, int64_t *out_unix_s);

/**
 * ff_wall_resolve_offset — pick the UTC offset, in the spec's order.
 *
 *   pack offset when !pack_offset_assumed  (a STATED offset wins)
 *     -> settings offset when set          (a deliberately configured value)
 *       -> pack's assumed default          (a guess, flagged as one)
 *         -> false                         (FF_WALL_UNKNOWN)
 *
 * "Stated" is load-bearing: fp_parse ALWAYS populates utc_offset_min,
 * defaulting to -240 (EDT) with utc_offset_assumed = true. The naive
 * "if (pack_loaded) use pack.utc_offset_min" makes the settings field
 * dead code the moment any pack loads, and lets a parser default
 * outrank a value the user configured deliberately.
 *
 * A source whose value falls outside [FF_WALL_OFFSET_MIN_LO,
 * FF_WALL_OFFSET_MIN_HI] is corrupt and is skipped, falling through to
 * the next source in the order.
 *
 * `*out_assumed` is true only for the third branch. Returns false when
 * no usable source exists; on false, neither output is written.
 */
bool ff_wall_resolve_offset(ff_wall_offset_cfg_t const *cfg, int16_t *out_offset_min, bool *out_assumed);

/**
 * ff_wall_split_local — unix seconds + UTC offset -> (day_doy, now_min)
 * per ff_sched.h's festival-day contract (see the header note above).
 *
 * Proleptic-Gregorian civil date math, integer only, no libc time
 * functions (no localtime/gmtime: not reentrant, not available bare
 * metal, and would drag in a TZ database this project deliberately does
 * not carry — the offset is an explicit input instead).
 *
 * Returns false without writing anything when `unix_s` falls outside the
 * plausibility window [FF_WALL_EPOCH_FLOOR, FF_WALL_EPOCH_CEILING) or
 * `utc_offset_min` is out of range — the same window ff_wall_observe
 * enforces, so the two entry points cannot disagree about what counts as
 * a time.
 *
 * Note this takes a FIXED offset: there is no DST rule anywhere in this
 * project. A pack states one offset for the whole event, which is
 * correct for a festival that does not span a DST transition, and is the
 * schema's own model (fp_pack.h's utc_offset_min).
 */
bool ff_wall_split_local(int64_t unix_s, int16_t utc_offset_min, uint16_t *out_day_doy, int16_t *out_now_min);

/**
 * ff_wall_now — the composed answer: latch + offset resolution +
 * festival-day mapping.
 *
 * Returns a zeroed ff_wall_t with src == FF_WALL_UNKNOWN if ANY step
 * cannot be honestly completed (nothing latched, latch expired, no
 * offset source, corrupt inputs). Never falls back to boot time or to a
 * plausible-looking clock. `st` or `cfg` NULL reads as UNKNOWN.
 *
 * This is what slice b1's `ff_shell_wall()` returns.
 */
ff_wall_t ff_wall_now(ff_wall_state_t const *st, uint32_t now_ms, ff_wall_offset_cfg_t const *cfg);

#ifdef __cplusplus
}
#endif

#endif /* FF_WALL_H */
