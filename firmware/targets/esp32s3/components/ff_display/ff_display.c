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

#include <math.h> /* sqrtf — the boot splash's ray-unit-vector precompute, ff_display_draw_boot_splash */
#include <string.h>

#include "ff_flare_mark.h" /* S26g — shared flare-mark geometry table; see that header's doc comment */
#include "ff_idle.h" /* S26 wake-only-touch amendment — ff_idle_touch_gate, consulted from the touch read path below */
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
#include "esp_timer.h" /* S26 slice (g) — esp_timer_get_time() for the boot-splash timing log */
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
#define FF_LCD_X_GAP 0 /* MUST stay 0 on this SPD2010: the pixel array is exactly 412 wide (a positive gap pushes the last columns off the array and the panel paints them WHITE — measured 2026-09-02). The glass sits ~5 px right of the pixel array; that is a THEME concern (FF_THEME_GLASS_*), not a panel window shift. See docs/hardware/glass-offset.md. */
#define FF_LCD_Y_GAP 0 /* see X_GAP */
_Static_assert(FF_LCD_X_GAP % 4 == 0 && FF_LCD_Y_GAP % 4 == 0,
              "panel gaps must be multiples of 4: the SPD2010 requires 4-px-aligned flush windows in panel coordinates");

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
 * flicker floor and below any audible-whine range. FF_BL_MIN_PCT/_MAX_PCT
 * (mirroring core's FF_BRIGHTNESS_MIN_PCT/_MAX_PCT, ff_settings.h) are now
 * PUBLIC (ff_display.h, S26 slice c) rather than local literals, so
 * app_main's DIM enact shares this exact constant instead of a second copy
 * that could drift from it — this device HAL still touches no core/
 * DOMAIN STATE (ff_display.h's updated header comment draws that line
 * precisely, as of S26g); the value is just no longer duplicated. The
 * floor is a DEFENSIVE second clamp: the
 * shell already clamps the setting to the same range, but the HAL never
 * trusts a caller to have done so — a 0% duty via ff_display_set_brightness
 * is a black, unrecoverable backlight, so that path can never program one.
 * The one legitimate true-zero path is ff_display_backlight_off (S26 slice
 * c's OFF enact), which bypasses this clamp entirely and is documented as
 * such in ff_display.h. */
#define FF_BL_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define FF_BL_LEDC_TIMER   LEDC_TIMER_0
#define FF_BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define FF_BL_LEDC_RES     LEDC_TIMER_10_BIT
#define FF_BL_LEDC_FREQ_HZ 5000

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

/* S26 wake-only-touch amendment (docs/specs/S26-device-lifecycle.md "(c)
 * Inactivity -> dim -> screen off", 2026-09-02): the shared idle FSM
 * (app_main.c's `s_idle`), wired in via ff_display_touch_set_idle so the
 * touch read path (ff_touch_gate_read_cb below) can consult
 * ff_idle_touch_gate before delivering a press to LVGL. NULL until wired
 * — the gate call fails open (delivers every press) in that case, same
 * as ff_idle_touch_gate's own NULL-safety convention. `s_touch_gate` is
 * THIS input source's own latch (ff_idle_touch_gate_t's own doc comment:
 * one instance per physical input — BOOT gets a separate instance in
 * app_main.c). `s_touch_orig_read_cb` is the vendored esp_lvgl_port
 * touch read callback lvgl_port_add_touch installs; ff_touch_gate_read_cb
 * wraps it rather than reimplementing touch reads (see that function's
 * own doc comment). `s_touch_raw_down` is the physical finger-down state,
 * refreshed on EVERY poll (press AND release) from the wrapped callback's
 * own result — the truthful signal ff_display_touch_is_down() now
 * returns, independent of what the gate decided to report to LVGL. */
static ff_idle_t *s_touch_idle;
static ff_idle_touch_gate_t s_touch_gate;
static lv_indev_read_cb_t s_touch_orig_read_cb;
static bool s_touch_raw_down;
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

/* Shared LEDC duty program + error handling, used by both
 * ff_display_set_brightness (percent, clamped) and
 * ff_display_backlight_off (a raw duty of 0, S26 slice c — bypasses that
 * clamp entirely, see this file's FF_BL_MIN_PCT comment above). Requires
 * s_bl_ready — callers check that themselves so each can log its own
 * "called before panel_init" context. */
static esp_err_t ff_display_program_duty(uint32_t duty, uint8_t logged_pct)
{
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
    const uint32_t full_duty = (1u << FF_BL_LEDC_RES) - 1u;
    ESP_LOGI(TAG, "backlight set to %u%% (duty %u/%u)", (unsigned)logged_pct, (unsigned)duty, (unsigned)full_duty);
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
    return ff_display_program_duty(duty, pct);
}

esp_err_t ff_display_backlight_off(void)
{
    if (!s_bl_ready) {
        ESP_LOGE(TAG, "backlight_off called before panel_init (backlight LEDC not up)");
        return ESP_ERR_INVALID_STATE;
    }
    /* The one true-zero path — deliberately bypasses ff_display_set_brightness's
     * FF_BL_MIN_PCT floor (see this file's top-of-block comment and
     * ff_display.h's doc comment on this function). */
    return ff_display_program_duty(0u, 0u);
}

esp_err_t ff_display_backlight_on(uint8_t pct)
{
    return ff_display_set_brightness(pct);
}

/* Active touch-calibration transform (S15 slice d). Identity until
 * ff_display_touch_set_cal installs a fit — so an uncalibrated device (and
 * the calibration capture itself) sees raw coords pass through. Read by
 * ff_touchcal_process_cb on every touch poll (LVGL port task); written by
 * ff_display_touch_set_cal. A torn read across the write is harmless — the
 * fields are independent floats and the next poll reads the settled value. */
static ff_touchcal_t s_active_cal = {.ax = 1.0f, .bx = 0.0f, .ay = 1.0f, .by = 0.0f, .valid = false};

/* Active screen-flip flag (format v8 amendment). Written by
 * ff_display_set_flip, read by ff_touchcal_process_cb on every touch poll
 * — same "torn read is harmless" contract as s_active_cal above (a bool
 * is written/read atomically on this target either way, and even a torn
 * read only ever produces one of the two valid values for one frame). */
static bool s_screen_flip = false;

/* Set only while ff_display_run_calibration's crosshair capture is live
 * (see that function). While true, ff_touchcal_process_cb captures TRUE
 * raw ticks — it still runs ff_touchcal_apply (identity, per "Capture
 * RAW" below) but SKIPS the screen_flip rotation step, and
 * ff_cal_release_cb records each target's screen_x/y already rotated to
 * match (`ff_touchcal_flip180` applied to the LOGICAL crosshair position
 * when screen_flip is on). This is not a cosmetic nicety: composing
 * "capture-time flip" with "apply-time flip" the same way live touch
 * does would fit an affine transform against a DIFFERENT, flip-
 * contaminated raw/target relationship than the one live touch actually
 * measures against later — see this file's own worked-through derivation
 * in the PR that introduced this flag for why a calibration solved while
 * naively flipping during capture drifts on live use, even for a puck
 * that never changes its screen_flip setting again after calibrating.
 * Capturing genuinely raw ticks against the PHYSICAL (flip-aware) target
 * position instead keeps the solved (ax,bx,ay,by) a pure, orientation-
 * independent characterization of the touch sensor's own error — exactly
 * the property that lets it "stay valid in both orientations" (no
 * re-calibration on a later flip toggle) regardless of which orientation
 * the puck happened to be in WHEN it was calibrated. */
static volatile bool s_cal_capturing = false;

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
 * format v8 amendment (maintainer ask, 2026-09-02) — SCREEN NORMAL|
 * FLIPPED: a HARDWARE 180-degree panel mirror, applied via MADCTL.
 *
 * Driver support, confirmed by reading the vendored espressif__esp_lcd_
 * spd2010 2.0.0~1 source (~/Library/Caches/Espressif/ComponentManager/
 * .../espressif__esp_lcd_spd2010_2.0.0~1_2b62e656/esp_lcd_spd2010.c,
 * lines 717-735 on this machine — re-confirm on another, per this
 * component's own line-number caveat already noted in
 * docs/hardware/glass-offset.md):
 *
 *     static esp_err_t panel_spd2010_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
 *     {
 *         ...
 *         if (mirror_x) { spd2010->madctl_val |= BIT(1); } else { ... &= ~BIT(1); }
 *         if (mirror_y) { spd2010->madctl_val |= BIT(0); } else { ... &= ~BIT(0); }
 *         ESP_RETURN_ON_ERROR(tx_param(spd2010, io, LCD_CMD_MADCTL, (uint8_t[]){ spd2010->madctl_val }, 1), ...);
 *         return ESP_OK;
 *     }
 *
 * i.e. `esp_lcd_panel_mirror` IS wired to a real, driver-implemented
 * MADCTL write on this panel — UNLIKE `panel_spd2010_swap_xy`, a few
 * lines below it in the same file, which is a stub
 * (`ESP_LOGE(TAG, "swap_xy is not supported by this panel"); return
 * ESP_ERR_NOT_SUPPORTED;`, already cited in ff_display_lvgl_start's own
 * comment on the benign swap_xy(false) log line esp_lvgl_port emits at
 * add_disp time). So `esp_lcd_panel_mirror(panel, flip, flip)` — both
 * axes together, the standard MADCTL trick for a full 180-degree
 * rotation — is a real, driver-backed call on THIS panel, not a silent
 * no-op the way swap_xy would be; no `lv_display_set_rotation` software
 * fallback is needed (see ff_display.h's doc comment on this function
 * for that fallback's cost, spelled out for the record even though it
 * isn't taken).
 *
 * 4-px window-alignment reasoning (docs/hardware/glass-offset.md's own
 * constraint, re-derived here because mirror touches orientation the
 * same way a bad gap once did — #152/#153): `panel_spd2010_draw_bitmap`
 * (the same vendored file) computes its CASET/RASET window ENTIRELY in
 * OUR coordinate space —
 *
 *     x_start += spd2010->x_gap;  x_end += spd2010->x_gap;
 *     y_start += spd2010->y_gap;  y_end += spd2010->y_gap;
 *     tx_param(..., LCD_CMD_CASET, ...);  tx_param(..., LCD_CMD_RASET, ...);
 *
 * — and NEVER reads `madctl_val` while building that window. The mirror
 * bits change how the panel's OWN internal GRAM address counter walks
 * the pixels it receives for a given CASET/RASET window (which physical
 * column framebuffer-column-0 lands on), not what window WE compute and
 * send. We never construct an `x' = 411 - x` window ourselves — there is
 * no software coordinate flip on the draw-bitmap path at all — so the
 * existing FF_LCD_X_GAP/_Y_GAP 4-px-aligned static assert and the
 * always-full-width-strip flush (FF_LVGL_STRIP_LINES, x always 0..411,
 * both divisible by 4) stay EXACTLY as aligned under mirror as without
 * it: mirror is orthogonal to gap/window math, not an interaction with
 * it. This is a *stronger* guarantee than "verified still 4-aligned by
 * reasoning through the arithmetic" — there is no new arithmetic to
 * misalign in the first place. What this file's reasoning CANNOT confirm
 * from source alone is whether the SPD2010 silicon's own mirrored-
 * addressing implementation has some other undocumented edge quirk (the
 * gap-tuning history in docs/hardware/glass-offset.md is a reminder this
 * particular controller has surprised this project before) — the sim
 * cannot catch that either way (it does not model MADCTL at all), so
 * on-glass verification of the mirrored render (boot splash upright,
 * Radar rim tint centred, no stray edge line) is the maintainer's, per
 * this repo's hardware-in-the-loop convention (S15b's own "Verification"
 * section).
 */
esp_err_t ff_display_set_flip(bool flip)
{
    if (s_panel == NULL) {
        ESP_LOGE(TAG, "set_flip called before panel_init — no panel handle");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t const err = esp_lcd_panel_mirror(s_panel, flip, flip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_mirror failed: %s", esp_err_to_name(err));
        return err;
    }
    /* Touch (ff_touchcal_process_cb below) reads this same flag: the
     * panel mirror and the touch-coordinate flip are ONE user-visible
     * decision (this call is the one seam both derive from), so they
     * must never be settable independently — there is no path here that
     * updates the display without also updating touch. */
    s_screen_flip = flip;
    ESP_LOGI(TAG, "panel mirror: %s", flip ? "FLIPPED (180 deg)" : "NORMAL");
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
 * S26 slice (g) — boot splash: the Firefly flare mark (the same 8-ray
 * burst + center dot scr_flare.c's takeover screen breathes,
 * core/include/ff_flare_mark.h) breathing once against the theme
 * background, drawn RAW (no LVGL) so it is the very first thing on
 * glass. Reuses the exact band-buffer / draw_bitmap / byte-swap shape
 * ff_display_draw_test_pattern established just above (small
 * INTERNAL-DMA band, 4-line-aligned, same ff_rgb565_swap helper) rather
 * than inventing a second draw path. See ff_display_draw_boot_splash's
 * doc comment (ff_display.h) for the rationale and constraints.
 *
 * flare_ray_t / flare_mark_pixel_hit below are this file's own raw
 * rasterizer for the mark's geometry — there is no lv_line, no lv_obj,
 * no anti-aliasing here, just a per-pixel capsule-vs-point test against
 * the SAME ray table scr_flare.c's flare_build_mark draws with LVGL
 * lines, so the two draw paths cannot silently drift into two different
 * shapes (S26g: "can the boot animation be the flare animation?").
 *
 * PERFORMANCE (independent review, PR #146): the first cut here stored
 * just the ray endpoint and tested every pixel against every ray with a
 * per-pixel float DIVIDE (t = dot / len_sq) and no early-out, over the
 * FULL 412px row width. Disassembly showed that divide wasn't inlined
 * and ran ~2.97M times across the splash's 10 fade steps — ~1.25 s of
 * ADDED compute on top of the existing FF_SPLASH_STEP_MS/HOLD_MS delays,
 * roughly doubling the maintainer-approved ~1.1 s total. Fixed three
 * ways: (1) below — each ray now precomputes a unit direction + length
 * (once, outside the per-step loop), so the per-pixel projection is a
 * dot product against a unit vector (t = px*ux + py*uy, clamped to
 * [0,len]) — NO division at all; (2) each ray also precomputes its own
 * axis-aligned bounding box (its own segment extent, expanded by the
 * stroke half-width), so a pixel outside a ray's box skips that ray
 * with four cheap comparisons instead of paying the full
 * projection+distance math — most (pixel, ray) pairs in the strip fall
 * outside the ray's own thin box; (3) see
 * ff_display_draw_boot_splash's Pass 2 comment below — only the mark's
 * own horizontal band of columns is rasterized at all now, not the
 * full 412px row. Also added: the accumulated-raster-time log this
 * review asked for, kept separate from the existing total-elapsed log
 * so the maintainer can read compute-vs-delay apart on glass. */
typedef struct {
    float ux, uy;  /* unit direction of the ray, pointing away from center */
    float len;     /* ray length, pixels (== |(ex,ey)|) — the projection clamp bound */
    float min_x, max_x, min_y, max_y; /* AABB of the STROKE (segment extent +/- half_w), mark-center-relative */
} flare_ray_t;

/* True if point (px,py) — pixel offset from the mark's center — falls
 * inside the filled center dot (radius `dot_r_sq`, already squared) or
 * within `half_w_sq` (half the stroke width, squared) of the nearest
 * point on any of the 8 ray segments in `rays`. The segment test is the
 * standard point-to-segment distance, but projected via a precomputed
 * UNIT direction (t = px*ux + py*uy, clamped to [0, len]) rather than
 * dividing by the segment's length-squared per pixel — see the struct's
 * doc comment above. Clamping t is what gives the ray a ROUNDED cap at
 * both ends for free, matching `lv_obj_set_style_line_rounded(line,
 * true, 0)` in scr_flare.c without needing LVGL's rasterizer here.
 * `static inline`: this is the hot loop (called once per strip pixel,
 * up to 8 times each) and the checked-in device build is -Og — the
 * algorithmic fixes above (no divide, AABB reject, narrower strip) are
 * what make this cheap, not reliance on -O2-grade auto-inlining, but
 * `inline` still gives the compiler the option at this call count. */
static inline bool flare_mark_pixel_hit(float px, float py, flare_ray_t const rays[FF_FLARE_MARK_N_RAYS],
                                         float dot_r_sq, float half_w_sq)
{
    if (px * px + py * py <= dot_r_sq) {
        return true;
    }
    for (int i = 0; i < FF_FLARE_MARK_N_RAYS; i++) {
        flare_ray_t const *r = &rays[i];
        /* AABB reject FIRST: four cheap comparisons that skip the
         * projection math entirely for a pixel outside this ray's own
         * stroke extent — true for the large majority of (pixel, ray)
         * pairs in the strip (the 8 boxes cover only a fraction of the
         * strip's area between them — see the struct doc comment). */
        if (px < r->min_x || px > r->max_x || py < r->min_y || py > r->max_y) {
            continue;
        }
        float t = px * r->ux + py * r->uy; /* projection length onto the ray direction, no divide */
        if (t < 0.0f) t = 0.0f;
        if (t > r->len) t = r->len;
        float const nx = px - t * r->ux;
        float const ny = py - t * r->uy;
        if (nx * nx + ny * ny <= half_w_sq) {
            return true;
        }
    }
    return false;
}

esp_err_t ff_display_draw_boot_splash(void)
{
    if (s_panel == NULL) {
        ESP_LOGE(TAG, "draw_boot_splash called before panel_init");
        return ESP_ERR_INVALID_STATE;
    }

    int64_t const t_start_us = esp_timer_get_time();

    enum { BAND_LINES = 4 };
    const size_t band_px = (size_t)FF_LCD_H_RES * BAND_LINES;
    uint16_t *band = heap_caps_malloc(band_px * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (band == NULL) {
        ESP_LOGE(TAG, "boot splash: OOM allocating %u byte band buffer", (unsigned)(band_px * 2));
        return ESP_ERR_NO_MEM;
    }

    /* Theme background (#0B0B10 -> RGB565 0x0842) — see
     * FF_THEME_COLOR_BG, app/theme/ff_theme.h. Hardcoded rather than
     * included: ff_theme.h is an app/ header that pulls in lvgl.h for its
     * color macros, and this splash draws before LVGL exists — a
     * different, still-live reason than core/ (ff_display.h's header
     * comment now draws that line at "domain state", not "any header
     * at all" — see S26g's ff_flare_mark.h include just above, which IS
     * a core/ header this file now uses). draw_test_pattern already
     * establishes the same "value transcribed in a comment" convention
     * for its own amber fill above. The amber accent itself (#FFC66B) is
     * not a separate constant here — the per-step blend below reaches it
     * exactly at its t=255 step (amber_r/g/b). */
    const uint16_t bg = ff_rgb565_swap(0x0842);

    /* ---- Pass 1: wipe the whole panel to the theme background ----
     * ff_display_panel_init's contract only promises "blank", not a
     * known colour — this explicitly establishes the canvas before the
     * mark is drawn on it. The FIRST pixel this splash puts on glass
     * (AC1) is logged at the first band of this pass. */
    for (size_t i = 0; i < band_px; i++) band[i] = bg;
    bool logged_first_pixel = false;
    for (int y = 0; y < FF_LCD_V_RES; y += BAND_LINES) {
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, FF_LCD_H_RES, y + BAND_LINES, band);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "boot splash: bg band y=%d draw_bitmap failed: %s", y, esp_err_to_name(err));
            heap_caps_free(band);
            return err;
        }
        if (!logged_first_pixel) {
            logged_first_pixel = true;
            ESP_LOGI(TAG, "S26g AC1: first splash pixel at t=%lld us", (long long)esp_timer_get_time());
        }
    }

    /* ---- Pass 2: breathe the flare mark ----
     * Centered on the panel (206,206) — unlike the takeover screen's
     * FLARE_MARK_CY offset above its headline/buttons, this splash has
     * no other content to make room for. Ray geometry comes from
     * ff_flare_mark_ray_offset (ff_flare_mark.h), computed once here
     * (not per fade step — the SHAPE doesn't change, only the blend
     * factor does) into a stack-local table: no static/.bss storage, the
     * same "transient, freed on every path" contract this splash's band
     * buffer already keeps.
     *
     * Only the square region the mark's rays can possibly reach — BOTH
     * rows and columns, see the strip_top/strip_left computation below —
     * is redrawn per step; pass 1 already painted everything outside it.
     *
     * There is no real alpha compositing in a raw panel write, so
     * "opacity" is faked exactly as the previous dot version did:
     * linearly blending each 8-bit channel between bg and amber per
     * step — a symmetric ramp up then down (breathe, not a hold),
     * matching the flare mark's own ease-style pulse in spirit
     * (scr_flare.c's flare_anim_set_opa_cb) without needing LVGL's
     * animator here. Only the per-pixel SHAPE test changed (mark vs.
     * circle); the ramp itself (kFadeSteps, FF_SPLASH_STEP_MS,
     * FF_SPLASH_HOLD_MS below) is untouched. */
    const int cx = FF_LCD_H_RES / 2;
    const int cy = FF_LCD_V_RES / 2;

    float const half_w = FF_FLARE_MARK_LINE_W_PX / 2.0f;
    float const half_w_sq = half_w * half_w;
    float const dot_r_sq = FF_FLARE_MARK_CENTER_R_PX * FF_FLARE_MARK_CENTER_R_PX;

    /* Ray table: unit direction + length (the no-divide projection basis)
     * plus each ray's own stroke AABB (the early-reject bound) — both
     * PERFORMANCE fixes from the struct's doc comment above, computed
     * once here per call, not per pixel or per fade step. sqrtf x8 is
     * negligible next to what it replaces. */
    flare_ray_t rays[FF_FLARE_MARK_N_RAYS];
    for (int i = 0; i < FF_FLARE_MARK_N_RAYS; i++) {
        float dx, dy;
        ff_flare_mark_ray_offset(i, FF_FLARE_MARK_MAX_LEN_PX, &dx, &dy);
        float const len = sqrtf(dx * dx + dy * dy);
        rays[i].len = len;
        rays[i].ux = (len > 0.0f) ? (dx / len) : 0.0f;
        rays[i].uy = (len > 0.0f) ? (dy / len) : 0.0f;
        /* AABB of the segment from (0,0) to (dx,dy), expanded by the
         * stroke half-width on every side. */
        float const seg_min_x = (dx < 0.0f) ? dx : 0.0f;
        float const seg_max_x = (dx > 0.0f) ? dx : 0.0f;
        float const seg_min_y = (dy < 0.0f) ? dy : 0.0f;
        float const seg_max_y = (dy > 0.0f) ? dy : 0.0f;
        rays[i].min_x = seg_min_x - half_w;
        rays[i].max_x = seg_max_x + half_w;
        rays[i].min_y = seg_min_y - half_w;
        rays[i].max_y = seg_max_y + half_w;
    }

    /* Both the row STRIP (y) and, as of this review's FAIL 1, the column
     * BAND (x) the mark's rays can possibly reach are bounded the same
     * way: every ray's |dx| and |dy| is <= FF_FLARE_MARK_MAX_LEN_PX by
     * construction (sin/cos are each <= 1), so a symmetric square of
     * that reach (plus stroke half-width, plus a 1px rasterization
     * safety margin — there is no anti-aliasing here) around the mark's
     * center is a safe superset of the true bounding box on BOTH axes,
     * without computing it exactly. Restricting x too (not just y) is
     * the third performance fix: pass 1 already painted the background
     * everywhere, so a full 412px row was always wasted work outside
     * this ~90px band — both the per-pixel rasterization cost AND the
     * SPI transfer per draw_bitmap call shrink with it. 4-px aligned on
     * both axes, matching the SPD2010's documented GRAM window rule
     * (see ff_display_invalidate_align_cb's comment below for the same
     * rule applied to LVGL partial redraws). */
    int const mark_reach = (int)(FF_FLARE_MARK_MAX_LEN_PX + half_w + 1.0f); /* +1: rasterization safety margin */
    int strip_top = cy - mark_reach;
    int strip_bottom = cy + mark_reach;
    strip_top -= strip_top % BAND_LINES;                                    /* 4-px align, round down */
    strip_bottom += (BAND_LINES - (strip_bottom % BAND_LINES)) % BAND_LINES; /* round up */
    if (strip_top < 0) strip_top = 0;
    if (strip_bottom > FF_LCD_V_RES) strip_bottom = FF_LCD_V_RES;

    int strip_left = cx - mark_reach;
    int strip_right = cx + mark_reach;
    strip_left -= strip_left % BAND_LINES;                                  /* 4-px align, round down */
    strip_right += (BAND_LINES - (strip_right % BAND_LINES)) % BAND_LINES;  /* round up */
    if (strip_left < 0) strip_left = 0;
    if (strip_right > FF_LCD_H_RES) strip_right = FF_LCD_H_RES;
    int const strip_w = strip_right - strip_left; /* <= FF_LCD_H_RES, so `band`'s existing capacity always fits it */

    /* 8-bit channel components (pre-swap, pre-RGB565-pack) for the blend. */
    const int bg_r = 0x0B, bg_g = 0x0B, bg_b = 0x10;
    const int amber_r = 0xFF, amber_g = 0xC6, amber_b = 0x6B;

    /* Blend factor (0..255, toward amber) per step: a symmetric triangle
     * ramp, 5 steps up to full amber, a HOLD at full amber, then 5 back
     * down to bg. The first cut had no hold — the mark peaked for a single
     * 35 ms step and read as a blink on glass (maintainer, 2026-09-01:
     * "really quick"). Timing: (10 * FF_SPLASH_STEP_MS) + FF_SPLASH_HOLD_MS
     * + draw ≈ 1.1 s total — the spec's budget was raised from ≤ 1 s to
     * ~1 s (S26g) on that feedback. Tune the two constants, not the ramp. */
#define FF_SPLASH_STEP_MS 35
#define FF_SPLASH_HOLD_MS 600 /* dwell at full amber (the t == 255 step) */
    static const int kFadeSteps[] = {51, 102, 153, 204, 255, 204, 153, 102, 51, 0};
    enum { N_STEPS = sizeof(kFadeSteps) / sizeof(kFadeSteps[0]) };

    /* Accumulated RASTER compute time only (the flare_mark_pixel_hit
     * double loop below) — separate from the total elapsed time logged
     * at the end, and excluding both the SPI draw_bitmap calls and the
     * vTaskDelay ramp, so the maintainer can read compute-vs-delay apart
     * on glass (independent review, PR #146 FAIL 1). Before the fixes in
     * this same review round, this number would have read ~1.1-1.3s (a
     * non-inlined per-pixel float divide, no early-out, over the full
     * 412px row); after them it should be a few ms. */
    int64_t raster_us = 0;

    for (int s = 0; s < N_STEPS; s++) {
        int const t = kFadeSteps[s];
        int const r = bg_r + ((amber_r - bg_r) * t) / 255;
        int const g = bg_g + ((amber_g - bg_g) * t) / 255;
        int const b = bg_b + ((amber_b - bg_b) * t) / 255;
        uint16_t const natural = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        uint16_t const blended = ff_rgb565_swap(natural);

        for (int y = strip_top; y < strip_bottom; y += BAND_LINES) {
            int64_t const raster_start_us = esp_timer_get_time();
            for (int ly = 0; ly < BAND_LINES; ly++) {
                int const py = y + ly;
                float const fpy = (float)(py - cy);
                uint16_t *row = band + (size_t)ly * strip_w;
                for (int x = strip_left; x < strip_right; x++) {
                    float const fpx = (float)(x - cx);
                    row[x - strip_left] = flare_mark_pixel_hit(fpx, fpy, rays, dot_r_sq, half_w_sq) ? blended : bg;
                }
            }
            raster_us += esp_timer_get_time() - raster_start_us;

            esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, strip_left, y, strip_right, y + BAND_LINES, band);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "boot splash: mark band y=%d draw_bitmap failed: %s", y, esp_err_to_name(err));
                heap_caps_free(band);
                return err;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(FF_SPLASH_STEP_MS));
        if (t == 255) {
            vTaskDelay(pdMS_TO_TICKS(FF_SPLASH_HOLD_MS)); /* the hold — see kFadeSteps' comment */
        }
    }

    heap_caps_free(band);
    int64_t const elapsed_us = esp_timer_get_time() - t_start_us;
    ESP_LOGI(TAG, "S26g: boot splash complete, %lld ms total (%lld us raster compute, PR #146 FAIL 1 fix), "
                  "8-ray flare mark, max_len=%dpx, %dx%dpx strip, %d fade steps",
             (long long)(elapsed_us / 1000), (long long)raster_us, (int)FF_FLARE_MARK_MAX_LEN_PX, strip_w,
             (strip_bottom - strip_top), (int)N_STEPS);
    return ESP_OK;
}

#if CONFIG_FF_GLASS_RULER
/* =====================================================================
 * Device-only diagnostic (CONFIG_FF_GLASS_RULER, default OFF) — the
 * "glass ruler" boot pattern. Measures the bezel/pixel-array offset
 * (docs/hardware/glass-offset.md) by eye: on the current build a
 * software-centered ring renders visibly off-center on the glass (the
 * left arc fully hidden under the bezel, a mirrored sliver reappearing at
 * the far right — a GRAM column-wrap, not a simple crop), so this pattern
 * gives the maintainer something to COUNT rather than eyeball a vague
 * "a few px" shift. Drawn RAW via esp_lcd_panel_draw_bitmap, the exact
 * same band-buffer/byte-swap shape ff_display_draw_test_pattern and
 * ff_display_draw_boot_splash above already establish — no new draw path.
 *
 * Compiled out entirely when CONFIG_FF_GLASS_RULER is off (the default):
 * this whole block, so a field/demo build is byte-identical either way.
 * ===================================================================== */

/* Pre-swap RGB565 for the theme colors this pattern uses (see
 * ff_rgb565_swap above and ff_display_draw_boot_splash's comment on why
 * these are transcribed rather than included — ff_theme.h pulls in LVGL,
 * and this draws before LVGL exists). Computed as
 * ((r>>3)<<11)|((g>>2)<<5)|(b>>3) from the RGB888 hex named alongside
 * each — verified with a throwaway script, not hand-arithmetic, given a
 * wrong-but-plausible color here would be exactly the kind of "measuring
 * with a broken ruler" mistake this diagnostic exists to avoid. */
#define FF_RULER_INK   0xF77C /* FF_THEME_COLOR_INK   0xF2EFE6 */
#define FF_RULER_AMBER 0xFE2D /* FF_THEME_COLOR_AMBER 0xFFC66B — same value ff_display_draw_test_pattern's "amber" already uses */
#define FF_RULER_MUTED 0x8C52 /* FF_THEME_COLOR_MUTED 0x8B8A97 */
#define FF_RULER_PINK  0xFAF5 /* FF_THEME_CREW_PINK   0xFF5CA8 */
#define FF_RULER_TEAL  0x4ED8 /* FF_THEME_CREW_TEAL   0x4FD8C4 */
#define FF_RULER_BG    0x0842 /* FF_THEME_COLOR_BG    0x0B0B10 — same value ff_display_draw_boot_splash's "bg" already uses */

/* Ruler tick geometry, shared by all four rulers via `d` = distance
 * inward from the panel's own edge (0, 2, 4, ... 40 — 21 positions). A
 * multiple of 10 is a MAJOR tick: 12px long, 2px wide (vs. the minor
 * ticks' 1px width) so every 10-px mark is unmistakable by eye without
 * needing numerals (the brief's "hard to draw raw" constraint). Non-major
 * ticks alternate short(4px)/long(8px) by (d/2)%2 — purely a visual
 * cadence so consecutive 2px ticks don't blur into one blob; only the
 * majors carry measurement meaning. */
static inline int ff_ruler_tick_len(int d) {
    if (d % 10 == 0) return 12;
    return ((d / 2) % 2 == 0) ? 4 : 8;
}
static inline int ff_ruler_tick_w(int d) { return (d % 10 == 0) ? 2 : 1; }

/* Per-pixel color for the glass-ruler pattern at (x,y). `row_x_left` /
 * `row_x_right` and `col_y_top` / `col_y_bot` are the r=205 outer
 * circle's precomputed intersections for THIS row and THIS column (see
 * ff_display_draw_glass_ruler's comment on why — no per-pixel sqrt over
 * ~170k pixels).
 *
 * Priority (first match wins), high to low:
 *  1. the four 45-degree corner squares
 *  2. the LEFT wrap-test stripe (x<12) — the PRIMARY dx read
 *  3. the TOP wrap-test stripe (y<12) — the PRIMARY dy-wrap read
 *  4. the outer r=205 circle
 *  5-8. the four rulers (LEFT=uniform pink; RIGHT/TOP/BOTTOM=ink+amber)
 *  9. the plain crosshair through (cx,cy) everywhere else
 *  10. theme background
 * Priority matters where regions overlap by construction (e.g. the LEFT
 * ruler's baseline runs through the LEFT stripe's x<12 columns too — the
 * stripe wins there, since it's the primary read this update introduced;
 * the ruler's own ticks from x=12 on are still a secondary cross-check). */
static uint16_t ff_ruler_classify(int x, int y, int row_x_left, int row_x_right,
                                   int col_y_top, int col_y_bot)
{
    const int cx = FF_LCD_H_RES / 2; /* 206 */
    const int cy = FF_LCD_V_RES / 2; /* 206 */

    /* 1. Four 6px filled corner squares at the r=205 circle's own
     * 45-degree points: (cx +/- 145, cy +/- 145), where 145 =
     * round(205 * cos 45deg) = round(205 * 0.70710678). A diagonal
     * misalignment (rotation/scale, not a pure translation) shows up here
     * even if the axis rulers alone wouldn't catch it. */
    {
        const int off = 145;
        const int csx[2] = {cx - off, cx + off};
        const int csy[2] = {cy - off, cy + off};
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 2; j++) {
                int const ax = csx[i] - 3;
                int const ay = csy[j] - 3;
                if (x >= ax && x < ax + 6 && y >= ay && y < ay + 6) {
                    return FF_RULER_AMBER;
                }
            }
        }
    }

    /* 2. LEFT wrap-test stripe: columns x=0..11, full height, alternating
     * 1px ink/pink verticals. Whatever wraps around to the panel's RIGHT
     * edge under the current (uncorrected) GRAM addressing counts BY EYE
     * as "N striped columns visible at the right edge" = dx directly —
     * see docs/hardware/glass-offset.md. Highest priority below the
     * corners so the count is never occluded by anything else drawn. */
    if (x < 12) {
        return (x % 2 == 0) ? FF_RULER_INK : FF_RULER_PINK;
    }

    /* 3. TOP wrap-test stripe: rows y=0..11 (x already >= 12, claimed by
     * #2 above), alternating 1px ink/teal horizontals — the same wrap
     * check on the Y axis. A different color pair than the left stripe so
     * the two blocks, and which axis wrapped, are never ambiguous. */
    if (y < 12) {
        return (y % 2 == 0) ? FF_RULER_INK : FF_RULER_TEAL;
    }

    /* 4. Outer circle, r=205 from (cx,cy) — the framebuffer's outermost
     * circle at this center (206-205=1, i.e. it comes within 1px of the
     * x=0/y=0 edges and touches x=411/y=411 exactly). 1px ring, muted. */
    if (x == row_x_left || x == row_x_right || y == col_y_top || y == col_y_bot) {
        return FF_RULER_MUTED;
    }

    /* 5. LEFT ruler: baseline row cy, x=0..40. Entirely crew-pink — both
     * the baseline and every tick — a single color deliberately distinct
     * from the ink/amber convention the other three rulers keep, so this
     * one reads as "the ruler on the side that's wrapping" at a glance.
     * Ticks are vertical marks (perpendicular to the horizontal baseline)
     * centered on row cy, growing toward the panel center as x increases
     * from the true edge (x=0). Kept as a cross-check against the stripe
     * read above, not the primary measurement anymore. */
    if (x <= 40) {
        if (y == cy) {
            return FF_RULER_PINK;
        }
        for (int d = 0; d <= 40; d += 2) {
            int const w = ff_ruler_tick_w(d);
            if (x < d || x >= d + w) continue;
            int const half = ff_ruler_tick_len(d) / 2;
            if (y >= cy - half && y < cy + half) {
                return FF_RULER_PINK;
            }
        }
    }

    /* 6. RIGHT ruler: baseline row cy, x=371..411, ink + amber major
     * ticks (unchanged convention) — the dx cross-check, and the place a
     * maintainer confirms the mirrored/wrapped arc the bezel photo showed
     * lines up with what the left stripe predicts. */
    if (x >= 371) {
        if (y == cy) {
            return FF_RULER_INK;
        }
        for (int d = 0; d <= 40; d += 2) {
            int const w = ff_ruler_tick_w(d);
            int const x2 = 411 - d;
            int const x1 = x2 - (w - 1);
            if (x < x1 || x > x2) continue;
            int const half = ff_ruler_tick_len(d) / 2;
            if (y >= cy - half && y < cy + half) {
                return (d % 10 == 0) ? FF_RULER_AMBER : FF_RULER_INK;
            }
        }
    }

    /* 7. TOP ruler: baseline col cx, y=0..40 (y<12 already claimed by the
     * top stripe, #3), ink + amber major ticks — the dy cross-check. */
    if (y <= 40) {
        if (x == cx) {
            return FF_RULER_INK;
        }
        for (int d = 0; d <= 40; d += 2) {
            int const w = ff_ruler_tick_w(d);
            if (y < d || y >= d + w) continue;
            int const half = ff_ruler_tick_len(d) / 2;
            if (x >= cx - half && x < cx + half) {
                return (d % 10 == 0) ? FF_RULER_AMBER : FF_RULER_INK;
            }
        }
    }

    /* 8. BOTTOM ruler: baseline col cx, y=371..411, ink + amber major
     * ticks — confirms nothing wraps on this edge either (no stripe block
     * here; a clean plain ruler is itself the "nothing wrapped" signal). */
    if (y >= 371) {
        if (x == cx) {
            return FF_RULER_INK;
        }
        for (int d = 0; d <= 40; d += 2) {
            int const w = ff_ruler_tick_w(d);
            int const y2 = 411 - d;
            int const y1 = y2 - (w - 1);
            if (y < y1 || y > y2) continue;
            int const half = ff_ruler_tick_len(d) / 2;
            if (x >= cx - half && x < cx + half) {
                return (d % 10 == 0) ? FF_RULER_AMBER : FF_RULER_INK;
            }
        }
    }

    /* 9. Plain crosshair through pixel center (cx,cy), everywhere the
     * rulers above don't already claim (the long straight middle span). */
    if (x == cx || y == cy) {
        return FF_RULER_MUTED;
    }

    /* 10. Theme background, everywhere else. */
    return FF_RULER_BG;
}

