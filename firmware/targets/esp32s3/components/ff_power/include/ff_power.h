/**
 * ff_power.h — battery power latch + PWR/BOOT button sampling + soft
 * power-off + battery-sense ADC for the Waveshare ESP32-S3-Touch-LCD-1.46
 * (S25 slices a+b+c / S26 slice b).
 *
 * The board's battery is gated by a soft-latch power circuit, not hard-wired
 * to the system rail. Pressing PWR momentarily powers the rail; firmware must
 * then drive the SYS_EN hold line (GPIO7) high to keep the latch closed, or
 * the rail drops the instant PWR is released. See docs/specs/S25-power-latch.md
 * and the board's reference driver (PWR_Key.c) for the hardware contract.
 *
 * This is a pure HAL — no core/domain logic. `ff_power_pwr_pressed()` and
 * `ff_power_boot_pressed()` only sample a pin level; the press-timing
 * DECISION (short tap / long hold / the reboot BOOT-release guard) is
 * `core/include/ff_power_fsm.h`'s job (CLAUDE.md's house rule: "all logic
 * goes in firmware/core/"). `ff_power_off()` only drives a pin — the
 * caller (app_main) decides WHEN to call it and composes it with the
 * backlight-off call (`ff_display_set_brightness(0)`), which deliberately
 * does NOT live in this component either (no display dependency here).
 * `ff_power_batt_mv()` (S25 slice c) is the same discipline applied to
 * the battery-sense ADC: it returns a millivolt READING, never a
 * percent — turning a voltage into a charge percent (and smoothing it
 * across ticks) is `core/include/ff_batt.h`'s job, fed by
 * `ff_shell_set_batt_mv` (app/include/ff_shell.h) one layer up.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Latch battery power ON: configure SYS_EN (GPIO7) as a push-pull output and
 * drive it high. MUST be called as the FIRST thing in app_main — on battery
 * the board only stays powered while PWR is physically held, so the latch has
 * to be asserted before the user's finger lifts (i.e. before the tens of ms
 * the display bring-up spends in reset pulses).
 *
 * Returns ESP_OK on success; on failure returns the underlying gpio error
 * (already logged). A failure is non-fatal to boot — the caller should log and
 * continue, not park.
 */
esp_err_t ff_power_latch_on(void);

/**
 * ff_power_off — soft power-off: drive SYS_EN (GPIO7) LOW. On battery this
 * cuts the rail (the puck goes dark); on USB the board stays up (USB feeds
 * the rail directly, bypassing the latch — same asymmetry
 * `ff_power_latch_on`'s doc comment and docs/specs/S25-power-latch.md
 * describe for the opposite direction). Deliberately does NOT touch the
 * backlight — see this header's top comment; the caller (app_main) calls
 * `ff_display_set_brightness(0)` itself, in the same place it calls this.
 *
 * Returns ESP_OK on success; the underlying gpio error (already logged)
 * on failure. Safe to call even if `ff_power_latch_on` was never called
 * or failed (configures the pin as an output here too if needed).
 */
esp_err_t ff_power_off(void);

/**
 * ff_power_pwr_pressed — sample the PWR key (GPIO6), configuring it as an
 * input on first call. Returns the DEBOUNCED-NOTHING raw level, translated
 * through the active-level interpretation below — feed this straight into
 * `ff_power_fsm_tick()`'s `pwr_pressed` parameter every tick; the FSM does
 * the debouncing.
 *
 * ## The active-level interpretation call (verify on glass)
 * The board's reference driver (Waveshare's PWR_Key.c) reads the button
 * HELD at boot as GPIO6 LOW (`!gpio_get_level`) — i.e. active-LOW. This
 * function follows that reading by default, but it is exactly the kind of
 * "the reference driver implies X, but nothing in this repo has run on
 * the real button yet" call this project's standing rule (AGENTS.md: "if
 * a spec is ambiguous, note the interpretation") exists for — flagged in
 * the PR body, not silently assumed correct. `FF_POWER_PWR_ACTIVE_LOW` in
 * ff_power.c is the ONE `#define` that would need to flip if bring-up on
 * glass shows the button reads the other way; nothing else in this header
 * or its callers encodes the polarity. The RAW level is also logged
 * (ESP_LOGI) on the first call, unconditionally, specifically so a bring-up
 * session can see what the pin actually reads before trusting the
 * translated boolean.
 */
