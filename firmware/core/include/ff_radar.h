/**
 * ff_radar.h — core/radar: the S06 radar-face view compute.
 *
 * Spec: docs/specs/S06-radar-face.md
 *
 * Pure C11, no I/O, no allocation (stack/caller-owned only). Reduces crew +
 * geo + clock into `ff_radar_view_t`, the single struct the radar screen
 * (`app/screens/scr_radar.c`, arriving with S06 slice b/c/d) renders
 * verbatim — same "state in, pixels out" projection principle as every
 * other face (docs/ARCHITECTURE.md principle 3).
 *
 * ## DRIFT GUARD resolution (PR #12 review finding #5)
 * This header is the "once this lands in core/include/ff_radar.h" the S13
 * scaffolding (`firmware/app/include/ff_app_state.h`'s old `ff_app_radar_t`
 * mirror struct) was waiting on. This slice takes option (a) from that
 * spec comment: `ff_app_radar_t` is deleted outright and
 * `ff_app_state_t.radar` is now a real `ff_radar_view_t` (see
 * ff_app_state.h's updated header comment). There is exactly one radar
 * view-state type in the codebase from this point on.
 *
 * ## Deviations from the spec's `ff_radar_compute` sketch (flagged per
 * AGENTS.md — see the PR body for the same note)
 *  - The spec's interface sketch omits any parameter for the arrow's
 *    exponential-smoothing state, but also specs that smoothing exists
 *    ("Arrow smoothing: exponential, time-constant 250 ms, wrap-aware")
 *    and that it must live in a "caller-owned ff_radar_smooth_t" so compute
 *    stays pure (no hidden static/global state, no I/O — the smoothing
 *    time constant needs the wall-clock delta between calls, which the
 *    caller alone can honestly provide by keeping the struct around
 *    between ticks). This header therefore adds a `ff_radar_smooth_t *`
 *    parameter, positioned right after the output view, and defines the
 *    struct concretely below (same "plain, inspectable struct, no opaque
 *    handle" style as `ff_geo_cal_state_t`).
 *  - The spec sketch also has no `imperial` parameter, but `dist_str` must
 *    honor the unit system (`ff_fmt_distance`'s existing, documented
 *    metric/imperial split — S02) and `ff_radar_view_t` carries only the
 *    already-formatted string, not a raw meters field for a later stage to
 *    reformat. Added as the final parameter.
 *  - `clock_str` / `batt_pct` / `mesh_ok` are NOT written by
 *    `ff_radar_compute` — they come from the RTC/battery ADC/mesh-link
 *    subsystems respectively, none of which this function receives as
 *    inputs (crew + geo + clock only, per the spec's own "State in, pixels
 *    out" section). The caller is responsible for populating those three
 *    fields separately (e.g. once per tick from the relevant HAL/mesh
 *    state) before or after calling this function; `ff_radar_compute`
 *    only ever touches the fields it can honestly derive from its inputs
 *    and leaves those three exactly as it found them.
 */
#ifndef FF_RADAR_H
#define FF_RADAR_H

#include <stdbool.h>
#include <stdint.h>

