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
 * @brief Touch IO configuration structure
 *
 */
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

#ifdef __cplusplus
}
#endif