bool ff_power_pwr_pressed(void);

/**
 * ff_power_boot_pressed — sample the BOOT key (GPIO0), configuring it as
 * an input with an internal pull-up on first call. GPIO0 is only special
 * at RESET (held LOW enters the ROM bootloader); once the app is running
 * it is a normal input, per docs/specs/S26-device-lifecycle.md's nav
 * model ("GPIO0 is a normal input once booted"). Active-LOW, the
 * ESP32-family BOOT-button convention (pressed pulls the pin to ground) —
 * NOT an interpretation call the way `ff_power_pwr_pressed` is, since this
 * is standard across every ESP32-S3 dev board's strapping description,
 * not read off one board's third-party reference driver.
 *
 * Feed this into `ff_power_fsm_reboot_ready()`'s `boot_pressed` parameter
 * (the reboot BOOT-release guard, S26 AC4) every tick.
 */
bool ff_power_boot_pressed(void);

/** Capacity of the BOOT-edge ISR ring `ff_power_boot_take_edges` drains
 * (`[api]` fix/quick-flare-detection, 2026-09-03) — public so a caller
 * (app_main.c) can size a same-length local drain buffer without
 * guessing. 8 is a small, cheap-to-allocate-on-the-stack power of two,
 * comfortably larger than a real 5-press quick-flare burst could ever
 * fill between two render-loop ticks (~20ms apart) even at an
 * unrealistically fast human tapping cadence. */
#define FF_POWER_BOOT_EDGE_RING_LEN 8u

/**
 * ff_power_boot_isr_init — `[api]` fix/quick-flare-detection (2026-09-03):
 * arm a GPIO0 falling-edge ISR that timestamps every real BOOT press with
 * `esp_timer_get_time()` (microsecond hardware timer, independent of the
 * render loop's cadence) into a small ring buffer — see ff_power.c's own
 * "BOOT edge ISR + edge ring" comment for the full "why" (worst-case
 * timing arithmetic is in this PR's own body / docs/specs/S10-flare.md's
 * Amendments). Call ONCE, at startup, before the render loop's first
 * tick. Does NOT change `ff_power_boot_pressed()`'s own behavior or the
 * ordinary debounced HOME-dispatch path (`ff_button_tick` +
 * `ff_shell_home_press`) at all — this is a second, additional signal
 * that feeds ONLY the multitap counter (`ff_shell_multitap_edge`,
 * app/include/ff_shell.h), via `ff_power_boot_take_edges` below.
 *
 * Returns the underlying `esp_err_t` (already logged) on failure — a
 * puck whose ISR fails to install still boots and is fully usable;
 * quick-flare timing simply degrades to what the debounced-tick path
 * alone would give (the exact behavior this codebase shipped with
 * before this PR), never a boot failure or a crash.
 */
esp_err_t ff_power_boot_isr_init(void);

/**
 * ff_power_boot_take_edges — drain up to `max` ISR-captured BOOT press
 * edges (oldest first, each a millisecond `esp_timer_get_time()/1000`
 * reading of when that real falling edge fired) into `out_ms`. Returns
 * how many were actually drained (0 if none pending, or if
 * `ff_power_boot_isr_init` was never called/failed — the honest "no
 * signal" case, same posture as `ff_power_batt_mv`'s "never
 * initialized" 0). Call once per render-loop tick and feed every
 * drained timestamp straight to `ff_shell_multitap_edge`
 * (app/include/ff_shell.h) — see that function's own doc comment for
 * why per-edge timestamps (rather than the tick's own `now_ms`) are
 * what make the multitap FSM robust to a slow/late drain.
 *
 * `out_ms` may be NULL (drains and discards) and `max` may be 0
 * (returns 0, drains nothing).
 */
