/**
 * ff_display.c — Waveshare ESP32-S3-Touch-LCD-1.46 display + touch HAL.
 *
 * Every hardware constant below was read from Waveshare's OWN ESP-IDF
 * 5.3.2 demo (github.com/waveshareteam/ESP32-S3-Touch-LCD-1.46, folder
 * example/ESP-IDF-5.3.2/.../main/: EXIO/TCA9554PWR, LCD_Driver/
 * Display_SPD2010, Touch_Driver/Touch_SPD2010, I2C_Driver) and
 * cross-checked against the espressif registry driver docs. Where the two
 * disagree the choice is noted inline. The SPD2010 init sequence is NOT
 * hand-rolled — the esp_lcd_spd2010 registry driver vendors it; we only
 * pass the panel/vendor config the demo uses (QSPI, RGB, 16bpp).
 *
 * The single most important ordering rule (S15b's "#1 failure mode"):
 * LCD_RST and TP_RST are NOT GPIOs — they hang off the TCA9554 IO
 * expander (EXIO2 / EXIO1). ff_display_expander_init() MUST run and pulse
 * those resets before ff_display_panel_init(), or the panel stays dark
 * with no error.
 */
#include "ff_display.h"

#include <string.h>

#include "ff_touchcal.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_io_expander_tca9554.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_spd2010.h"
#include "esp_lcd_touch_spd2010.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ff_display";

/* ---- Panel geometry (round glass addressed as a square) --------------- */
#define FF_LCD_H_RES 412
#define FF_LCD_V_RES 412
#define FF_LCD_BITS_PER_PIXEL 16

/* ---- Panel column/row offset (esp_lcd_panel_set_gap) ------------------
 * The esp_lcd_spd2010 driver adds x_gap/y_gap to the CASET/RASET start it
 * programs for every draw_bitmap, i.e. a POSITIVE gap shifts what we write
 * further into the panel's GRAM. If the visible window starts a few px into
 * GRAM, a gap of 0 leaves the rightmost/bottommost few px pointing at
 * unwritten GRAM — showing stale/wrapped content on the right & bottom
 * edges (the artifact seen faintly at STAGE 1/2). A matching positive gap
 * pulls the image back so edge N maps to visible edge N.
 *
 * RESEARCHED VALUE = 0,0: BOTH official sources for this exact 412x412
 * SPD2010 glass ship NO gap — Waveshare's own ESP-IDF-5.3.2 demo
 * (Display_SPD2010.c draws 0-origin, never calls set_gap) and Espressif's
 * esp_lcd_spd2010 test app (test_esp_lcd_spd2010.c, 412x412, no set_gap).
 * No non-zero offset is documented anywhere I could find. So 0,0 is the
 * only grounded value and — importantly — it is a NO-OP that cannot regress
 * the hardware-verified render.
 *
 * TUNING (maintainer, on glass): if the few-px right/bottom wrap persists,
 * nudge these UP by the number of stale pixels (they are small, "a few px";
 * try 2, then 4). X is the SPD2010's 4-px-aligned axis, so an x value that
 * is a multiple of 4 is the safe first guess. Increasing the gap moves the
 * image toward the top-left; if the wrap instead appears on the LEFT/TOP,
 * the value is too high. Leave the drawn resolution (412) and orientation
 * untouched — only these two numbers change. */
#define FF_LCD_X_GAP 0
#define FF_LCD_Y_GAP 0

/* ---- Display QSPI pins (Display_SPD2010.h) --------------------------- */
#define FF_LCD_HOST SPI2_HOST
#define FF_PIN_LCD_SCK 40
#define FF_PIN_LCD_D0 46
#define FF_PIN_LCD_D1 45
#define FF_PIN_LCD_D2 42
#define FF_PIN_LCD_D3 41
#define FF_PIN_LCD_CS 21
#define FF_PIN_LCD_TE 18 /* tearing-effect; wired but unused this slice (see note) */
#define FF_PIN_LCD_BL 5  /* backlight, active-high — driven via LEDC PWM (#100), matching Waveshare's demo */
#define FF_LCD_PCLK_HZ (80 * 1000 * 1000)

/* ---- Backlight LEDC PWM (#100) --------------------------------------
 * Waveshare's own ESP-IDF-5.3.2 demo drives BL (GPIO5) through the LEDC
 * peripheral (LCD_Backlight / Set_Backlight in its Backlight.c), NOT a bare
 * GPIO level — so brightness is a duty cycle, not on/off. We mirror that:
 * a low-speed-mode timer + one channel on GPIO5. 10-bit resolution (duty
 * 0..1023) is ample for a smooth 10..100% range; 5 kHz is well above the
 * flicker floor and below any audible-whine range. The MIN/MAX below mirror
 * core's FF_BRIGHTNESS_MIN_PCT / _MAX_PCT (ff_settings.h) — kept as local
 * literals so this device HAL stays free of any core/ include (its header
 * comment: "NONE of this touches core/ or app/"). The floor is a DEFENSIVE
 * second clamp: the shell already clamps the setting to the same range, but
 * the HAL never trusts a caller to have done so — a 0% duty is a black,
 * unrecoverable backlight, so ff_display_set_brightness can never program
 * one. */
