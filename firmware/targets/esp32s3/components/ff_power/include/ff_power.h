/**
 * ff_power.h — battery power latch for the Waveshare ESP32-S3-Touch-LCD-1.46
 * (S25 slice a).
 *
 * The board's battery is gated by a soft-latch power circuit, not hard-wired
 * to the system rail. Pressing PWR momentarily powers the rail; firmware must
 * then drive the SYS_EN hold line (GPIO7) high to keep the latch closed, or
 * the rail drops the instant PWR is released. See docs/specs/S25-power-latch.md
 * and the board's reference driver (PWR_Key.c) for the hardware contract.
 *
 * This is a pure HAL — no core/domain logic. The button read + soft power-off
 * (GPIO6, the sleep/shutdown state machine) are deferred slices; the FSM there
 * belongs in core, this component only samples/drives the pins.
 */
#pragma once

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

#ifdef __cplusplus
}
#endif
