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
 * NONE of this touches core/ DOMAIN STATE or app/: the screens consume
 * ff_app_state_t and emit ff_intent_t exactly as in the sim; this HAL only
 * provides the display they draw into and the pointer events they
 * hit-test against. This HAL DOES include a couple of core/ headers that
 * are plain values with zero domain logic and zero LVGL types —
 * ff_touchcal.h (the calibration math) and, as of S26g,
 * ff_flare_mark.h (the flare mark's geometry table, shared with
 * app/screens/scr_flare.c so the boot splash draws the identical mark —
 * see ff_display_draw_boot_splash below and that header's own doc
 * comment) — deliberately distinguished from "touches core/": neither
 * reads or writes a single byte of live device state.
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

/** FF_BL_MIN_PCT / FF_BL_MAX_PCT — the backlight percent range
 * `ff_display_set_brightness` clamps a caller's `pct` into (mirrors core
 * FF_BRIGHTNESS_MIN_PCT/_MAX_PCT, ff_settings.h). Public (S26 slice c)
 * so app_main's DIM enact (`ff_display_set_brightness(FF_BL_MIN_PCT)`,
 * docs/specs/S26-device-lifecycle.md "(c) Inactivity -> dim -> screen
 * off") uses the SAME constant this HAL enforces, rather than a second
 * local literal that could drift from it. The floor is non-zero on
 * purpose — see `ff_display_set_brightness`'s doc comment and
 * `ff_display_backlight_off` below for the one true-zero path. */
#define FF_BL_MIN_PCT 10u
#define FF_BL_MAX_PCT 100u

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
 * ff_display_set_flip — format v8 amendment (maintainer ask, 2026-09-02):
 * apply/undo a HARDWARE 180-degree mirror of the panel via
 * `esp_lcd_panel_mirror(panel, flip, flip)`, driven by
 * `ff_settings_t.screen_flip` (the Settings SCREEN NORMAL|FLIPPED row).
 * A hardware mirror, not `lv_display_set_rotation` (a software/LVGL
 * rotation), on purpose — see this function's own definition for the
 * `esp_lcd_spd2010` driver citation confirming `mirror` IS implemented
 * (via a MADCTL write), unlike `swap_xy` (logged unsupported by this
 * panel, see ff_display_lvgl_start's own comment on that log line).
 *
 * The Fusion-designed case mounts the puck upside-down; mirroring BOTH
 * axes (`mirror_x=flip, mirror_y=flip`) is the standard MADCTL trick for
 * a full 180-degree rotation (mirror-X and mirror-Y compose to a
 * point-reflection through the panel centre) — cheaper than a real
 * hardware rotate (which this controller doesn't support at all —
 * `swap_xy` is the piece an actual 90/270 rotation would need) and,
 * unlike `lv_display_set_rotation(LV_DISPLAY_ROTATION_180)`, costs
 * NOTHING extra per frame: the panel does the pixel reordering in
 * silicon, so LVGL's own render/flush path (including the FF_LVGL_STRIP_
 * LINES full-width strip flushing this HAL already relies on) is
 * completely unaffected — no shadow buffer, no per-pixel software
 * rotation pass.
 *
 * Requires `ff_display_panel_init()` to have run (that brings `s_panel`
 * up) — returns ESP_ERR_INVALID_STATE otherwise, and a logged esp_err_t
 * on any panel-IO failure. Safe to call again at ANY time after that,
 * including from a live UI with no reboot: it is a single MADCTL command
 * write, independent of the framebuffer content already flushed (the
 * NEXT flush after this call paints through the new orientation; nothing
 * currently on glass needs to be redrawn for the mirror itself to take
 * effect, though the app still triggers a normal face rebuild so the
 * Settings row and Radar's glass-centred rim tint reflect the new value
 * too — see app_main.c and `ff_theme_glass_cx`/`_cy`, ff_theme.h).
 */