esp_err_t ff_display_draw_glass_ruler(void)
{
    if (s_panel == NULL) {
        ESP_LOGE(TAG, "draw_glass_ruler called before panel_init");
        return ESP_ERR_INVALID_STATE;
    }

    const int cx = FF_LCD_H_RES / 2; /* 206 */
    const int cy = FF_LCD_V_RES / 2; /* 206 */
    enum { RULER_R = 205 };

    /* Outer-circle hit test, gap-free WITHOUT a per-pixel sqrt: precompute
     * the r=205 ring's row-wise (x at each y) AND column-wise (y at each
     * x) intersections ONCE — 412+412 sqrtf calls total, not one per each
     * of the ~170k pixels below. The union of a row-complete and a
     * column-complete sampling of a circle has no gaps at any angle (each
     * axis's own sampling is sparsest exactly where the OTHER axis's is
     * densest) — same "no float cost in the per-pixel hot loop"
     * discipline PR #146 established for the boot splash's ray
     * rasterizer above (ff_display_draw_boot_splash's doc comment).
     * Heap, not stack: ~1.6KB apiece, freed before return, matching this
     * file's "transient, never a static/.bss allocation" convention for
     * these raw-draw helpers. */
    int *col_y_top = heap_caps_malloc((size_t)FF_LCD_H_RES * sizeof(int), MALLOC_CAP_INTERNAL);
    int *col_y_bot = heap_caps_malloc((size_t)FF_LCD_H_RES * sizeof(int), MALLOC_CAP_INTERNAL);
    if (col_y_top == NULL || col_y_bot == NULL) {
        ESP_LOGE(TAG, "glass ruler: OOM allocating circle column tables");
        heap_caps_free(col_y_top);
        heap_caps_free(col_y_bot);
        return ESP_ERR_NO_MEM;
    }
    for (int x = 0; x < FF_LCD_H_RES; x++) {
        int const ddx = x - cx;
        if (ddx * ddx > RULER_R * RULER_R) {
            col_y_top[x] = -1;
            col_y_bot[x] = -1;
            continue;
        }
        int const dy = (int)(sqrtf((float)(RULER_R * RULER_R - ddx * ddx)) + 0.5f);
        col_y_top[x] = cy - dy;
        col_y_bot[x] = cy + dy;
    }

    enum { BAND_LINES = 4 };
    const size_t band_px = (size_t)FF_LCD_H_RES * BAND_LINES;
    uint16_t *band = heap_caps_malloc(band_px * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (band == NULL) {
        ESP_LOGE(TAG, "glass ruler: OOM allocating %u byte band buffer", (unsigned)(band_px * 2));
        heap_caps_free(col_y_top);
        heap_caps_free(col_y_bot);
        return ESP_ERR_NO_MEM;
    }

    for (int y0 = 0; y0 < FF_LCD_V_RES; y0 += BAND_LINES) {
        for (int ly = 0; ly < BAND_LINES; ly++) {
            int const y = y0 + ly;
            int row_x_left = -1, row_x_right = -1;
            int const ddy = y - cy;
            if (ddy * ddy <= RULER_R * RULER_R) {
                int const dx = (int)(sqrtf((float)(RULER_R * RULER_R - ddy * ddy)) + 0.5f);
                row_x_left = cx - dx;
                row_x_right = cx + dx;
            }
            uint16_t *row = band + (size_t)ly * FF_LCD_H_RES;
            for (int x = 0; x < FF_LCD_H_RES; x++) {
                uint16_t const raw = ff_ruler_classify(x, y, row_x_left, row_x_right,
                                                        col_y_top[x], col_y_bot[x]);
                row[x] = ff_rgb565_swap(raw);
            }
        }
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y0, FF_LCD_H_RES, y0 + BAND_LINES, band);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "glass ruler: band y=%d draw_bitmap failed: %s", y0, esp_err_to_name(err));
            heap_caps_free(band);
            heap_caps_free(col_y_top);
            heap_caps_free(col_y_bot);
            return err;
        }
    }

    heap_caps_free(band);
    heap_caps_free(col_y_top);
    heap_caps_free(col_y_bot);
    ESP_LOGI(TAG, "glass ruler drawn: crosshair + r=%d ring + 4 rulers (2px ticks to 40px) "
                  "+ 45deg corner squares + left/top dx/dy wrap-probe stripes — see "
                  "docs/hardware/glass-offset.md to read it",
             (int)RULER_R);
    return ESP_OK;
}
#endif /* CONFIG_FF_GLASS_RULER */

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
 * uncalibrated, so raw passes through (still clamped to the panel).
 *
 * format v8 amendment: the screen-flip 180-degree rotation is applied
 * AFTER calibration, as its own separate step — never folded into
 * `s_active_cal` itself. The calibration fit corrects the panel's OWN
 * raw-to-screen error (a per-unit hardware quirk, measured against
 * un-mirrored controller output); the case orientation is a SEPARATE,
 * independently-toggled fact that doesn't change what the controller
 * reports. Keeping them as two composed steps means a previously-solved
 * calibration stays valid in EITHER orientation — flipping the case
 * never demands a re-calibration, and re-running Calibrate Touch while
 * flipped still fits against the (already-flipped) screen-space the
 * crosshair itself is drawn in, so it composes correctly either way. */
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
        /* Skip the rotation while a calibration crosshair capture is in
         * progress (s_cal_capturing) — see that flag's own doc comment
         * for why: it must record TRUE raw-ticks-through-apply() only, so
         * the solved transform stays a pure, orientation-independent
         * sensor characterization that composes correctly with THIS same
         * flip step on every later live touch, in either orientation. */
        if (s_screen_flip && !s_cal_capturing) {
            ff_touchcal_flip180(sx, sy, FF_LCD_H_RES, FF_LCD_V_RES, &sx, &sy);
        }
        x[i] = (uint16_t)sx;
        y[i] = (uint16_t)sy;
    }
}