#define FF_BL_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define FF_BL_LEDC_TIMER   LEDC_TIMER_0
#define FF_BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define FF_BL_LEDC_RES     LEDC_TIMER_10_BIT
#define FF_BL_LEDC_FREQ_HZ 5000
#define FF_BL_MIN_PCT      10  /* mirrors core FF_BRIGHTNESS_MIN_PCT — never 0/black-unrecoverable */
#define FF_BL_MAX_PCT      100 /* mirrors core FF_BRIGHTNESS_MAX_PCT */

/* ---- Shared I2C bus: touch (0x53) + TCA9554 (0x20) (I2C_Driver.h) ----- */
#define FF_I2C_PORT I2C_NUM_0
#define FF_PIN_I2C_SDA 11
#define FF_PIN_I2C_SCL 10
#define FF_PIN_TP_INT 4
#define FF_I2C_HZ 400000

/* ---- IO expander reset lines (TCA9554PWR.*, Display/Touch_SPD2010.*) --
 * EXIO1 (expander P1) = TP_RST, EXIO2 (expander P2) = LCD_RST, both
 * active-LOW. The demo pulses LCD low 100ms / high 100ms and touch low
 * 50ms / high 50ms. */
#define FF_EXIO_TP_RST IO_EXPANDER_PIN_NUM_1
#define FF_EXIO_LCD_RST IO_EXPANDER_PIN_NUM_2

/* File-static handles, brought up in order by the functions below. */
static i2c_master_bus_handle_t s_i2c_bus;
static esp_io_expander_handle_t s_io_exp;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static lv_indev_t *s_touch_indev; /* the LVGL pointer indev the panel's touch drives (see ff_display_touch_indev) */
static lv_display_t *s_lv_disp;
static bool s_bl_ready; /* true once the LEDC backlight timer+channel are configured */

/* =====================================================================
 * Backlight PWM (#100). LEDC timer + one channel on GPIO5, mirroring the
 * Waveshare demo. ff_display_set_brightness maps a clamped percent onto the
 * duty range; the initial duty is full-on so the first-light stages (b1) are
 * bright before the shell's stored brightness is ever applied.
 * ===================================================================== */
static esp_err_t ff_display_backlight_init(void)
{
    const ledc_timer_config_t timer_cfg = {
        .speed_mode = FF_BL_LEDC_MODE,
        .timer_num = FF_BL_LEDC_TIMER,
        .duty_resolution = FF_BL_LEDC_RES,
        .freq_hz = FF_BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight ledc_timer_config failed: %s", esp_err_to_name(err));
        return err;
    }

    const uint32_t full_duty = (1u << FF_BL_LEDC_RES) - 1u; /* start full-on for bring-up */
    const ledc_channel_config_t ch_cfg = {
        .gpio_num = FF_PIN_LCD_BL,
        .speed_mode = FF_BL_LEDC_MODE,
        .channel = FF_BL_LEDC_CHANNEL,
        .timer_sel = FF_BL_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .duty = full_duty,
        .hpoint = 0,
    };
    err = ledc_channel_config(&ch_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight ledc_channel_config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_bl_ready = true;
    ESP_LOGI(TAG, "backlight LEDC up (GPIO%d, %d-bit @ %d Hz, full-on)", FF_PIN_LCD_BL, (int)FF_BL_LEDC_RES,
             FF_BL_LEDC_FREQ_HZ);
    return ESP_OK;
}

esp_err_t ff_display_set_brightness(uint8_t pct)
{
    if (!s_bl_ready) {
        ESP_LOGE(TAG, "set_brightness called before panel_init (backlight LEDC not up)");
        return ESP_ERR_INVALID_STATE;
    }

    /* Defensive clamp — never a 0% (black, unrecoverable) backlight, even if
     * a caller forgets the shell's own clamp (ff_settings.h / shell_setting_set). */
    if (pct < FF_BL_MIN_PCT) pct = FF_BL_MIN_PCT;
    if (pct > FF_BL_MAX_PCT) pct = FF_BL_MAX_PCT;

    const uint32_t full_duty = (1u << FF_BL_LEDC_RES) - 1u;
    const uint32_t duty = (full_duty * (uint32_t)pct) / 100u;

    esp_err_t err = ledc_set_duty(FF_BL_LEDC_MODE, FF_BL_LEDC_CHANNEL, duty);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight ledc_set_duty failed: %s", esp_err_to_name(err));
        return err;
    }
    err = ledc_update_duty(FF_BL_LEDC_MODE, FF_BL_LEDC_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight ledc_update_duty failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "backlight set to %u%% (duty %u/%u)", (unsigned)pct, (unsigned)duty, (unsigned)full_duty);
    return ESP_OK;
}

