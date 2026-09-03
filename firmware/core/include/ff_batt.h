/**
 * ff_batt.h — core/batt: honest mV -> percent battery gauge.
 *
 * Spec: docs/specs/S25-power-latch.md, slice (c) "Battery gauge". This is
 * the CORE half only — no ADC, no I/O, no floats in the hot path (pure
 * C11, matching every other core/ module, CLAUDE.md's "all logic goes in
 * core/"). A device-target PR (not this one; see that spec's slice (c)
 * bullet) reads the Waveshare board's ADC1 ch7 (GPIO8, 12 dB atten, 1:3
 * divider, ~0.99 correction) and calls `ff_shell_set_batt_mv`
 * (app/include/ff_shell.h) once per battery-read tick; the sim has no
 * battery hardware at all and never calls it, so `batt_pct` stays -1
 * ("unknown") there, honestly.
 *
 * Two pieces, both pure functions/state machines of their own inputs:
 *
 *  1. `ff_batt_pct_from_mv` — a single reading, in isolation: pack
 *     millivolts -> percent, or -1 ("unknown") when the reading cannot
 *     honestly be turned into a percent at all (no battery attached, or
 *     a sense line that has failed outright). This never fabricates a
 *     level it does not have evidence for (CLAUDE.md: "honest data over
 *     pretty data").
 *
 *  2. `ff_batt_filter_t` — the raw ADC droops under momentary load
 *     (radio TX, backlight PWM, LVGL redraw spikes) and would otherwise
 *     make the displayed percent flicker tick to tick even though the
 *     pack's actual charge is not changing that fast. This smooths a
 *     stream of readings into one slowly-changing DISPLAYED percent, via
 *     a small pre-filled moving AVERAGE in the mV domain plus Schmitt-
 *     style hysteresis on the resulting percent — see its own doc
 *     comment below for exactly what "displayed" means, why a plain
 *     mean replaced an earlier moving-median design (PR #180 review:
 *     a median flips its majority every sample under ALTERNATING input,
 *     which is exactly the load-sag pattern a radio TX duty cycle
 *     produces — a mean does not), and why the low-battery threshold's
 *     hysteresis exemption is asymmetric (prompt on the way down into
 *     low, deliberately NOT prompt on the way back out).
 *
 * Neither piece owns *when* to read the ADC (the target's own tick
 * cadence decides that) or *how* to convert a raw ADC count to
 * millivolts (the target's own attenuation/divider math, entirely
 * device-specific) — this header starts from "here is a pack voltage in
 * mV", which is the one honest, platform-neutral unit both the real
 * device and any future test rig can hand it.
 */
#ifndef FF_BATT_H
#define FF_BATT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * ff_batt_pct_from_mv — the single-cell LiPo open-circuit voltage curve
 * ------------------------------------------------------------------- */

/**
 * Plausibility window: a reading outside [FF_BATT_MV_PLAUSIBLE_MIN,
 * FF_BATT_MV_PLAUSIBLE_MAX] is not "a battery at an extreme charge
 * level" — a real single-cell LiPo never legitimately sits below 2.5 V
 * (protection circuitry on any sane pack cuts the cell well above that)
 * or above 4.6 V (past a hard-fault overcharge on the highest common
 * per-cell ceiling, 4.35 V packs included, with headroom) — it is a
 * broken/absent sense line, a disconnected battery, or a divider math
 * bug. `ff_batt_pct_from_mv` reports -1 (unknown) rather than clamping
 * such a reading into a plausible-looking percent: CLAUDE.md's "unknown
 * = explicitly unknown", applied to sensor plausibility the same way
 * ff_geo.h's heading-invalid sentinel is.
 */
#define FF_BATT_MV_PLAUSIBLE_MIN ((uint16_t)2500u)
#define FF_BATT_MV_PLAUSIBLE_MAX ((uint16_t)4600u)