void ff_display_touch_set_idle(ff_idle_t *idle)
{
    s_touch_idle = idle;
    /* Reset the latch on every (re-)wire — a stale `swallowing=true` left
     * over from a previous idle instance (or a previous call with a
     * different pointer) would incorrectly gate the very next press
     * against a decision that no longer means anything. */
    ff_idle_touch_gate_init(&s_touch_gate);
}

/* S26 wake-only-touch amendment (docs/specs/S26-device-lifecycle.md "(c)
 * Inactivity -> dim -> screen off", 2026-09-02: "a touch or button press
 * that begins while the screen is not ACTIVE is a wake-only input and is
 * never delivered to the UI"). Wraps (does not replace) the vendored
 * esp_lvgl_port touch read callback `lvgl_port_add_touch` installs — see
 * `ff_display_touch_start` below, which captures it into
 * `s_touch_orig_read_cb` before swapping this one in via
 * `lv_indev_set_read_cb`. Wrapping rather than reimplementing the touch
 * read means calibration, the 180-degree flip, multi-point handling, and
 * any esp_lvgl_port internals (gesture recognizers, track IDs) stay
 * EXACTLY as they already were — `process_coordinates`
 * (`ff_touchcal_process_cb`) is unchanged and still does calibration +
 * flip only, no gating logic; this function is the ONE place the gate
 * decision is enacted, running AFTER that seam's result is already in
 * `data`.
 *
 * `data->state == LV_INDEV_STATE_PRESSED` after the wrapped call is this
 * poll's PHYSICAL truth (a finger is on the glass) — captured into
 * `s_touch_raw_down` BEFORE the gate can override `data->state`, so
 * `ff_display_touch_is_down()` stays truthful (see that function's own
 * doc comment) regardless of what gets reported to LVGL. The gate is
 * consulted on EVERY poll, pressed or not (`ff_idle_touch_gate`'s own
 * contract: "release always resets the latch") — when it says NOT
 * delivered, `data->state` is forced to RELEASED (the point is left
 * exactly as the wrapped call set it — harmless, since a RELEASED state
 * doesn't act on `.point` past this) so LVGL sees no press at all: no
 * PRESSED style, no CLICKED. Wake itself happens via `ff_idle_touch_gate`
 * firing `ff_idle_input` internally (same call every other input source
 * makes) — this function does not call it separately. */