/* Active touch-calibration transform (S15 slice d). Identity until
 * ff_display_touch_set_cal installs a fit — so an uncalibrated device (and
 * the calibration capture itself) sees raw coords pass through. Read by
 * ff_touchcal_process_cb on every touch poll (LVGL port task); written by
 * ff_display_touch_set_cal. A torn read across the write is harmless — the
 * fields are independent floats and the next poll reads the settled value. */
static ff_touchcal_t s_active_cal = {.ax = 1.0f, .bx = 0.0f, .ay = 1.0f, .by = 0.0f, .valid = false};

/* =====================================================================
 * b1 step 1 — I2C bus + TCA9554 up, both resets released through it.
 * ===================================================================== */
esp_err_t ff_display_expander_init(void)
{
    esp_err_t err;

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = FF_I2C_PORT,
        .sda_io_num = FF_PIN_I2C_SDA,
        .scl_io_num = FF_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "I2C bus up (port %d, SDA=%d SCL=%d, %d Hz)", FF_I2C_PORT, FF_PIN_I2C_SDA,
             FF_PIN_I2C_SCL, FF_I2C_HZ);

    /* TCA9554 @ 0x20 (ADDRESS_000). If the ACK below fails, the expander
     * is the wrong part/address — everything downstream stays dark. */
    err = esp_io_expander_new_i2c_tca9554(s_i2c_bus, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000,
                                          &s_io_exp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TCA9554 @0x20 not found (ACK failed): %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "TCA9554 IO expander ACKed @ 0x20");

    /* EXIO1 (TP_RST) + EXIO2 (LCD_RST) as outputs, idle HIGH (deasserted). */
    err = esp_io_expander_set_dir(s_io_exp, FF_EXIO_TP_RST | FF_EXIO_LCD_RST, IO_EXPANDER_OUTPUT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "expander set_dir failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_io_expander_set_level(s_io_exp, FF_EXIO_TP_RST | FF_EXIO_LCD_RST, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "expander set_level(high) failed: %s", esp_err_to_name(err));
        return err;
    }
    /* Register read-back proves the write landed (ACK + state), logged. */
    esp_io_expander_print_state(s_io_exp);

    /* LCD reset pulse: low 100ms, high 100ms (Display_SPD2010.c). */
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_io_expander_set_level(s_io_exp, FF_EXIO_LCD_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_io_expander_set_level(s_io_exp, FF_EXIO_LCD_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "LCD_RST released (EXIO2, active-low pulse 100/100 ms)");

    /* Touch reset pulse: low 50ms, high 50ms (Touch_SPD2010.c). */
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_io_expander_set_level(s_io_exp, FF_EXIO_TP_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_io_expander_set_level(s_io_exp, FF_EXIO_TP_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "TP_RST released (EXIO1, active-low pulse 50/50 ms)");

    return ESP_OK;
}

/* =====================================================================
 * b1 step 2 — QSPI bus + SPD2010 panel init, backlight on.
 * ===================================================================== */
esp_err_t ff_display_panel_init(void)
{
    esp_err_t err;

    if (s_io_exp == NULL) {
        ESP_LOGE(TAG, "panel_init called before expander_init — LCD_RST never released");
        return ESP_ERR_INVALID_STATE;
    }

    /* QSPI bus. SPD2010_PANEL_BUS_QSPI_CONFIG sets the quad data lines +
     * the QSPI bus flags for us. max_transfer_sz sized for a full-frame
     * flush (H*V*2 bytes) so LVGL's full-refresh buffer flushes in one go. */
    const spi_bus_config_t bus_cfg = SPD2010_PANEL_BUS_QSPI_CONFIG(
        FF_PIN_LCD_SCK, FF_PIN_LCD_D0, FF_PIN_LCD_D1, FF_PIN_LCD_D2, FF_PIN_LCD_D3,
        FF_LCD_H_RES * FF_LCD_V_RES * (FF_LCD_BITS_PER_PIXEL / 8));
    err = spi_bus_initialize(FF_LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "QSPI bus up (host %d, SCK=%d D0..3=%d/%d/%d/%d)", FF_LCD_HOST, FF_PIN_LCD_SCK,
             FF_PIN_LCD_D0, FF_PIN_LCD_D1, FF_PIN_LCD_D2, FF_PIN_LCD_D3);

    const esp_lcd_panel_io_spi_config_t io_cfg =
        SPD2010_PANEL_IO_QSPI_CONFIG(FF_PIN_LCD_CS, NULL, NULL);
    err = esp_lcd_new_panel_io_spi(FF_LCD_HOST, &io_cfg, &s_panel_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Vendor config: QSPI interface, driver-vendored init sequence
     * (no custom init_cmds — the demo uses none either). */
    spd2010_vendor_config_t vendor_cfg = {
        .flags = {.use_qspi_interface = 1},
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1, /* LCD_RST is on EXIO2, already released above */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = FF_LCD_BITS_PER_PIXEL,
        .flags = {.reset_active_high = 0},
        .vendor_config = &vendor_cfg,
    };
    err = esp_lcd_new_panel_spd2010(s_panel_io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_spd2010 failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_reset(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_reset failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_init(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_init failed: %s", esp_err_to_name(err));
        return err;
    }
    /* Column/row offset for the round 412x412 glass. Default 0,0 (both
     * official demos use no gap — see the FF_LCD_*_GAP note above); this is
     * the tuning knob for the faint right/bottom edge wrap, a no-op at 0. */
    err = esp_lcd_panel_set_gap(s_panel, FF_LCD_X_GAP, FF_LCD_Y_GAP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_set_gap failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "panel gap set (x=%d, y=%d)", FF_LCD_X_GAP, FF_LCD_Y_GAP);

    /* Native orientation: the demo sets no mirror/swap for this glass. */
    err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_disp_on_off failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "SPD2010 panel init done (%dx%d RGB565 QSPI)", FF_LCD_H_RES, FF_LCD_V_RES);

    /* Backlight via LEDC PWM (#100), full-on at init so first light is
     * bright before the shell's stored brightness is applied. The app forwards
     * the persisted percent via ff_display_set_brightness once the shell is
     * up (app_main), and on every later change. */
    err = ff_display_backlight_init();
    if (err != ESP_OK) {
        return err; /* already logged */
    }

    return ESP_OK;
}

/* =====================================================================
 * b1 step 3 — first light: solid fill + two-colour split (no LVGL).
 *
 * SPD2010 takes RGB565 big-endian over the wire; ESP32 stores RGB565
 * little-endian, so each pixel is byte-swapped here (the demo does the
 * same with SPI_SWAP_DATA_TX). Whether the swap is CORRECT is exactly
 * what this stage proves: a swapped-wrong panel shows the wrong colours,
 * and the split's left/right halves prove orientation. draw_bitmap x
 * bounds stay full-width (0..412, both divisible by 4 — the SPD2010's
 * documented alignment rule); the colour boundary lives in the pixel
 * data, not the draw rectangle.
 * ===================================================================== */
static inline uint16_t ff_rgb565_swap(uint16_t c)
{
    return (uint16_t)((c >> 8) | (c << 8));
}

esp_err_t ff_display_draw_test_pattern(void)
{
    if (s_panel == NULL) {
        ESP_LOGE(TAG, "draw_test_pattern called before panel_init");
        return ESP_ERR_INVALID_STATE;
    }

    /* Draw in horizontal bands from a small INTERNAL-DMA buffer. A full
     * 412x412 frame (~331 KB) cannot be pushed in one draw_bitmap: sending
     * that from PSRAM in a single SPI transaction returns ESP_ERR_NO_MEM
     * (the SPI/esp_lcd layer can't get enough internal DMA for a transfer
     * that large), and a full-frame INTERNAL buffer will not fit (~240 KB
     * free). BAND_LINES divides 412 evenly (412 = 4*103) so every band is
     * 4-line aligned on the y axis, matching the SPD2010's documented
     * draw-alignment rule; the band buffer is a few KB of internal DMA RAM. */
    enum { BAND_LINES = 4 };
    const size_t band_px = (size_t)FF_LCD_H_RES * BAND_LINES;
    uint16_t *band = heap_caps_malloc(band_px * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (band == NULL) {
        ESP_LOGE(TAG, "test pattern: OOM allocating %u byte band buffer", (unsigned)(band_px * 2));
        return ESP_ERR_NO_MEM;
    }

    /* Solid fill: amber (#FFC66B -> RGB565 0xFE2D). Every band is identical. */
    const uint16_t amber = ff_rgb565_swap(0xFE2D);
    for (size_t i = 0; i < band_px; i++) band[i] = amber;
    for (int y = 0; y < FF_LCD_V_RES; y += BAND_LINES) {
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, FF_LCD_H_RES, y + BAND_LINES, band);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "solid-fill band y=%d draw_bitmap failed: %s", y, esp_err_to_name(err));
            heap_caps_free(band);
            return err;
        }
    }
    ESP_LOGI(TAG, "first light: solid amber fill drawn (%d bands)", FF_LCD_V_RES / BAND_LINES);
    vTaskDelay(pdMS_TO_TICKS(1500));

    /* Two-colour split: left half green (#9BE07B->0x9F6F), right half red
     * (#FF0000->0xF800). Boundary at x=206 lives in pixel data; every band
     * is identical, so fill the band once and repeat it down the screen. */
    const uint16_t green = ff_rgb565_swap(0x9F6F);
    const uint16_t red = ff_rgb565_swap(0xF800);
    for (int ly = 0; ly < BAND_LINES; ly++) {
        uint16_t *row = band + (size_t)ly * FF_LCD_H_RES;
        for (int x = 0; x < FF_LCD_H_RES; x++) {
            row[x] = (x < FF_LCD_H_RES / 2) ? green : red;
        }
    }
    for (int y = 0; y < FF_LCD_V_RES; y += BAND_LINES) {
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, FF_LCD_H_RES, y + BAND_LINES, band);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "split band y=%d draw_bitmap failed: %s", y, esp_err_to_name(err));
            heap_caps_free(band);
            return err;
        }
    }
    ESP_LOGI(TAG, "first light: two-colour split drawn (left=green right=red)");

    heap_caps_free(band);
    return ESP_OK;
}