/**
 * ff_batt_pct_from_mv — single-cell LiPo pack voltage -> charge percent,
 * or -1 ("unknown") when no honest percent can be derived.
 *
 * -1 whenever:
 *   - `pack_mv == 0` — the target's own "never read yet / sense line
 *     dead" sentinel (mirrors `ff_shell.h`'s existing -1-for-unknown
 *     convention on the display side, one layer up).
 *   - `pack_mv` falls outside the plausibility window above.
 *
 * Otherwise, a piecewise-linear interpolation over a table of open-
 * circuit-voltage (OCV) -> state-of-charge (SOC) points for a single
 * LiPo cell at rest:
 *
 *     3300 mV ->   0%      3800 mV ->  50%
 *     3500 mV ->   5%      3900 mV ->  65%
 *     3600 mV ->  10%      4000 mV ->  80%
 *     3700 mV ->  30%      4100 mV ->  92%
 *                           4200 mV -> 100%
 *
 * SOURCE: these are the same nine breakpoints published across the
 * fuel-gauge reference guides this project's own bring-up docs already
 * point to for the LC709203F/MAX1704x family of single-cell gauges
 * (Adafruit's "Custom Battery Profile" learn guides, and the near-
 * identical table SparkFun's LiPo Fuel Gauge hookup guide mirrors) — the
 * shape every hobbyist ESP32 LiPo percentage table on the market
 * ultimately derives from, not a datasheet specific to whichever cell
 * ships in a given puck (this project has not picked one yet). It is a
 * REST/open-circuit curve, not a curve measured under the puck's own
 * load — see `ff_batt_filter_t` below for why that matters less than it
 * sounds (the filter's whole job is exactly to reject the load-induced
 * sag this curve does not model).
 *
 * Below the table's lowest point (< 3300 mV, but still inside the
 * plausibility window): clamped to 0 — a real pack down there is
 * effectively empty, and the curve has no data to interpolate against.
 * Above the table's highest point (> 4200 mV): clamped to 100.
 *
 * Interpolation is integer-only (round-to-nearest at each segment,
 * `(delta_mv * delta_pct + half_denominator) / delta_mv_span`) — no
 * floats anywhere in this function, so it costs nothing to call from a
 * device tick.
 */
int8_t ff_batt_pct_from_mv(uint16_t pack_mv);


/* ---------------------------------------------------------------------
 * ff_batt_filter_t — moving-average + Schmitt-hysteresis DISPLAY
 * smoothing (PR #180 review round: replaces an earlier moving-median
 * design that a subsequent review found unstable — see below)
 * ------------------------------------------------------------------- */

/** Moving-average window, in samples, in the mV DOMAIN (not the percent
 * domain — averaging mV first and converting the result through
 * `ff_batt_pct_from_mv` ONCE gives a single, consistent rounding point,
 * versus averaging N already-rounded percents). 4 was chosen by
 * simulating this exact filter against a battery of alternating-input
 * stress pairs spanning the OCV table (the review's own 3720/3650 mV
 * and 3625/3630 mV cases among them) and picking the smallest window
 * whose worst case held to the review's <= 2 sub-hysteresis-band
 * displayed-change budget — see `core/tests/test_batt.c`'s two
 * literal simulation tests, which run the acceptance scenarios exactly
 * as specified rather than asserting a hand-derived number. Smaller
 * also means more responsive to a genuine, sustained voltage change
 * (the window fully turns over in 4 pushes, not a dozen). Pinned by
 * literal in tests — changing this constant changes both stress-test
 * outcomes above and must be re-validated against them, not assumed
 * safe.
 *
 * PRE-FILLED, not grown one sample at a time: the first real reading
 * (or the first reading after a stale-gap/dead-sensor reset — see
 * below) fills EVERY slot with that one value, rather than starting
 * from a window of 1 and growing to 4. This is what makes the filter's
 * per-sample response constant from the very first subsequent push
 * (each new sample always carries exactly 1/4 of the average's weight)
 * instead of following an early transient of ever-changing weight
 * (1/2, then 1/3, then 1/4 as a GROWING window fills) — that specific
 * transient is what made the earlier moving-median design (and an
 * early growing-mean draft of this one) bounce non-monotonically
 * through 3+ displayed values before settling on exactly this review's
 * 3720/3650 mV case, one over budget. A pre-filled window still reveals
 * the first-ever real reading IMMEDIATELY (the average of N identical
 * copies of one value is that value) — "unknown-until-read, then
 * real" is unaffected. */
