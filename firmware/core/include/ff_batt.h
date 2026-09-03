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
 *     a small moving-median window plus 2% display hysteresis — see its
 *     own doc comment below for exactly what "displayed" means and why
 *     the low-battery threshold is deliberately exempt from the
 *     hysteresis.
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
 * ff_batt_filter_t — moving-median + hysteresis DISPLAY smoothing
 * ------------------------------------------------------------------- */

/** Moving-median window, in samples. 5 is the smallest odd window that
 * meaningfully rejects a single load-sag outlier (the median of 5 is
 * unmoved by up to 2 outliers on either side) while still converging
 * fast — the window fills in 5 pushes, not dozens. Odd on purpose: an
 * odd count has a single, unambiguous middle sample, no
 * average-of-two-middles tie-break to get subtly wrong. Pinned by
 * literal in tests (`core/tests/test_batt.c`) — changing this constant
 * is a behavior change, not a tuning knob to touch casually. */
#define FF_BATT_FILTER_WINDOW ((uint8_t)5u)

/** Display hysteresis, in percentage points. The filtered (median)
 * value must move at least this far from the currently DISPLAYED value
 * before the display is allowed to change — this is what stops a
 * genuine 1% wobble in the underlying reading (which the median alone
 * does not fully suppress, since the true SOC really is drifting by
 * small amounts) from visibly ticking the percent on screen every few
 * seconds. 2 is small enough that a real, sustained charge/discharge
 * trend still updates the display promptly (it only ever has to cross
 * two points, not accumulate a large silent backlog) and large enough
 * to swallow ordinary reading noise. See `FF_BATT_LOW_PCT`
 * (`ff_radar.h`) below for the one case this hysteresis is NOT allowed
 * to delay. */
#define FF_BATT_HYSTERESIS_PCT ((int16_t)2)

/** A gap between two pushes at least this long (milliseconds) is
 * treated as "the device was asleep/away, not merely between two
 * ordinary ticks" — the median window is reset (not blended) before
 * folding in the new sample, so a stale pre-sleep reading never drags
 * out convergence to the real post-wake voltage. Wraparound-safe via
 * `ff_time_reached` (`ff_clock.h`), same convention as every other
 * core FSM's deadline math. 30 s is comfortably longer than any
 * expected battery-read tick period on either target and comfortably
 * shorter than the shortest plausible sleep/wake cycle. */
#define FF_BATT_FILTER_STALE_GAP_MS ((uint32_t)30000u)

/**
 * ff_batt_filter_t — caller-owned filter state. Plain, inspectable
 * struct (no opaque handle), same convention as `ff_radar_smooth_t` /
 * `ff_button_t`: safe on the stack or in a static; zero-initialize or
 * call `ff_batt_filter_init` before first use.
 */
typedef struct {
    int8_t   history[FF_BATT_FILTER_WINDOW]; /* ring buffer of the last N ff_batt_pct_from_mv results (valid readings only) */
    uint8_t  count;                          /* samples currently held, saturates at FF_BATT_FILTER_WINDOW */
    uint8_t  next;                           /* ring-buffer next-write index */
    bool     has_displayed;                  /* true once any real (non-unknown) reading has ever been shown */
    int8_t   displayed_pct;                  /* the value ff_batt_filter_push last returned; -1 until has_displayed */
    bool     has_last_push;                  /* true once ff_batt_filter_push has been called at least once */
    uint32_t last_push_ms;                   /* clock time of the last push, for the stale-gap reset above */
} ff_batt_filter_t;

/** ff_batt_filter_init — zero the filter: no history, nothing
 * displayed yet (`ff_batt_filter_t{0}` is equally valid; this exists for
 * the same "readable at the call site" reason `ff_button_init` does).
 * NULL-safe (no-op). */
void ff_batt_filter_init(ff_batt_filter_t *f);

/**
 * ff_batt_filter_push — feed one raw pack-voltage reading; returns the
 * percent that should be DISPLAYED right now (which may be unchanged
 * from the previous call).
 *
 * Behavior:
 *  - `pack_mv` is converted via `ff_batt_pct_from_mv` first. An unknown
 *    reading (-1) is NOT folded into the history (a broken sense-line
 *    blip must not drag a good median toward garbage) and this call
 *    simply returns whatever is currently displayed (-1 if nothing has
 *    ever been displayed yet — honest "still unknown").
 *  - A gap since the previous push of at least
 *    `FF_BATT_FILTER_STALE_GAP_MS` clears the history first (see that
 *    constant's doc comment) — the new sample then starts a fresh
 *    window rather than blending with pre-gap readings.
 *  - The valid reading is pushed into the ring buffer (overwriting the
 *    oldest once full) and the MEDIAN of everything currently held is
 *    computed.
 *  - FIRST EVER real sample (`!has_displayed`): the median (which, with
 *    exactly one sample, equals that sample) is shown immediately, no
 *    hysteresis, no warm-up delay — "unknown-until-read, then real",
 *    the same honesty posture `ff_shell_set_heading`'s -1 sentinel
 *    documents on the input side.
 *  - Every subsequent push: the median is compared against the
 *    currently displayed value. The display updates to the median if
 *    EITHER (a) they differ by at least `FF_BATT_HYSTERESIS_PCT`, OR
 *    (b) updating would cross the `FF_BATT_LOW_PCT` (`ff_radar.h`)
 *    boundary — i.e. exactly one of {old displayed value, new median}
 *    is `ff_radar_batt_is_low`-true and the other is not. (b) exists
 *    because the low-battery alert (S06's amber status-bar tint, the
 *    launcher's amber status row) must never be delayed by up to
 *    `FF_BATT_HYSTERESIS_PCT - 1` percentage points of hysteresis
 *    lag — a puck reading 16% must be free to show 15% (and the alert
 *    that comes with it) on the very next push that crosses, not stall
 *    at 16% waiting for a 2-point move that a slow, real discharge may
 *    take many ticks to accumulate. This is a ONE-WAY exemption in
 *    effect (the interesting crossing is always downward, into low
 *    battery) but is written as a symmetric boundary-crossing test —
 *    crossing back out of low range gets the same promptness, which is
 *    equally honest and costs nothing extra to implement uniformly.
 *
 * NULL `f`: returns -1, no-op otherwise.
 */
int8_t ff_batt_filter_push(ff_batt_filter_t *f, uint16_t pack_mv, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* FF_BATT_H */