/* The SPD2010 addresses its GRAM in 4-px-aligned windows on BOTH axes: our
 * strip flushes are full-width (x 0..411) and 4-line tall, so they always land
 * aligned. A PARTIAL redraw at an arbitrary rect — e.g. a single key lighting
 * up on press — does not, and the panel then flushes a misaligned window and
 * ghosts a mirrored slice off the right edge. Snap every invalidated area out
 * to the 4-px grid so partial redraws stay aligned like the strips do. */
static void ff_display_invalidate_align_cb(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_invalidated_area(e);
    a->x1 &= ~3;          /* round start down to x % 4 == 0 */
    a->y1 &= ~3;
    a->x2 |= 3;           /* round inclusive end up to == 3 (mod 4) */
    a->y2 |= 3;
    if (a->x2 > FF_LCD_H_RES - 1) { a->x2 = FF_LCD_H_RES - 1; }
    if (a->y2 > FF_LCD_V_RES - 1) { a->y2 = FF_LCD_V_RES - 1; }
}

/* =====================================================================
 * b2 — LVGL v9 via esp_lvgl_port, lv_display backed by the panel.
 * ===================================================================== */
lv_display_t *ff_display_lvgl_start(void)
{
    if (s_panel == NULL || s_panel_io == NULL) {
        ESP_LOGE(TAG, "lvgl_start called before panel_init");
        return NULL;
    }

    /* lvgl_port_init() calls lv_init() and spawns the LVGL task/timer —
     * do NOT call lv_init() elsewhere. */
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /* The default LVGL-task stack (7168 B) overflows rendering the Signals feed
     * — a scrollable list of message rows with per-row text is a deeper layout +
     * glyph-render than the Radar face, and it blew taskLVGL's stack (detected
     * at a context switch). Give it headroom. This stack is internal RAM
     * (task_stack_caps = MALLOC_CAP_INTERNAL), the same scarce pool as the DMA
     * strip buffers, so keep the bump modest; verified both configs still
     * allocate their LVGL buffers (demo 20-line, field 40-line). */
    port_cfg.task_stack = 12288;
    esp_err_t err = lvgl_port_init(&port_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: %s", esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "LVGL v9 port init (task started)");

    /* Partial FULL-WIDTH strip buffers in internal DMA RAM. A full-frame
     * PSRAM buffer with full_refresh flushes all ~331 KB in ONE
     * draw_bitmap, which fails at panel_io_spi_tx_color with ESP_ERR_NO_MEM
     * (the SPI/esp_lcd layer can't get enough internal DMA for a transfer
     * that large — the same wall b1's raw fill hit). Instead flush in
     * FF_LVGL_STRIP_LINES-row full-width strips: x is always 0..411 (the
     * SPD2010 x%4 alignment rule holds — 412 is a multiple of 4), the strip
     * height is a multiple of 4 too, and each transfer is a few tens of KB.
     * The two strip buffers fit internal DMA RAM (~240 KB free). */
#if CONFIG_FF_DEMO_MODE
    /* The demo build carries the seeded festpack + a fuller projection, leaving
     * less contiguous internal DMA RAM; 20-line strips (~16 KB/buffer) allocate
     * where the field build's 40-line (~32 KB) buffers would not. Field builds
     * keep 40 lines (fewer flushes, unchanged hardware-verified path). */
    enum { FF_LVGL_STRIP_LINES = 20 };
#else
    enum { FF_LVGL_STRIP_LINES = 40 };
#endif
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_panel_io,
        .panel_handle = s_panel,
        .buffer_size = (uint32_t)FF_LCD_H_RES * FF_LVGL_STRIP_LINES,
        .double_buffer = true,
        .hres = FF_LCD_H_RES,
        .vres = FF_LCD_V_RES,
        .monochrome = false,
        /* Rotation 0, no swap/mirror — the verified-correct orientation.
         * NOTE: at add_disp time esp_lvgl_port unconditionally calls
         * esp_lcd_panel_swap_xy(panel, false) for ROTATION_0, and the
         * SPD2010 panel driver logs `E spd2010: swap_xy is not supported by
         * this panel` for ANY swap_xy call. That one line is BENIGN — the
         * call is a no-op (false) and orientation is unaffected. The only
         * ways to suppress it (sw_rotate, or vendoring the panel driver)
         * would risk the hardware-verified strip-flush/orientation, so we
         * leave it per S15b FIX 3's "leave it if silencing risks orientation". */
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .swap_bytes = true,
            .full_refresh = false,
        },
    };
    s_lv_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_lv_disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return NULL;
    }
    lv_display_add_event_cb(s_lv_disp, ff_display_invalidate_align_cb,
                            LV_EVENT_INVALIDATE_AREA, NULL);
    ESP_LOGI(TAG, "lv_display added (%dx%d RGB565, %d-line full-width strips, internal DMA)",
             FF_LCD_H_RES, FF_LCD_V_RES, FF_LVGL_STRIP_LINES);
    return s_lv_disp;
}

