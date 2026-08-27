/**
 * ff_display.h — ESP32-S3 display + touch HAL for the Waveshare
 * ESP32-S3-Touch-LCD-1.46 (S15 slice b).
 *
 * The device analogue of the sim's SDL display + pointer indev. Brings up
 * the round 412x412 SPD2010 QSPI panel and its SPD2010 I2C touch
 * controller, and exposes them as an LVGL v9 lv_display + lv_indev so the
 * UNCHANGED app/screens render on glass and a physical tap/swipe feeds the
 * SAME abstract input events the sim ctl socket injects.
 *
 * The functions are ordered as the bring-up gates are (S15b's b1/b2/b3):
 * each is idempotent-ish only in that app_main calls them once, in order,
 * for the selected stage. Every step logs (ESP_LOGI/E, tag "ff_display")
 * so a dark screen still tells the maintainer how far it got — the whole
 * point of the staged bring-up.
 *
 * NONE of this touches core/ or app/: the screens consume ff_app_state_t
 * and emit ff_intent_t exactly as in the sim; this HAL only provides the
 * display they draw into and the pointer events they hit-test against.
 */
#ifndef FF_DISPLAY_H
#define FF_DISPLAY_H

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "ff_touchcal.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_display_expander_init — bring up the shared I2C bus and the TCA9554
 * IO expander, then RELEASE both hardware resets through it (TP_RST on
 * EXIO1, LCD_RST on EXIO2). This MUST run before the panel init: LCD_RST
 * is not a GPIO, so a panel driver that never sees this stays dark with no
 * error (S15b's documented #1 failure mode). Logs the expander ACK, the
 * register read-back, and the reset release.
 *
 * Returns ESP_OK on success; the first failing esp_err_t otherwise (and
 * logs which step failed).
 */
esp_err_t ff_display_expander_init(void);

/**
 * ff_display_panel_init — bring up the SPD2010 over QSPI and turn the
 * backlight on. Requires ff_display_expander_init() to have released
 * LCD_RST first. On success the panel is initialised, oriented, and
 * displaying (blank) — ready for ff_display_draw_test_pattern (b1) or
 * ff_display_lvgl_start (b2). Logs each step (bus, panel IO, panel init,
 * backlight).
 */
esp_err_t ff_display_panel_init(void);

/**
 * ff_display_draw_test_pattern — b1 "first light": paint a solid colour
 * fill over the whole 412x412 panel, then a two-colour split (left half
 * one colour, right half another) via esp_lcd_panel_draw_bitmap. The
 * split proves orientation (which half is "left") and the absence of an
 * RGB565 byte-swap error (a swapped panel renders the wrong colours).
 * NO LVGL is involved — this isolates the panel from the GUI stack.
 * Logs when each draw completes.
 */
esp_err_t ff_display_draw_test_pattern(void);

/**
 * ff_display_lvgl_start — b2: initialise esp_lvgl_port and add an LVGL v9
 * lv_display backed by this panel (PSRAM double buffer, RGB565). After
 * this returns, lv_screen_active() draws to the glass and the caller
 * builds a real ff_shell face on it. Returns the lv_display (NULL on
 * failure, logged). Requires ff_display_panel_init() first.
 */
lv_display_t *ff_display_lvgl_start(void);

/**
 * ff_display_touch_start — b3: bring up the SPD2010 I2C touch controller
 * (TP_RST already released by the expander init) and add an LVGL pointer
 * indev on `disp`. This is the device's ONLY input path: touch -> the
 * lv_indev -> LVGL hit-testing -> the screens' existing intent emits ->
 * the bound ff_shell_intent_sink — the same seam the sim's synthetic
 * pointer indev drives (targets/sim/ctl_loop.c). No second input path.
 * Logs the touch init; per-tap coordinate logging is enabled so raw
 * coords appear in the serial log on every press (honest: uncalibrated
 * coords are logged as-is, not massaged).
 *
 * Returns ESP_OK on success; a logged esp_err_t otherwise.
 */
esp_err_t ff_display_touch_start(lv_display_t *disp);

/**
 * ff_display_set_brightness — set the backlight to `pct` percent via LEDC
 * PWM (#100). Clamped to [10, 100]: 0 would be a black, unrecoverable
 * screen, so the floor is non-zero (this mirrors, and defensively
 * re-applies, the same clamp the shell puts on the stored setting). The app
 * forwards ff_settings_t.brightness_pct here on boot and on every change;
 * core never touches this. Requires ff_display_panel_init() to have run
 * (that brings the LEDC timer/channel up) — returns ESP_ERR_INVALID_STATE
 * otherwise, and a logged esp_err_t on any LEDC failure.
 */
esp_err_t ff_display_set_brightness(uint8_t pct);

/**
 * ff_display_touch_set_cal — install the active touch-calibration
 * transform (S15 slice d). Every subsequent physical touch is run through
 * ff_touchcal_apply in the SAME seam that feeds LVGL (the esp_lcd_touch
 * process_coordinates callback), BEFORE the coord reaches LVGL/the shell —
 * so gestures, long-press, and buttons are all corrected via one path, no
 * second input route. Passing an invalid (`!valid`) or NULL cal restores
 * identity (raw passes through, still clamped to the panel). Safe to call
 * before ff_display_touch_start (the value is simply stored until the
 * indev exists) or at any time after. `c` is copied; the caller need not
 * keep it alive.
 */
void ff_display_touch_set_cal(const ff_touchcal_t *c);

/**
 * ff_display_run_calibration — S15 slice d: the crosshair capture flow.
 * Renders a crosshair at each of the five spec targets in turn (center
 * 206,206 + the four insets), with "tap the target (N/5)" text, and
 * captures ONE raw tap per target (one capture per stable press/release).
 * The five (raw -> screen) pairs are fed to ff_touchcal_solve; the result
 * (and every captured pair) is ESP_LOGI'd. Returns the solved transform in
 * `*out_cal` (identity/!valid if the capture was degenerate).
 *
 * MUST run after ff_display_lvgl_start() and ff_display_touch_start(), and
 * with the active cal at identity (so the captured taps are raw) — the
 * caller installs the solved cal via ff_display_touch_set_cal afterward.
 * Blocks the calling task until all five targets are captured. Returns
 * ESP_OK on a completed capture; a logged error otherwise.
 */
esp_err_t ff_display_run_calibration(ff_touchcal_t *out_cal);

/**
 * ff_display_lock / ff_display_unlock — take/release the LVGL API mutex.
 * esp_lvgl_port runs LVGL in its OWN task; any LVGL call from another task
 * (app_main building a face) must be bracketed by these. `timeout_ms` of 0
 * waits forever. Returns false on timeout. Only meaningful after
 * ff_display_lvgl_start(); before that there is no port task and callers
 * simply do not touch LVGL.
 */
bool ff_display_lock(uint32_t timeout_ms);
void ff_display_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* FF_DISPLAY_H */