#define FF_BATT_FILTER_WINDOW ((uint8_t)4u)

/** Display hysteresis, in percentage points, applied Schmitt-style: the
 * DISPLAYED value only moves to the freshly filtered percent when the
 * two differ by at least this much (ordinary case — see the asymmetric
 * low-battery exception below, which this constant also defines the
 * exit margin for). 2 is small enough that a real, sustained trend
 * still updates promptly (it only ever has to cross two points, not
 * accumulate a large silent backlog) and large enough, PAIRED WITH the
 * mV-domain averaging above, to swallow ordinary reading noise — see
 * `core/tests/test_batt.c`'s alternating-input simulation tests for the
 * measured proof, not just the reasoning. */
#define FF_BATT_HYSTERESIS_PCT ((int16_t)2)

/** A gap between two pushes at least this long (milliseconds) is
 * treated as "the device was asleep/away, not merely between two
 * ordinary ticks" — the averaging window resets (refills with the new
 * sample, per the pre-fill discipline above) rather than blending a
 * stale pre-gap reading with a fresh post-wake one. Wraparound-safe via
 * `ff_time_reached` (`ff_clock.h`), same convention as every other
 * core FSM's deadline math. 30 s is comfortably longer than any
 * expected battery-read tick period on either target and comfortably
 * shorter than the shortest plausible sleep/wake cycle. */
#define FF_BATT_FILTER_STALE_GAP_MS ((uint32_t)30000u)

/** Consecutive IMPLAUSIBLE/zero readings (see `ff_batt_pct_from_mv`)
 * after which the filter honestly reverts the display to -1 (unknown)
 * rather than continuing to show a percent frozen from before the
 * sense line went bad. PR #180 review (should-fix): a dead ADC line
 * pushing 0 or an out-of-window reading must not leave a stale-but-
 * plausible-looking number on the glass — CLAUDE.md's "honest data
 * over pretty data" applies to a sensor that STOPS reporting, not only
 * to one that never started. 3 tolerates a couple of one-off bad
 * samples (a transient ADC glitch) without flapping the display back
 * to "--%" and immediately back to a number, while still reverting
 * within a handful of ticks of a genuinely dead line. A single valid
 * reading at any point resets this counter to 0 — it counts a
 * consecutive run, not a lifetime total. */
#define FF_BATT_FILTER_DEAD_AFTER ((uint8_t)3u)

/**
 * ff_batt_filter_t — caller-owned filter state. Plain, inspectable
 * struct (no opaque handle), same convention as `ff_radar_smooth_t` /
 * `ff_button_t`: safe on the stack or in a static; zero-initialize or
 * call `ff_batt_filter_init` before first use.
 */
typedef struct {
    uint16_t history[FF_BATT_FILTER_WINDOW]; /* ring buffer of raw pack_mv, valid readings only (mV domain, per this header's doc comment) */
    bool     filled;                         /* true once the window has been pre-filled by a first-ever (or post-reset) reading */
    uint8_t  next;                           /* ring-buffer next-write index */
    bool     has_displayed;                  /* true once any real (non-unknown) reading has ever been shown */
    int8_t   displayed_pct;                  /* the value ff_batt_filter_push last returned; -1 until has_displayed */
    bool     has_last_push;                  /* true once ff_batt_filter_push has been called at least once */
    uint32_t last_push_ms;                   /* clock time of the last push, for the stale-gap reset above */
    uint8_t  consecutive_bad;                /* consecutive implausible/zero readings since the last good one, for the dead-sensor revert above */
} ff_batt_filter_t;

/** ff_batt_filter_init — zero the filter: no history, nothing
 * displayed yet (`ff_batt_filter_t{0}` is equally valid; this exists for
 * the same "readable at the call site" reason `ff_button_init` does).
 * NULL-safe (no-op). */