static void ff_touch_gate_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    if (s_touch_orig_read_cb != NULL) {
        s_touch_orig_read_cb(indev, data);
    }

    bool const physically_down = (data->state == LV_INDEV_STATE_PRESSED);
    s_touch_raw_down = physically_down;

    uint32_t const now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool const deliver = ff_idle_touch_gate(s_touch_idle, &s_touch_gate, now_ms, physically_down);

    if (physically_down && !deliver) {
        data->state = LV_INDEV_STATE_RELEASED;
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
    /* S26 wake-only-touch amendment: wrap esp_lvgl_port's own read
     * callback with the gate (ff_touch_gate_read_cb's own doc comment
     * has the full contract) — captured first so the wrapper can still
     * call through to it. */
    s_touch_orig_read_cb = lv_indev_get_read_cb(indev);
    lv_indev_set_read_cb(indev, ff_touch_gate_read_cb);
    /* Log raw coords on every press so an uncalibrated tap still tells the
     * maintainer where the controller thinks the finger landed. Fires
     * only for a DELIVERED press (LVGL never sees PRESSED for a
     * wake-only-swallowed gesture) — so this log stays an honest record
     * of what actually reached the UI, not every physical touch. */
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
     * not started) reads as "not down", so the caller never blocks.
     *
     * S26 wake-only-touch amendment: reads `s_touch_raw_down`
     * (ff_touch_gate_read_cb's own physical-truth capture), NOT
     * `lv_indev_get_state(s_touch_indev)` — the gate can report
     * LV_INDEV_STATE_RELEASED to LVGL while a wake-only gesture is still
     * physically held, and this function must stay truthful about the
     * REAL finger regardless (the rebuild-mid-tap latch above needs to
     * see the physical finger, not what LVGL was told). `s_touch_indev`
     * being NULL (touch not started) still reads as "not down": nothing
     * has set `s_touch_raw_down` true yet in that case either, so the
     * separate NULL check is redundant but kept for clarity/symmetry
     * with `s_touch_indev`'s other use in this file. */
    return s_touch_indev != NULL && s_touch_raw_down;
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
    /* format v8 amendment: the crosshair is placed at the LOGICAL
     * framebuffer position (s_cal_tx/ty) — under a HARDWARE mirror
     * (screen_flip), it physically APPEARS at that position's 180-degree
     * rotation, which is where the raw ticks captured above (flip
     * deliberately skipped during capture — see s_cal_capturing's doc
     * comment) actually measure. Record the PHYSICAL position as the fit
     * target, not the logical one, so the solved transform is a pure
     * sensor characterization — see s_cal_capturing's doc comment for the
     * full reasoning. A no-op rotation when screen_flip is off. */
    if (s_screen_flip) {
        ff_touchcal_flip180(s_cal_tx[idx], s_cal_ty[idx], FF_LCD_H_RES, FF_LCD_V_RES, &s_cal.pts[idx].screen_x,
                             &s_cal.pts[idx].screen_y);
    } else {
        s_cal.pts[idx].screen_x = s_cal_tx[idx];
        s_cal.pts[idx].screen_y = s_cal_ty[idx];
    }
    ESP_LOGI(TAG, "cal capture %d/%d: raw (%d,%d) -> target (%d,%d) [logical (%d,%d)%s]", idx + 1,
             FF_CAL_TARGET_COUNT, (int)p.x, (int)p.y, s_cal.pts[idx].screen_x, s_cal.pts[idx].screen_y,
             s_cal_tx[idx], s_cal_ty[idx], s_screen_flip ? ", screen_flip on" : "");

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

    /* Capture RAW: the active transform must be identity while we record,
     * AND (format v8 amendment) the screen-flip rotation step must be
     * skipped too — see s_cal_capturing's own doc comment for why. */
    ff_touchcal_identity(&s_active_cal);
    s_cal_capturing = true;
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

    s_cal_capturing = false; /* every point is captured; live touch handling resumes normally */

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