/* =====================================================================
 * b3 — SPD2010 touch -> LVGL pointer indev (the ONLY input path).
 * ===================================================================== */
static void ff_touch_press_log_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    /* Post-calibration coords: this is what LVGL and the shell act on. When
     * uncalibrated (identity), it equals the raw controller report. */
    ESP_LOGI(TAG, "touch @ (%d, %d) [%s]", (int)p.x, (int)p.y,
             s_active_cal.valid ? "calibrated" : "raw, uncalibrated");
}

/* process_coordinates: the ONE seam every physical touch passes through
 * before esp_lvgl_port hands it to LVGL (esp_lcd_touch_get_coordinates
 * calls this after the controller read). Correcting here fixes gestures,
 * long-press, and buttons alike — no second input path. Identity when
 * uncalibrated, so raw passes through (still clamped to the panel). */
static void ff_touchcal_process_cb(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                                   uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    (void)tp;
    (void)strength;
    (void)max_point_num;
    if (x == NULL || y == NULL || point_num == NULL) {
        return;
    }
    for (uint8_t i = 0; i < *point_num; i++) {
        int sx, sy;
        ff_touchcal_apply(&s_active_cal, (int)x[i], (int)y[i], &sx, &sy);
        x[i] = (uint16_t)sx;
        y[i] = (uint16_t)sy;
    }
}

