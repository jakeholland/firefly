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
 * member names, member order). */
typedef enum {
    RADAR_LIVE,
    RADAR_STALE,
    RADAR_LOST,
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
    bool    stale;      /* true when that member's own freshness != LIVE */
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
 *     with a GPS-stale/lost/never position).
 *  4. Otherwise, `ff_crew_freshness()` of the selected member's position:
 *     FF_FRESH_LIVE -> RADAR_LIVE, FF_FRESH_STALE -> RADAR_STALE,
 *     FF_FRESH_LOST -> RADAR_LOST. FF_FRESH_NEVER (selected member is
 *     paired but has never sent a position fix at all) also maps to
 *     RADAR_LOST — the spec's mode enum has no dedicated "never" state,
 *     and NEVER is a strict subset of what LOST already means ("don't
 *     trust this position"); this is flagged as an interpretation call in
 *     the PR body per AGENTS.md.
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
 * fact doesn't depend on *my* position/heading being known).
 *
 * The crew ring (`dots[]`/`n_dots`) is independent of the current
 * selection and mode: every *paired* member with a known position fix
 * gets a dot (heading-relative bearing), skipped entirely when
 * `!my_pos_ok` or heading is invalid (no bearing is computable for
 * anyone, not just the selection) or when that particular member has
 * never had a position fix. `dot.stale` is true whenever that member's own
 * `ff_crew_freshness() != FF_FRESH_LIVE`.
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