#include "ff_crew.h"
#include "ff_latlon.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors docs/specs/S06-radar-face.md's radar_mode_t exactly (name,
 * member names, member order) — AS AMENDED 2026-08-24 for issue #33
 * (see that spec's Amendments section): RADAR_PLACE is not in the
 * original sketch, added between RADAR_LOST and RADAR_CLOSE. */
typedef enum {
    RADAR_LIVE,
    RADAR_STALE,
    RADAR_LOST,
    /* issue #33: the selected member's latest position is an ASSERTION
     * (Meshtastic LOC_MANUAL), not a measurement — neither LIVE, STALE,
     * nor LOST (ff_crew_freshness's FF_FRESH_ASSERTED). A place, not a
     * person whose whereabouts were just checked. See ff_radar_compute's
     * doc comment for exactly where this sits in mode-resolution priority
     * and what it does/doesn't claim about age. */
    RADAR_PLACE,
    RADAR_CLOSE,
    RADAR_NOFIX,
    RADAR_NOSEL,
} radar_mode_t;

/* Small-string budgets, transcribed from the spec sketch's field widths
 * (name[16], dist_str[12], age_str[12], clock_str[6]). */
#define FF_RADAR_NAME_LEN  16
#define FF_RADAR_STR_LEN   12
#define FF_RADAR_CLOCK_LEN 6

/** One crew-ring dot: a paired member's heading-relative bearing. */
typedef struct {
    float   ring_deg;  /* heading-relative bearing, [0, 360) */
    char    initial;   /* display letter, '\0' if unknown */
    uint8_t color_idx;  /* index into the theme crew palette */
    /* true when that member's own freshness is STALE/LOST/NEVER — i.e.
     * "aging", in the sense the dashed/ghost ring-dot treatment means.
     * NEVER true for an asserted member (see `place` below): an asserted
     * position isn't aging, it's a different KIND of fact, and marking it
     * "stale" would falsely imply decay it never had (issue #33). */
    bool    stale;
    /* issue #33: true when that member's latest position is asserted
     * (FF_FRESH_ASSERTED) — mutually exclusive with `stale`. The ring
     * renders this member as a place marker, not a friend dot; see
     * scr_radar.c's doc comment on the KNOWN GAP for clustered markers
     * that mix a place with live/stale friends. */
    bool    place;
} ff_radar_dot_t;

/**
 * ff_radar_view_t — the whole radar-face view-state snapshot. See
 * docs/specs/S06-radar-face.md's "State in, pixels out" section for the
 * per-field semantics; `dots[]` is sized `FF_CREW_MAX` per that spec's own
 * sketch (one possible ring dot per crew roster slot).
 */
typedef struct {
    radar_mode_t mode;
    float arrow_deg;   /* smoothed screen rotation, [0, 360) */
    bool  arrow_valid; /* false in CLOSE/NOFIX/NOSEL, and whenever the
                         * selected member has no position fix to point at
                         * (see ff_radar_compute's doc comment) */
    char  name[FF_RADAR_NAME_LEN];
    char  dist_str[FF_RADAR_STR_LEN];
    /* issue #47: true when `dist_str` is an approximate-area statement
     * (e.g. "~5.8 km area") rather than a point-to-point distance,
     * because the selected member's latest fix arrived with
     * has_precision_bits && precision_bits < FF_CREW_POS_PRECISION_MIN_BITS.
     * Always false when dist_str is "" (nothing to caveat) or when
     * precision is UNKNOWN (absent has_precision_bits renders like today
     * — see ff_radar_compute's doc comment for why that asymmetry is
     * honest, not a regression). Renderer contract: scr_radar.c must not
     * pair this distance with any UI element that implies point precision
     * (no "exact-looking" mono digits without the area framing). */
    bool  dist_imprecise;
    char  age_str[FF_RADAR_STR_LEN];
    int8_t trend; /* -1/0/+1, meaningful in CLOSE mode (hot/cold) */
    ff_radar_dot_t dots[FF_CREW_MAX];
    uint8_t n_dots;
    /* NOT written by ff_radar_compute — see this header's deviation note. */
    char  clock_str[FF_RADAR_CLOCK_LEN];
    int8_t batt_pct;
    bool  mesh_ok;
} ff_radar_view_t;

/**
 * ff_radar_smooth_t — caller-owned state for the arrow's exponential
 * smoothing (see this header's deviation note for why `ff_radar_compute`
 * needs it passed in rather than owning it internally). Zero-initialize
 * (or call `ff_radar_smooth_reset`) before the first call for a given
 * radar session; safe on the stack or in a static, no allocation.
 */
typedef struct {
    bool     has_prev;      /* false until the first smoothing update */
    float    smoothed_deg;  /* last smoothed arrow_deg, valid iff has_prev */
    uint32_t last_update_ms; /* clock time of that update */
} ff_radar_smooth_t;

/** ff_radar_smooth_reset — reset smoothing state (e.g. on selection change,
 * if the caller wants the next valid frame to snap rather than sweep — see
 * ff_radar_compute's doc comment on why that's rarely necessary in
 * practice). Equivalent to zero-initializing the struct. */
void ff_radar_smooth_reset(ff_radar_smooth_t *s);

/**
 * ff_radar_compute — derive `*v` from the crew roster's current
 * selection, `heading_deg`/`my_pos`/`my_pos_ok`, and `now_ms`.
 *
 * Mode resolution (highest to lowest priority):
 *  1. RADAR_NOSEL — no member is currently paired (`ff_crew_selected`
 *     returns NULL). Independent of `my_pos_ok`/`heading_deg`.
 *  2. RADAR_NOFIX — a member is selected, but `!my_pos_ok` or
 *     `heading_deg` is invalid (negative — the same "unreliable" sentinel
 *     `ff_geo_heading_deg` returns).
 *  3. RADAR_CLOSE — `ff_crew_close_range()` is true for the selected
 *     member (checked before freshness: a member can be RSSI-close even
 *     with a GPS-stale/lost/never/asserted position — proximity by real
 *     signal strength is a fact independent of the position's provenance,
 *     and by distance to an ASSERTED position CLOSE can still fire
 *     honestly: an asserted coordinate is a real place, just not a fresh
 *     measurement, so "you are standing next to this spot" is a true
 *     statement about geometry, not a false one about currency).
 *  4. Otherwise, `ff_crew_freshness()` of the selected member's position:
 *     FF_FRESH_LIVE -> RADAR_LIVE, FF_FRESH_STALE -> RADAR_STALE,
 *     FF_FRESH_ASSERTED -> RADAR_PLACE (issue #33 — checked as its own
 *     freshness value, so it can never also be LIVE/STALE/LOST no matter
 *     what `now_ms` is). FF_FRESH_LOST -> RADAR_LOST. FF_FRESH_NEVER
 *     (selected member is paired but has never sent a position fix at
 *     all) also maps to RADAR_LOST — the spec's mode enum has no
 *     dedicated "never" state, and NEVER is a strict subset of what LOST
 *     already means ("don't trust this position"); this is flagged as an
 *     interpretation call in the PR body per AGENTS.md.
 *
 * RADAR_PLACE's own fields, stated because they deliberately DIFFER from
 * every other freshness-derived mode (issue #33's binding ruling: LOC_MANUAL
 * means "not measured," not "placed deliberately or recently" — a
 * six-month-old asserted fix looks bit-for-bit identical to a fresh one, so
 * nothing here may claim otherwise):
 *   - `age_str` is always "" in RADAR_PLACE, even though `has_pos` is true
 *     and a real `pos_age_ms` exists. That timestamp is receive time (when
 *     THIS broadcast arrived), not placement time — for an asserted fixed
 *     position it resets every re-broadcast interval regardless of how long
 *     ago the coordinate was actually set, so displaying it would claim
 *     "placed N seconds/minutes ago", exactly the false recency the ruling
 *     forbids. This is a deliberate departure from `arrow_valid`'s pattern
 *     of surfacing every honestly-known fact — here the fact age_str would
 *     normally carry is not honestly knowable, so the field stays empty
 *     rather than reporting a number that means something else entirely.
 *   - `dist_str`/`arrow_valid`/`arrow_deg` behave exactly as they do for
 *     LIVE/STALE/LOST when a position exists: distance and bearing are
 *     geometric facts about the coordinate itself, which an assertion does
 *     not make dishonest — only the coordinate's AGE is unknowable, not the
 *     coordinate.
 *
 * RENDERER CONTRACT (PR #13 review finding #2 — read this before writing
 * scr_radar.c): because of the FF_FRESH_NEVER folding above, `mode ==
 * RADAR_LOST` alone does NOT distinguish "this member's fix is genuinely
 * old" from "this member has never sent a fix at all" — and those two
 * cases need different copy (CLAUDE.md's honesty rule: a never-fixed
 * member must not be told apart from a real "LAST SEEN 47 MIN" by name
 * alone, or the UI ends up implying a real-but-stale position that never
 * existed). Disambiguate on `age_str`, not `mode`:
 *   - `mode == RADAR_LOST && age_str[0] != '\0'` — a real past fix exists;
 *     show it ("LAST SEEN <age_str>").
 *   - `mode == RADAR_LOST && age_str[0] == '\0'` — never fixed; show a
 *     distinct "NO FIX YET" (or equivalent), never a "LAST SEEN" label.
 * This is exactly the invariant `S06_AC1_mode_lost_via_never_had_a_fix`
 * (core/tests/test_radar.c) pins: `age_str == ""` whenever there is no
 * real fix to report, regardless of `mode`.
 *
 * `arrow_valid` is true only when a bearing genuinely exists to smooth
 * toward: `my_pos_ok && heading_deg` valid `&&` the selected member has a
 * position fix (`has_pos`). This is `false` for CLOSE/NOFIX/NOSEL per the
 * spec's explicit list, and *also* false for the FF_FRESH_NEVER-as-LOST
 * edge case above (a LOST reading with no bearing data at all would
 * otherwise fabricate a direction — CLAUDE.md: "never fake... positions").
 * Every other LIVE/STALE/LOST case has `arrow_valid == true`, matching the
 * spec exactly.
 *
 * `dist_str`/`age_str` are each independently left as `""` ("") when their
 * underlying fact is unknown (no `my_pos` for distance, no fix ever for
 * age) rather than reusing/fabricating a stale value — this can differ
 * per-field from what `mode` alone would suggest (e.g. RADAR_NOFIX still
 * reports a true `age_str` for the selected member's last fix, since that
 * fact doesn't depend on *my* position/heading being known). RADAR_PLACE
 * is the one further exception: `age_str` stays "" even though a fix
 * exists — see RADAR_PLACE's own paragraph above for why.
 *
 * issue #47 — `dist_str`/`dist_imprecise`: when the selected member's
 * latest fix has `has_precision_bits && precision_bits <
 * FF_CREW_POS_PRECISION_MIN_BITS` (ff_crew.h), the position is
 * known-degraded — the received coordinate can be off by kilometers (a
 * channel setting, not a bug; see issue #47's hardware measurement).
 * `dist_str` is then an approximate-AREA statement built from
 * `ff_crew_pos_precision_grid_m()` (e.g. "~5.8 km area"), never the raw
 * point-to-point distance, and `dist_imprecise` is set true so the
 * renderer can mark it distinctly. The same gate also excludes this
 * member from `ff_crew_close_range()`'s DISTANCE leg (a degraded
 * coordinate cannot honestly support "you are standing next to them" —
 * the RSSI leg is untouched, since it is measured by our own radio and
 * carries no coordinate-precision dependency at all). Absent
 * `has_precision_bits` renders exactly as before this issue's fix
 * (`dist_imprecise` stays false, normal point distance): "the sender
 * didn't say" is not evidence the fix is degraded, and this asymmetry vs.
 * a *known*-degraded fix is deliberate, not a gap — see mc_client.h's
 * `precision_bits` doc comment for the hardware-verified reason (live
 * packets stamp it affirmatively on current firmware; the want_config
 * NodeInfo replay never does, replay-absent and live-absent are the same
 * bytes on the wire and cannot be told apart, and treating "didn't say"
 * as "must be degraded" would regress every ordinary live position on
 * every replay-derived reading to a blanket "imprecise" label that is
 * itself dishonest).
 *
 * The crew ring (`dots[]`/`n_dots`) is independent of the current
 * selection and mode: every *paired* member with a known position fix
 * gets a dot (heading-relative bearing), skipped entirely when
 * `!my_pos_ok` or heading is invalid (no bearing is computable for
 * anyone, not just the selection) or when that particular member has
 * never had a position fix. `dot.place` is true whenever that member's
 * own `ff_crew_freshness() == FF_FRESH_ASSERTED`; `dot.stale` is true
 * whenever that member's own freshness is STALE/LOST/NEVER — i.e. never
 * simultaneously with `place` (see `ff_radar_dot_t`'s doc comment).
 *

 * Smoothing: whenever a bearing is computable for the selection (see
 * `arrow_valid`'s condition, minus the mode-gating), the target angle
 * (`ff_geo_arrow_deg(bearing, heading_deg)`) is folded into `*smooth` via
 * wrap-aware (`ff_geo_angdiff_deg`) exponential smoothing with a 250 ms
 * time constant, and `v->arrow_deg` is the result. When no bearing is
 * computable this call, `*smooth` is left untouched (frozen, not reset) —
 * `v->arrow_deg` reports the last smoothed value. This needs no special
 * "just became valid again after N seconds" handling: the exponential
 * step's `alpha = 1 - exp(-dt/tau)` already approaches 1 for any large
 * gap, so the very next valid frame snaps close to the new target on its
 * own.
 *
 * Allocation-free, <1 ms on any host CPU (S06 AC6) — bounded work over
 * `FF_CREW_MAX` (8) members, no I/O, no libc calls beyond `<string.h>`
 * memcpy-shaped helpers and `<math.h>` trig/`expf`.
 */
void ff_radar_compute(ff_radar_view_t *v, ff_radar_smooth_t *smooth, ff_crew_t *crew, float heading_deg,
                       ff_latlon_t my_pos, bool my_pos_ok, bool imperial, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* FF_RADAR_H */