void ff_batt_filter_init(ff_batt_filter_t *f);

/**
 * ff_batt_filter_push — feed one raw pack-voltage reading; returns the
 * percent that should be DISPLAYED right now (which may be unchanged
 * from the previous call). `now_ms` is the CALLER's own clock reading
 * at the moment of this actual sensor read — see `ff_shell.h`'s
 * `ff_shell_set_batt_mv` doc comment for why this must be a real,
 * per-reading timestamp rather than a cached/stale one (PR #180 review,
 * should-fix).
 *
 * Behavior:
 *  - `pack_mv` is validated via `ff_batt_pct_from_mv` first (its return
 *    value is used only to decide valid/invalid here; the DISPLAYED
 *    percent always comes from the WINDOW AVERAGE below, never a raw
 *    single-sample conversion, once at least one valid sample exists).
 *  - INVALID reading (`ff_batt_pct_from_mv(pack_mv) < 0`): not folded
 *    into the window (a broken sense-line blip must not drag a good
 *    average toward garbage). Increments `consecutive_bad`; once that
 *    reaches `FF_BATT_FILTER_DEAD_AFTER`, the display honestly reverts
 *    to -1 (unknown) and the window is cleared, so the NEXT valid
 *    reading is treated as a fresh first-ever sample (immediate
 *    reveal, no residual weight from before the sense line died).
 *    Otherwise returns whatever is currently displayed unchanged.
 *  - VALID reading: `consecutive_bad` resets to 0. A gap since the
 *    previous push of at least `FF_BATT_FILTER_STALE_GAP_MS` clears the
 *    window first (see that constant's doc comment) — the new sample
 *    then PRE-FILLS a fresh window rather than blending with pre-gap
 *    readings.
 *  - The valid reading is pushed into the mV ring buffer (pre-filling
 *    every slot on the very first push, or the first after a reset —
 *    see `FF_BATT_FILTER_WINDOW`'s doc comment) and the MEAN of the
 *    whole window is converted to a percent via `ff_batt_pct_from_mv`
 *    — this is the "filtered percent" referenced below.
 *  - FIRST EVER real sample (`!has_displayed`): the filtered percent
 *    (which, with a freshly pre-filled window, equals that one sample's
 *    own percent) is shown immediately, no hysteresis, no warm-up
 *    delay — "unknown-until-read, then real".
 *  - Every subsequent push, a Schmitt-style comparison against the
 *    CURRENTLY DISPLAYED value decides whether to move:
 *      - Filtered percent CROSSES DOWN into low battery (currently-
 *        displayed value is NOT `ff_radar_batt_is_low`, filtered value
 *        IS): always promotes immediately, no hysteresis check at all
 *        — the low-battery alert (S06's amber status-bar tint, the
 *        launcher's amber status row) must never be delayed by even a
 *        1-point move.
 *      - Filtered percent would CROSS UP OUT of low battery (currently
 *        displayed value IS low, filtered value is NOT): only promotes
 *        when the filtered value has cleared the boundary by a FULL
 *        hysteresis margin (`filtered >= FF_BATT_LOW_PCT +
 *        FF_BATT_HYSTERESIS_PCT`, i.e. >= 17). This is deliberately
 *        NOT symmetric with the downward case (PR #180 review,
 *        blocking finding: a symmetric exemption strobes the amber
 *        alert on/off when the raw reading hovers right at the
 *        boundary, e.g. an mV reading alternating between 15% and
 *        16% every push — exempting only entry and requiring a real
 *        margin to exit turns that exact hovering input into "enter
 *        low promptly once, then hold" instead of a flicker).
 *      - Otherwise (both low, or both not low): the ordinary rule —
 *        promotes only when `|filtered - displayed| >=
 *        FF_BATT_HYSTERESIS_PCT`.
 *
 * NULL `f`: returns -1, no-op otherwise.
 */
int8_t ff_batt_filter_push(ff_batt_filter_t *f, uint16_t pack_mv, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* FF_BATT_H */
