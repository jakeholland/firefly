/**
 * ff_power.h — battery power latch + PWR/BOOT button sampling + soft
 * power-off for the Waveshare ESP32-S3-Touch-LCD-1.46 (S25 slices a+b /
 * S26 slice b).
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
 * does NOT live in this component either (no display dependency in a
 * two-pin GPIO HAL).
 */
#pragma once

#include <stdbool.h>

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

#ifdef __cplusplus
}
#endif