size_t ff_power_boot_take_edges(uint32_t *out_ms, size_t max);

/**
 * ff_power_boot_isr_suspend_for_sleep / ff_power_boot_isr_rearm_after_sleep
 * / ff_power_boot_isr_synthesize_wake_edge — the light-sleep interaction
 * the BOOT edge ISR needs, `[api]` fix/quick-flare-detection (2026-09-03;
 * split into three functions + a disable/re-enable ordering fix,
 * 2026-09-04, review round 2 — see ff_power.c's own doc comments for the
 * exact race this closes: GPIO0's edge-ISR interrupt was staying enabled
 * into sleep, so a still-held BOOT could re-trigger the handler once the
 * pin's type flipped to the level-triggered wake type, before the wake
 * edge was ever cleanly synthesized).
 *
 * ESP light-sleep GPIO wake only supports LEVEL interrupt types (not
 * EDGE), and GPIO0's hardware intr-type register can only hold one type
 * at a time — so the edge ISR (`GPIO_INTR_NEGEDGE`, armed by
 * `ff_power_boot_isr_init`) and the light-sleep wake config
 * (`GPIO_INTR_LOW_LEVEL`) cannot both be armed at once. Call order,
 * exactly:
 *  1. `ff_power_boot_isr_suspend_for_sleep()` immediately before
 *     `esp_light_sleep_start()` — disables the edge interrupt FIRST,
 *     then arms the level wake. Nothing else touches GPIO0 in between.
 *  2. `esp_light_sleep_start()`.
 *  3. `ff_power_boot_isr_rearm_after_sleep()` IMMEDIATELY after that
 *     call returns — no logging, no `esp_sleep_get_wakeup_cause()` read,
 *     nothing else first. Re-arms `GPIO_INTR_NEGEDGE` and re-enables the
 *     interrupt for the new ACTIVE period.
 *  4. THEN read the wake cause, log it, determine whether THIS wake
 *     looks like it was caused by BOOT specifically — the caller's own
 *     best determination, since this chip's light-sleep GPIO wake path
 *     has no per-pin status readback (`esp_sleep_get_gpio_wakeup_status`
 *     is compiled out for ESP32-S3 here, gated behind
 *     `SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP`; app_main.c's own light-sleep
 *     block documents the level-read fallback it uses instead:
 *     `wake_cause == ESP_SLEEP_WAKEUP_GPIO && ff_power_boot_pressed()`,
 *     sound because the wake trigger is LEVEL not EDGE, so a BOOT-caused
 *     wake's pin is still low the instant execution resumes).
 *  5. `ff_power_boot_isr_synthesize_wake_edge(wake_was_boot_gpio)` —
 *     when true, pushes a synthesized edge at the current
 *     `esp_timer_get_time()`, UNLESS a real edge was already pushed
 *     within `FF_POWER_BOOT_EDGE_DEDUP_MS` of it (the just-re-armed ISR
 *     may have already captured this exact press — see this function's
 *     own doc comment, ff_power.c, for when that race window exists).
 *
 * See each function's own doc comment (ff_power.c) for the full
 * reasoning. All three are safe no-ops if the edge ISR was never
 * installed.
 */
void ff_power_boot_isr_suspend_for_sleep(void);
void ff_power_boot_isr_rearm_after_sleep(void);
void ff_power_boot_isr_synthesize_wake_edge(bool wake_was_boot_gpio);

