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

#include "driver/gpio.h"
#include "driver/i2c_master.h"
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

/* ---- Display QSPI pins (Display_SPD2010.h) --------------------------- */
#define FF_LCD_HOST SPI2_HOST
#define FF_PIN_LCD_SCK 40
#define FF_PIN_LCD_D0 46
#define FF_PIN_LCD_D1 45
#define FF_PIN_LCD_D2 42
#define FF_PIN_LCD_D3 41
#define FF_PIN_LCD_CS 21
#define FF_PIN_LCD_TE 18 /* tearing-effect; wired but unused this slice (see note) */
#define FF_PIN_LCD_BL 5  /* backlight, active-high (demo drives via LEDC; we drive GPIO) */
#define FF_LCD_PCLK_HZ (80 * 1000 * 1000)

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
static lv_display_t *s_lv_disp;

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
    /* Native orientation: the demo sets no mirror/swap/gap for this glass. */
    err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_disp_on_off failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "SPD2010 panel init done (%dx%d RGB565 QSPI)", FF_LCD_H_RES, FF_LCD_V_RES);

    /* Backlight on (GPIO5, active-high). The demo ramps it via LEDC PWM;
     * a plain push-pull high is enough for bring-up and provably ON. */
    const gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << FF_PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    err = gpio_config(&bl_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    gpio_set_level(FF_PIN_LCD_BL, 1);
    ESP_LOGI(TAG, "backlight ON (GPIO%d high)", FF_PIN_LCD_BL);

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
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    esp_err_t err = lvgl_port_init(&port_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: %s", esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "LVGL v9 port init (task started)");

    /* Full-frame double buffer in PSRAM. full_refresh keeps every flush
     * full-width (0..412), sidestepping the SPD2010 x%4 partial-area
     * alignment rule. swap_bytes handles the RGB565 big-endian wire order
     * (same swap b1 applies by hand). */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_panel_io,
        .panel_handle = s_panel,
        .buffer_size = (uint32_t)FF_LCD_H_RES * FF_LCD_V_RES,
        .double_buffer = true,
        .hres = FF_LCD_H_RES,
        .vres = FF_LCD_V_RES,
        .monochrome = false,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .swap_bytes = true,
            .full_refresh = true,
        },
    };
    s_lv_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_lv_disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return NULL;
    }
    ESP_LOGI(TAG, "lv_display added (%dx%d RGB565, PSRAM double buffer, full_refresh)",
             FF_LCD_H_RES, FF_LCD_V_RES);
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
    /* Honest raw coords: uncalibrated, logged as the controller reports
     * them (S15b: "don't fake positions; log raw coords and say so"). */
    ESP_LOGI(TAG, "touch @ (%d, %d) [raw, uncalibrated]", (int)p.x, (int)p.y);
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
        .rst_gpio_num = -1,             /* TP_RST is on EXIO1, already released */
        .int_gpio_num = FF_PIN_TP_INT,  /* INT on GPIO4 — the controller's ready line */
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    err = esp_lcd_touch_new_i2c_spd2010(tp_io, &tp_cfg, &s_touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_touch_new_i2c_spd2010 failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "SPD2010 touch up (I2C 0x53, INT=GPIO%d)", FF_PIN_TP_INT);

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
    /* Log raw coords on every press so an uncalibrated tap still tells the
     * maintainer where the controller thinks the finger landed. */
    lv_indev_add_event_cb(indev, ff_touch_press_log_cb, LV_EVENT_PRESSED, NULL);
    ESP_LOGI(TAG, "touch indev added -> shell input seam (LVGL pointer)");
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