void ff_display_touch_set_cal(const ff_touchcal_t *c)
{
    if (c == NULL || !c->valid) {
        ff_touchcal_identity(&s_active_cal);
        ESP_LOGI(TAG, "touch cal: identity (uncalibrated — raw passes through)");
        return;
    }
    s_active_cal = *c;
    ESP_LOGI(TAG, "touch cal active: sx=%.4f*rawx%+.2f  sy=%.4f*rawy%+.2f", (double)c->ax,
             (double)c->bx, (double)c->ay, (double)c->by);
}

esp_err_t ff_display_touch_start(lv_display_t *disp)
{
    if (s_i2c_bus == NULL) {
        ESP_LOGE(TAG, "touch_start called before expander_init (no I2C bus / TP_RST)");
        return ESP_ERR_INVALID_STATE;
    }
    if (disp == NULL) {
        ESP_LOGE(TAG, "touch_start called with NULL display (run lvgl_start first)");
        return ESP_ERR_INVALID_ARG;
    }

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_SPD2010_CONFIG();
    tp_io_cfg.scl_speed_hz = FF_I2C_HZ;
    esp_err_t err = esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch panel_io_i2c failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = FF_LCD_H_RES,
        .y_max = FF_LCD_V_RES,
        .rst_gpio_num = -1,               /* TP_RST is on EXIO1, already released */
        /* POLLING, not INT (S15b touch-read fix). Waveshare's OWN
         * ESP-IDF-5.3.2 demo (ESP32-S3-Touch-LCD-1.46) never configures a
         * GPIO interrupt for the touch: its LVGL indev read_cb
         * (LVGL_Driver.c) POLLS Touch_Get_xy -> tp_read_data every cycle,
         * and EXAMPLE_PIN_NUM_TOUCH_INT (GPIO4) is only a #define it never
         * wires to gpio_isr. On this board the INT line never asserts on
         * touch, so INT-driven reads (int_gpio_num = GPIO4) starved
         * esp_lvgl_port's reader and no press ever reached LVGL. With
         * int_gpio_num = GPIO_NUM_NC, esp_lvgl_port's indev timer polls
         * esp_lcd_touch_read_data() every cycle — matching the demo — and
         * the vendored driver's read path now reports 0 points (not an
         * error) on an empty poll, so ESP_ERROR_CHECK never trips. */
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
        /* S15 slice d: correct every touch in-place here, the single seam
         * esp_lvgl_port reads through. Identity until a cal is installed. */
        .process_coordinates = ff_touchcal_process_cb,
    };
    err = esp_lcd_touch_new_i2c_spd2010(tp_io, &tp_cfg, &s_touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_touch_new_i2c_spd2010 failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "SPD2010 touch up (I2C 0x53, polling — INT/GPIO%d unused)", FF_PIN_TP_INT);

    /* The device's ONLY input path: this pointer indev drives LVGL
     * hit-testing against the screens, whose existing intent emits reach
     * the bound ff_shell_intent_sink — the exact seam the sim's synthetic
     * pointer indev (targets/sim/ctl_loop.c) drives. No second path. */
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = s_touch,
    };
    lv_indev_t *indev = lvgl_port_add_touch(&touch_cfg);
    if (indev == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_touch failed");
        return ESP_FAIL;
    }
    s_touch_indev = indev; /* remembered so app_main can poll finger-down state (defer-rebuild-mid-tap) */
    /* Log raw coords on every press so an uncalibrated tap still tells the
     * maintainer where the controller thinks the finger landed. */
    lv_indev_add_event_cb(indev, ff_touch_press_log_cb, LV_EVENT_PRESSED, NULL);
    ESP_LOGI(TAG, "touch indev added -> shell input seam (LVGL pointer)");
    return ESP_OK;
}