/**
 * ff_power_batt_init — S25 slice c: bring up the battery-sense ADC.
 * HARDWARE (from Waveshare's own reference driver, `BAT_Driver.c`,
 * https://github.com/yaosy1997/ESP32-S3-Touch-LCD-1.46-Test/blob/main/main/BAT_Driver/BAT_Driver.c):
 * pack voltage is sensed on **ADC1 channel 7 (GPIO8)**, `ADC_ATTEN_DB_12`,
 * `ADC_BITWIDTH_DEFAULT`, through a 1:3 resistive divider, with a small
 * measured correction (see `ff_power_batt_mv`'s doc comment for the exact
 * arithmetic). Configures an ADC1 oneshot unit + that channel, and
 * attempts calibration in the same tiered order ESP-IDF's own
 * `esp_adc`-family examples use: curve fitting first
 * (`adc_cali_create_scheme_curve_fitting`, the only scheme
 * `ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED` compiles in for this chip —
 * ESP32-S3 has no line-fitting scheme at all, `ADC_CALI_SCHEME_LINE_
 * FITTING_SUPPORTED` is never defined for it), then line fitting on any
 * chip where that scheme *is* compiled in (kept `#if`-guarded, matching
 * the SDK's own portability idiom, even though it never compiles on
 * this board), then no calibration at all. Pure HAL — no percent math
 * here (see `ff_power_batt_mv`); this call only brings the ADC up.
 *
 * MUST be called after `ff_power_latch_on()` (the battery-keep-alive
 * latch always goes first) but can run anywhere after that — unlike the
 * latch, a few extra ms here costs nothing on either power source.
 * Non-fatal on failure (same "log and continue" posture as every other
 * HAL bring-up call in this file): a puck with no battery-sense ADC
 * still boots and runs; `ff_power_batt_mv()` simply reports 0 (unknown)
 * forever. Returns the underlying `esp_err_t` (already logged) on unit/
 * channel-config failure; `ESP_OK` even when calibration itself could
 * not be established (that degrade is logged, not treated as an error —
 * an uncalibrated ADC is still readable, just not turned into an
 * honest mV figure, see `ff_power_batt_mv`).
 */
esp_err_t ff_power_batt_init(void);

/**
 * ff_power_batt_mv — sample the battery-sense ADC and return the PACK
 * voltage in millivolts, or 0 ("unknown") if `ff_power_batt_init` was
 * never called, never calibrated, or a read fails. `0` matches
 * `ff_shell_set_batt_mv`'s (app/include/ff_shell.h) own documented
 * "no reading" sentinel one layer up — this function never fabricates a
 * plausible-looking figure it does not have real calibrated evidence
 * for (CLAUDE.md: "honest data over pretty data").
 *
 * Averages `FF_BATT_ADC_SAMPLES` raw `adc_oneshot_read` calls (see
 * ff_power.c) before calibrating — cuts sample-to-sample ADC noise
 * before it ever reaches `ff_batt_filter_t`'s own moving-average +
 * Schmitt-hysteresis filter (that filter's job is smoothing across
 * TICKS/load transients, not
 * suppressing single-conversion quantization noise within one tick).
 *
 * ## The conversion arithmetic
 * `adc_cali_raw_to_voltage` returns the voltage AT THE ADC PIN
 * (post-divider) in mV. Getting from there to PACK mV composes two
 * factors, cited in `ff_power.c`'s own top-of-file comment:
 *   1. the board's 1:3 resistive divider — multiply by 3;
 *   2. Waveshare's own measured correction on top of that (their
 *      `BAT_Driver.c` divides the naive ×3 result by 0.990476) — the
 *      silicon's actual divider ratio is not exactly 3:1.
 * Composed into ONE named integer constant
 * (`FF_BATT_PACK_MV_PER_CAL_MV_X1E6`, ff_power_batt_conv.h — a pure,
 * ESP-free header hoisted out of ff_power.c specifically so a HOST test,
 * targets/sim/tests/test_batt_pack_mv.c, can pin the conversion by
 * literal) rather than two separate floating-point multiplies, so the
 * whole device-facing conversion is integer-only.
 *
 * The first successful call logs (ESP_LOGI, once) the raw average, the
 * calibrated pin mV, and the resulting pack mV — specifically so
 * bring-up can sanity-check the reading against a multimeter on the
 * pack — labelled with which calibration scheme is actually in effect
 * (curve / line / NONE — an uncalibrated reading is never silently
 * reported as if it were a real voltage).
 */
uint16_t ff_power_batt_mv(void);

#ifdef __cplusplus
}
#endif