esp_err_t ff_display_set_flip(bool flip);

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
 * ff_display_draw_boot_splash — S26 slice (g): the boot splash, drawn
 * directly via esp_lcd_panel_draw_bitmap exactly like
 * ff_display_draw_test_pattern above — NO LVGL — so it is the earliest
 * possible content on glass, covering the reset pulses + LVGL init
 * latency that otherwise leave the screen black for a few hundred ms
 * (docs/specs/S26-device-lifecycle.md "(g) Boot animation"; see that
 * section's amendment for this change). Wipes the panel to the theme
 * background, then breathes the actual Firefly flare mark — the same
 * 8-ray burst + center dot app/screens/scr_flare.c's takeover screen
 * draws with LVGL (flare_build_mark), rasterized procedurally here
 * instead ("can the boot animation be the flare animation?" — S26g,
 * 2026-09-02) since this path runs before LVGL exists. The ray
 * count/lengths/angles, the dot radius, and the stroke width are the
 * SAME table both draw sites read (core/include/ff_flare_mark.h), so
 * the two cannot silently drift into two different shapes; only the
 * color (amber, matching FF_THEME_COLOR_AMBER) is a value transcribed
 * here rather than included, for the LVGL-coupling reason ff_display.c's
 * own comment on the theme background gives. Centered on the panel
 * (this splash has no headline/buttons to make room for, unlike the
 * takeover screen). Once, in and out, over ~1.1 s total — the spec's
 * timing budget (FF_SPLASH_STEP_MS/FF_SPLASH_HOLD_MS in ff_display.c).
 * No text, no version, no "connecting" claim — honest data: a static
 * mark, nothing this splash cannot itself guarantee is true yet.
 *
 * Requires ff_display_panel_init() first (same precondition as
 * ff_display_draw_test_pattern) and MUST be called before
 * ff_display_lvgl_start() — mixing this with LVGL running is undefined
 * (LVGL owns the panel writes once started). Draws from a small
 * INTERNAL-DMA band buffer AND the mark's ray-endpoint table into
 * stack-local storage, both allocated/computed and released within this
 * call — never a static/.bss allocation, so this splash costs zero
 * bytes of internal DIRAM at rest.
 *
 * Logs ESP_LOGI at the very first pixel drawn (AC1's latch-vs-splash
 * timestamp ordering, read together with ff_power_latch_on's own log)
 * and again on completion with the measured elapsed time. Returns
 * ESP_OK on success; the first failing esp_err_t (logged) otherwise.
 */
esp_err_t ff_display_draw_boot_splash(void);

#if CONFIG_FF_GLASS_RULER
/**
 * ff_display_draw_glass_ruler — device-only diagnostic (CONFIG_FF_GLASS_RULER,
 * default OFF): draws a boot-time "glass ruler" pattern directly to the
 * panel via esp_lcd_panel_draw_bitmap (same raw, no-LVGL path as
 * ff_display_draw_test_pattern / ff_display_draw_boot_splash above) so the
 * maintainer can measure the bezel/pixel-array offset by eye on real
 * glass. See docs/hardware/glass-offset.md for exactly what it draws and
 * how to read dx/dy off it. The caller (app_main, under the same
 * CONFIG_FF_GLASS_RULER guard) holds the pattern on glass forever after
 * this returns — it never hands off to the splash/LVGL/normal boot.
 *
 * Requires ff_display_panel_init() first (same precondition as
 * ff_display_draw_test_pattern). Compiled out entirely — this declaration
 * included — when CONFIG_FF_GLASS_RULER is off (the default), so a field
 * or demo build is byte-identical with or without this diagnostic
 * existing in the tree. Returns ESP_OK on success; the first failing
 * esp_err_t (logged) otherwise.
 */
esp_err_t ff_display_draw_glass_ruler(void);
#endif /* CONFIG_FF_GLASS_RULER */

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
 * ff_display_touch_is_down — true while a finger is physically on the panel
 * (the touch indev is in LV_INDEV_STATE_PRESSED). The render loop uses it to
 * DEFER a face teardown+rebuild until the finger lifts: rebuilding
 * (lv_obj_clean + ff_face_build) between a tap's press and release destroys
 * the button under the finger, so its LV_EVENT_CLICKED never fires — the
 * on-glass "just highlights, won't open / missed tap" report. Returns false
 * when touch has not been started (no indev yet), so the caller never blocks
 * on a device without a live touch panel.
 */
bool ff_display_touch_is_down(void);

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
 * ff_display_backlight_off — S26 slice (c)'s screen-OFF enact: drive the
 * LEDC duty to a TRUE zero, bypassing `ff_display_set_brightness`'s
 * `FF_BL_MIN_PCT` floor. That floor exists so no OTHER caller can
 * accidentally leave the backlight black-and-unrecoverable; the idle
 * FSM's OFF state is the one legitimate "actually dark" caller
 * (docs/specs/S26-device-lifecycle.md "(c) Inactivity -> dim -> screen
 * off", AC2/AC3), so it gets its own explicit entry point rather than
 * weakening that clamp for everyone. Requires `ff_display_panel_init()`
 * to have run, same precondition as `ff_display_set_brightness` —
 * returns ESP_ERR_INVALID_STATE otherwise.
 */
esp_err_t ff_display_backlight_off(void);

/**
 * ff_display_backlight_on — the wake-side counterpart of
 * `ff_display_backlight_off`: restore the backlight to `pct` percent.
 * A thin, explicitly-named alias for `ff_display_set_brightness(pct)`
 * (same clamp, same precondition, same LEDC path) — kept as its own
 * symbol so `app_main.c`'s ACTIVE/DIM/OFF enact site reads as three
 * parallel calls (`_on`/`ff_display_set_brightness(FF_BL_MIN_PCT)` for
 * DIM/`_off`) rather than one of the three being a bare call into a
 * function named for a general-purpose setter. Callers restoring the
 * pre-dim brightness on wake pass the EXACT stored `settings.brightness_pct`
 * here — never a hardcoded value (AC2).
 */
esp_err_t ff_display_backlight_on(uint8_t pct);

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