bool ff_display_touch_is_down(void)
{
    /* True while a finger is physically on the panel. app_main uses this to
     * DEFER a face teardown+rebuild until the finger lifts: a rebuild
     * (lv_obj_clean + ff_face_build) between a tap's press and release
     * destroys the very button being pressed, so its LV_EVENT_CLICKED never
     * fires — the "just highlights, won't open" report. No indev yet (touch
     * not started) reads as "not down", so the caller never blocks. */
    return s_touch_indev != NULL && lv_indev_get_state(s_touch_indev) == LV_INDEV_STATE_PRESSED;
}

/* =====================================================================
 * S15 slice d — crosshair capture calibration flow.
 *
 * Five targets (screen space): center first, then the four insets — all
 * comfortably inside the round glass, giving 3 distinct x and 3 distinct y
 * for a well-conditioned per-axis fit (docs/specs/S15d). One raw tap is
 * captured per target on a full press+release (debounce), then the pairs
 * feed ff_touchcal_solve. The capture runs with the active cal at identity
 * so the taps recorded are raw; the caller installs the solved transform.
 * ===================================================================== */
enum { FF_CAL_TARGET_COUNT = 5 };
static const int s_cal_tx[FF_CAL_TARGET_COUNT] = {206, 90, 322, 90, 322};
static const int s_cal_ty[FF_CAL_TARGET_COUNT] = {206, 90, 90, 322, 322};

static struct {
    volatile int captured; /* number of targets captured so far          */
    volatile bool done;    /* set true once all FF_CAL_TARGET_COUNT taken */
    ff_cal_point_t pts[FF_CAL_TARGET_COUNT];
    lv_obj_t *label;
    lv_obj_t *ring;
    lv_obj_t *bar_h;
    lv_obj_t *bar_v;
    lv_obj_t *dot;
} s_cal;

/* Move the crosshair to target `idx` and update the progress text. Runs
 * under the LVGL lock (from the setup path or the LVGL-task event cb). */
static void ff_cal_place_crosshair(int idx)
{
    int tx = s_cal_tx[idx];
    int ty = s_cal_ty[idx];
    lv_obj_set_pos(s_cal.ring, tx - 20, ty - 20);
    lv_obj_set_pos(s_cal.bar_h, tx - 20, ty - 1);
    lv_obj_set_pos(s_cal.bar_v, tx - 1, ty - 20);
    lv_obj_set_pos(s_cal.dot, tx - 3, ty - 3);
    lv_label_set_text_fmt(s_cal.label, "Tap the target\n%d / %d", idx + 1, FF_CAL_TARGET_COUNT);
}

/* One capture per target, on release (a deliberate, completed tap). */
static void ff_cal_release_cb(lv_event_t *e)
{
    if (s_cal.done) {
        return;
    }
    int idx = s_cal.captured;
    if (idx >= FF_CAL_TARGET_COUNT) {
        return;
    }
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    s_cal.pts[idx].raw_x = (int)p.x;
    s_cal.pts[idx].raw_y = (int)p.y;
    s_cal.pts[idx].screen_x = s_cal_tx[idx];
    s_cal.pts[idx].screen_y = s_cal_ty[idx];
    ESP_LOGI(TAG, "cal capture %d/%d: raw (%d,%d) -> target (%d,%d)", idx + 1, FF_CAL_TARGET_COUNT,
             (int)p.x, (int)p.y, s_cal_tx[idx], s_cal_ty[idx]);

    s_cal.captured = idx + 1;
    if (s_cal.captured >= FF_CAL_TARGET_COUNT) {
        lv_label_set_text(s_cal.label, "Calibrating...");
        s_cal.done = true; /* published last, after pts[] is written */
    } else {
        ff_cal_place_crosshair(s_cal.captured);
    }
}

