/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a new SPD2010 touch driver
 *
 * @note  The I2C communication should be initialized before use this function.
 *
 * @param io LCD panel IO handle, it should be created by `esp_lcd_new_panel_io_i2c()`
 * @param config Touch panel configuration
 * @param tp Touch panel handle
 * @return
 *      - ESP_OK: on success
 */
esp_err_t esp_lcd_touch_new_i2c_spd2010(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config, esp_lcd_touch_handle_t *tp);

/**
 * @brief [api] Touch-read health counter (2026-09-08 QA hardening).
 *
 * A failing multi-transaction read (e.g. a cracked panel NACKing/timing
 * out) is counted here instead of only flooding the console — see the
 * FIREFLY PATCH #6 block in esp_lcd_touch_spd2010.c for the full story.
 * Either output pointer may be NULL.
 *
 * @param out_total_fail   lifetime count of failed tp_read_data() calls, or NULL
 * @param out_fail_per_min the most recently CLOSED ~60s window's failure
 *                         count ("failures per minute"), or NULL — 0 both
 *                         when nothing has failed and during the first
 *                         still-open window after a failure starts
 */
void esp_lcd_touch_spd2010_touch_health(uint32_t *out_total_fail, uint32_t *out_fail_per_min);

/**
 * @brief I2C address of the SPD2010 controller
 *
 */
#define ESP_LCD_TOUCH_IO_I2C_SPD2010_ADDRESS     (0x53)

/**
 * @brief Per-transaction I2C timeout for this touch controller (2026-09-08
 * QA hardening). `esp_lcd_panel_io_i2c_config_t.transaction_timeout_ms`
 * (esp_lcd_io_i2c.h) is "0/-1: wait forever, >0: finite timeout" — left
 * unset (as upstream's own macro left it, and as this file's own vendored
 * copy shipped until this fix), every `i2c_master_transmit`/`_receive`/
 * `_transmit_receive` call this touch controller's read path makes
 * (`esp_lcd_panel_io_tx_param`/`rx_param`, this file's `i2c_write`/
 * `i2c_read` macros) blocks the CALLING TASK — the esp_lvgl_port task,
 * since the touch poll runs from its indev read callback — with NO
 * timeout at all if a transaction genuinely never completes (SDA stuck
 * low, a hardware fault the driver's own retry/NACK path doesn't cover).
 * That is exactly the "a stalled I2C transaction can hang the LVGL task
 * forever" risk this fix closes. 20 ms matches `ff_compass.c`'s own
 * `FF_COMPASS_I2C_TIMEOUT_MS` on the SAME shared bus (ff_display.h's
 * `ff_display_i2c_bus_lock` doc comment) — ample for a genuine multi-byte
 * transfer at 100 kHz (sub-millisecond), short enough that a stuck
 * transaction fails fast and reports up through this driver's own
 * ESP_RETURN_ON_ERROR chain (-> read_data()'s health counter, see
 * esp_lcd_touch_spd2010.c) rather than blocking indefinitely.
 *
 * NOT AVAILABLE ON EVERY TOOLCHAIN: this field only exists on ESP-IDF
 * builds that carry upstream commit 69c97183c3f ("feat(lcd): add
 * configurable timeout for lcd i2c panel"), which landed on the
 * release/v5.3 branch AFTER the v5.3.5 tag and is not yet in any tagged
 * release — CI's pinned v5.3.5 build does NOT have it, even though this
 * project's minimum-supported ESP-IDF is otherwise 5.3.5. This
 * component's own CMakeLists.txt detects the field at configure time
 * (`check_struct_has_member`, since ESP_IDF_VERSION reports 5.3.5
 * either way and can't tell the two apart) and defines
 * FF_ESP_LCD_I2C_HAS_TRANSACTION_TIMEOUT only when it's really there —
 * see ESP_LCD_TOUCH_IO_I2C_SPD2010_CONFIG() below. Where it's absent,
 * this one specific protection silently reduces to the pre-fix
 * behavior (transactions can block their calling task indefinitely on
 * a truly stuck bus) — the render-loop task watchdog and LVGL-liveness
 * probe (app_main.c, same QA hardening pass) are the backstop in that
 * case: a 15s TWDT trip instead of a 20ms transaction-level bound, but
 * still a recoverable log line (or, on qa/wdt-panic-optin, a reboot)
 * rather than a device stuck forever.
 */
#define ESP_LCD_TOUCH_I2C_SPD2010_TRANSACTION_TIMEOUT_MS (20)

/**
 * @brief Touch IO configuration structure
 *
 */
#ifdef FF_ESP_LCD_I2C_HAS_TRANSACTION_TIMEOUT
#define ESP_LCD_TOUCH_IO_I2C_SPD2010_CONFIG()               \
    {                                                       \
        .scl_speed_hz = 100000,                             \
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_SPD2010_ADDRESS,   \
        .control_phase_bytes = 1,                           \
        .dc_bit_offset = 0,                                 \
        .lcd_cmd_bits = 0,                                  \
        .transaction_timeout_ms = ESP_LCD_TOUCH_I2C_SPD2010_TRANSACTION_TIMEOUT_MS, \
        .flags =                                            \
        {                                                   \
            .disable_control_phase = 1,                     \
        }                                                   \
    }
#else
#define ESP_LCD_TOUCH_IO_I2C_SPD2010_CONFIG()               \
    {                                                       \
        .scl_speed_hz = 100000,                             \
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_SPD2010_ADDRESS,   \
        .control_phase_bytes = 1,                           \
        .dc_bit_offset = 0,                                 \
        .lcd_cmd_bits = 0,                                  \
        .flags =                                            \
        {                                                   \
            .disable_control_phase = 1,                     \
        }                                                   \
    }
#endif

#ifdef __cplusplus
}
#endif