static lv_obj_t *ff_cal_make_bar(lv_obj_t *parent, int w, int h, lv_color_t col)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, col, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE); /* clicks pass to root */
    return o;
}

esp_err_t ff_display_run_calibration(ff_touchcal_t *out_cal)
{
    if (out_cal == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ff_touchcal_identity(out_cal);

    if (s_lv_disp == NULL || s_touch == NULL) {
        ESP_LOGE(TAG, "run_calibration needs lvgl_start + touch_start first");
        return ESP_ERR_INVALID_STATE;
    }

    /* Capture RAW: the active transform must be identity while we record. */
    ff_touchcal_identity(&s_active_cal);
    memset(&s_cal, 0, sizeof(s_cal));

    const lv_color_t accent = lv_color_hex(0xFFC66B); /* Firefly amber */

    if (!ff_display_lock(1000)) {
        ESP_LOGE(TAG, "run_calibration: LVGL lock timeout");
        return ESP_ERR_TIMEOUT;
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);

    /* Full-screen black backdrop that catches every tap. */
    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    s_cal.label = lv_label_create(root);
    lv_obj_set_style_text_color(s_cal.label, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_cal.label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_cal.label, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_remove_flag(s_cal.label, LV_OBJ_FLAG_CLICKABLE);

    /* Crosshair: an open ring + a thin cross + a centre dot, all in accent
     * and all click-through so the tap always reaches `root`. */
    s_cal.ring = lv_obj_create(root);
    lv_obj_remove_style_all(s_cal.ring);
    lv_obj_set_size(s_cal.ring, 40, 40);
    lv_obj_set_style_radius(s_cal.ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_cal.ring, 3, 0);
    lv_obj_set_style_border_color(s_cal.ring, accent, 0);
    lv_obj_set_style_bg_opa(s_cal.ring, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_cal.ring, LV_OBJ_FLAG_CLICKABLE);

    s_cal.bar_h = ff_cal_make_bar(root, 40, 2, accent);
    s_cal.bar_v = ff_cal_make_bar(root, 2, 40, accent);
    s_cal.dot = ff_cal_make_bar(root, 6, 6, accent);
    lv_obj_set_style_radius(s_cal.dot, LV_RADIUS_CIRCLE, 0);

    lv_obj_add_event_cb(root, ff_cal_release_cb, LV_EVENT_RELEASED, NULL);
    ff_cal_place_crosshair(0);
    ff_display_unlock();

    ESP_LOGI(TAG, "S15d calibration started: tap each of %d crosshairs on the glass",
             FF_CAL_TARGET_COUNT);

    /* Block this task until the LVGL-task event cb has captured all five. */
    while (!s_cal.done) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    bool ok = ff_touchcal_solve(s_cal.pts, FF_CAL_TARGET_COUNT, out_cal);

    ESP_LOGI(TAG, "=== S15d touch calibration result ===");
    for (int i = 0; i < FF_CAL_TARGET_COUNT; i++) {
        ESP_LOGI(TAG, "  pair %d/%d: raw (%d,%d) -> screen (%d,%d)", i + 1, FF_CAL_TARGET_COUNT,
                 s_cal.pts[i].raw_x, s_cal.pts[i].raw_y, s_cal.pts[i].screen_x, s_cal.pts[i].screen_y);
    }
    if (ok) {
        ESP_LOGI(TAG, "  params: ax=%.6f bx=%.4f ay=%.6f by=%.4f", (double)out_cal->ax,
                 (double)out_cal->bx, (double)out_cal->ay, (double)out_cal->by);
        ESP_LOGI(TAG, "  transform: sx = %.6f*rawx %+.4f   sy = %.6f*rawy %+.4f", (double)out_cal->ax,
                 (double)out_cal->bx, (double)out_cal->ay, (double)out_cal->by);
    } else {
        ESP_LOGE(TAG, "  DEGENERATE capture (no x- or y-spread) — identity kept, no correction. "
                      "Re-run calibration and tap the distinct targets.");
    }
    ESP_LOGI(TAG, "=====================================");

    /* Tear the calibration screen down; the caller rebuilds the real face. */
    if (ff_display_lock(1000)) {
        lv_obj_clean(lv_screen_active());
        ff_display_unlock();
    }
    /* Drop the now-dangling object pointers (screen was cleaned). */
    memset(&s_cal, 0, sizeof(s_cal));

    return ESP_OK;
}

/* ---- LVGL task lock (esp_lvgl_port runs LVGL in its own task) --------- */
bool ff_display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void ff_display_unlock(void)
{
    lvgl_port_unlock();
}
