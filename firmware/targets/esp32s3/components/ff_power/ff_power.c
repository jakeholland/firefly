/**
 * ff_power.c — S25 slices a+b+c / S26 slice b. See ff_power.h for the
 * hardware contract.
 *
 * ## S25 slice c — battery-sense ADC (the citation)
 * Pin, attenuation, divider ratio and the measured correction below are
 * all read off Waveshare's own reference driver for this exact board
 * (ESP32-S3-Touch-LCD-1.46), `BAT_Driver.c`:
 * https://github.com/yaosy1997/ESP32-S3-Touch-LCD-1.46-Test/blob/main/main/BAT_Driver/BAT_Driver.c
 * — battery sense is ADC1 channel 7 (GPIO8), `ADC_ATTEN_DB_12`,
 * `ADC_BITWIDTH_DEFAULT`, through a 1:3 resistive divider, and their own
 * code applies a small measured correction on top of the naive ×3
 * (dividing by 0.990476) — this is a single-cell LiPo board, no fuel-
 * gauge IC, so the divider math is the entire "sensor".
 */
#include "ff_power.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#include "ff_power_batt_conv.h"

/* SYS_EN / PWR_Control — the battery keep-alive latch. Drive HIGH to hold the
 * rail on; LOW is a soft power-off. Direct ESP32-S3 GPIO, NOT a TCA9554
 * expander pin, so this needs no I2C bring-up and can run first. Pin from
 * the board's reference driver (PWR_Key.h: PWR_Control_PIN 7). */
#define FF_PIN_PWR_HOLD GPIO_NUM_7

/* PWR key — read for press/long-press detection (core/ff_power_fsm.h owns
 * the decision; this pin sample is all this file does). Pin from the same
 * reference driver (PWR_Key.h: PWR_Key_Input_PIN 6). */
#define FF_PIN_PWR_KEY GPIO_NUM_6

/* BOOT key — GPIO0, the ESP32-family strapping pin (held LOW at reset ->
 * ROM bootloader; a normal input once running). S26's nav model makes this
 * the home button AND the reboot BOOT-release guard's input. */
#define FF_PIN_BOOT_KEY GPIO_NUM_0

/* THE ACTIVE-LEVEL INTERPRETATION CALL — see ff_power_pwr_pressed's doc
 * comment in ff_power.h. 1 = active-LOW (the reference driver's own
 * boot-time held read, `!gpio_get_level`), matching Waveshare's PWR_Key.c.
 * Flip THIS ONE DEFINE if bring-up on glass shows the button reads the
 * other way — nothing else in this file or its callers encodes the
 * polarity. */
#define FF_POWER_PWR_ACTIVE_LOW 1

/* S25 slice c — battery-sense ADC. See this file's top comment for the
 * citation; GPIO8 is ADC1's channel 7 on the ESP32-S3 (fixed by the SoC's
 * ADC-to-GPIO map, not a board choice — matches the reference driver's
 * own pin). 12 dB attenuation is the reference driver's own choice too:
 * the widest of the SoC's four attenuation steps, needed because the
 * divided pack voltage (a single-cell LiPo's ~3.0-4.35V pack, /3 at the
 * divider -> ~1.0-1.45V at the pin) still sits above the ~950mV a lower
 * attenuation setting could read without clipping. */
#define FF_PIN_BATT_ADC GPIO_NUM_8
#define FF_BATT_ADC_UNIT ADC_UNIT_1
#define FF_BATT_ADC_CHANNEL ADC_CHANNEL_7 /* GPIO8 on ADC1, this SoC's fixed map */
#define FF_BATT_ADC_ATTEN ADC_ATTEN_DB_12
#define FF_BATT_ADC_BITWIDTH ADC_BITWIDTH_DEFAULT

/* Raw reads averaged per `ff_power_batt_mv()` call, cutting sample-to-
 * sample ADC noise before calibration/conversion even runs (see
 * ff_power.h's doc comment on that function for why this is a SEPARATE
 * concern from ff_batt_filter_t's own cross-tick smoothing, one layer
 * up). 8 is a small, cheap-to-read power of two — each `adc_oneshot_
 * read` is a few microseconds, so 8 of them add negligible cost to a
 * call this file's own caller (app_main.c) makes once every 2 seconds. */
#define FF_BATT_ADC_SAMPLES 8u

/* The pack-voltage conversion (the board's 1:3 divider composed with
 * Waveshare's own measured correction) is pure arithmetic with no ESP
 * dependency — hoisted into ff_power_batt_conv.h (review fix: also
 * where the ×1e6-scaled constant's derivation is documented and where
 * a HOST test, targets/sim/tests/test_batt_pack_mv.c, pins it by
 * literal) rather than defined here. */

static const char *TAG = "ff_power";

esp_err_t ff_power_latch_on(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << FF_PIN_PWR_HOLD,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PWR-hold gpio_config(GPIO%d) failed: %s", FF_PIN_PWR_HOLD, esp_err_to_name(err));
        return err;
    }

    err = gpio_set_level(FF_PIN_PWR_HOLD, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PWR-hold set high failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "battery power latched on (SYS_EN GPIO%d high)", FF_PIN_PWR_HOLD);
    return ESP_OK;
}

/* Shared by ff_power_off (a fresh boot that never called ff_power_latch_on
 * on this path, e.g. a unit/bench harness) and ff_power_latch_on's own
 * config — idempotent (gpio_config on an already-output pin is a no-op
 * re-apply, not an error). */
static esp_err_t ff_power_hold_configure_output(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << FF_PIN_PWR_HOLD,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io);
}

esp_err_t ff_power_off(void)
{
    esp_err_t err = ff_power_hold_configure_output();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PWR-hold gpio_config(GPIO%d) failed on power-off: %s", FF_PIN_PWR_HOLD, esp_err_to_name(err));
        return err;
    }

    err = gpio_set_level(FF_PIN_PWR_HOLD, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PWR-hold set low failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "soft power-off: SYS_EN GPIO%d low (battery: rail drops now; USB: board stays up)",
             FF_PIN_PWR_HOLD);
    return ESP_OK;
}

/* First-call input configuration, shared shape for both key inputs below:
 * configure once, lazily, on the first sample rather than requiring a
 * separate init call the way the two output pins above do — S26's spec
 * scope has app_main tick these every loop iteration with no other
 * bring-up step, and a missed explicit init would otherwise read a
 * floating pin forever. */
static bool s_pwr_key_configured;
static bool s_boot_key_configured;
static bool s_pwr_key_first_read_logged;

bool ff_power_pwr_pressed(void)
{
    if (!s_pwr_key_configured) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << FF_PIN_PWR_KEY,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&io);
        if (err != ESP_OK) {
            /* Non-fatal, same "log and continue" posture as
             * ff_power_latch_on's own failure path: a puck that cannot
             * read PWR is still more debuggable running than parked. A
             * failed config leaves this false forever (gpio_get_level on
             * an unconfigured pin is not something to trust either way),
             * which is the honest degrade — never claim a press that
             * cannot be sampled. */
            ESP_LOGE(TAG, "PWR-key gpio_config(GPIO%d) failed: %s", FF_PIN_PWR_KEY, esp_err_to_name(err));
            return false;
        }
        s_pwr_key_configured = true;
    }

    int const raw = gpio_get_level(FF_PIN_PWR_KEY);
    if (!s_pwr_key_first_read_logged) {
        s_pwr_key_first_read_logged = true;
        ESP_LOGI(TAG, "PWR key (GPIO%d) first raw read: %d (active_low=%d) — verify against a physical press on glass",
                 FF_PIN_PWR_KEY, raw, FF_POWER_PWR_ACTIVE_LOW);
    }

#if FF_POWER_PWR_ACTIVE_LOW
    return raw == 0;
#else
    return raw != 0;
#endif
}

bool ff_power_boot_pressed(void)
{
    if (!s_boot_key_configured) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << FF_PIN_BOOT_KEY,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE, /* idle-high; pressed pulls to ground */
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&io);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "BOOT-key gpio_config(GPIO%d) failed: %s", FF_PIN_BOOT_KEY, esp_err_to_name(err));
            return false; /* honest degrade — see ff_power_pwr_pressed's note */
        }
        s_boot_key_configured = true;
    }

    return gpio_get_level(FF_PIN_BOOT_KEY) == 0; /* active-LOW, standard ESP32 BOOT-button convention */
}

/* ---------------------------------------------------------------------
 * S25 slice c — battery-sense ADC. See this file's top comment for the
 * hardware citation and the constants block above for the pin/atten/
 * correction rationale.
 * ------------------------------------------------------------------- */

static adc_oneshot_unit_handle_t s_batt_adc;      /* NULL until ff_power_batt_init succeeds */
static adc_cali_handle_t         s_batt_cali;     /* NULL until a calibration scheme is established */
static char const               *s_batt_cali_kind = "NONE"; /* "curve" / "line" / "NONE" — honesty label, see doc comment */
static bool                      s_batt_first_reading_logged;

/* Tiered calibration bring-up, same order (and the same reason to try
 * each) as ESP-IDF's own esp_adc oneshot_read example: curve fitting
 * first, then line fitting on any chip where that scheme compiles in,
 * then none. Both `#if` guards mirror `adc_cali_schemes.h`'s own
 * per-chip feature macros rather than assuming either is available —
 * on THIS chip (ESP32-S3) only `ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED`
 * is ever defined (ESP32-S3 has no line-fitting scheme at all), so the
 * line-fitting branch below is inert here, kept for the same
 * "don't assume this file never runs on a different chip" portability
 * reason the SDK's own example keeps it. Sets s_batt_cali/s_batt_cali_kind;
 * returns true iff a scheme was established. */
static bool ff_power_batt_cali_init(void)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    {
        adc_cali_curve_fitting_config_t const cfg = {
            .unit_id = FF_BATT_ADC_UNIT,
            .chan = FF_BATT_ADC_CHANNEL,
            .atten = FF_BATT_ADC_ATTEN,
            .bitwidth = FF_BATT_ADC_BITWIDTH,
        };
        esp_err_t const err = adc_cali_create_scheme_curve_fitting(&cfg, &s_batt_cali);
        if (err == ESP_OK) {
            s_batt_cali_kind = "curve";
            return true;
        }
        ESP_LOGW(TAG, "battery ADC curve-fit calibration unavailable: %s", esp_err_to_name(err));
    }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    {
        adc_cali_line_fitting_config_t const cfg = {
            .unit_id = FF_BATT_ADC_UNIT,
            .atten = FF_BATT_ADC_ATTEN,
            .bitwidth = FF_BATT_ADC_BITWIDTH,
        };
        esp_err_t const err = adc_cali_create_scheme_line_fitting(&cfg, &s_batt_cali);
        if (err == ESP_OK) {
            s_batt_cali_kind = "line";
            return true;
        }
        ESP_LOGW(TAG, "battery ADC line-fit calibration unavailable: %s", esp_err_to_name(err));
    }
#endif
    s_batt_cali_kind = "NONE";
    s_batt_cali = NULL;
    return false;
}

esp_err_t ff_power_batt_init(void)
{
    adc_oneshot_unit_init_cfg_t const unit_cfg = {
        .unit_id = FF_BATT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_batt_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "battery ADC unit init failed: %s — battery reading will stay unknown", esp_err_to_name(err));
        s_batt_adc = NULL;
        return err;
    }

    adc_oneshot_chan_cfg_t const chan_cfg = {
        .atten = FF_BATT_ADC_ATTEN,
        .bitwidth = FF_BATT_ADC_BITWIDTH,
    };
    err = adc_oneshot_config_channel(s_batt_adc, FF_BATT_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "battery ADC channel config (GPIO%d) failed: %s — battery reading will stay unknown",
                 FF_PIN_BATT_ADC, esp_err_to_name(err));
        return err;
    }

    bool const calibrated = ff_power_batt_cali_init();
    ESP_LOGI(TAG, "battery ADC up: unit=1 chan=%d (GPIO%d) atten=12dB calibration=%s%s", FF_BATT_ADC_CHANNEL,
             FF_PIN_BATT_ADC, s_batt_cali_kind, calibrated ? "" : " (uncalibrated — readings will report unknown)");
    return ESP_OK; /* an uncalibrated ADC is a logged degrade, not an init failure — see ff_power.h's doc comment */
}

uint16_t ff_power_batt_mv(void)
{
    if (s_batt_adc == NULL || s_batt_cali == NULL) {
        return 0; /* not initialized, or no calibration scheme — honest "unknown", never a fabricated figure */
    }

    uint32_t raw_sum = 0;
    for (uint32_t i = 0; i < FF_BATT_ADC_SAMPLES; i++) {
        int raw = 0;
        esp_err_t const err = adc_oneshot_read(s_batt_adc, FF_BATT_ADC_CHANNEL, &raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "battery ADC read failed: %s", esp_err_to_name(err));
            return 0;
        }
        raw_sum += (uint32_t)raw;
    }
    uint32_t const raw_avg = raw_sum / FF_BATT_ADC_SAMPLES;

    int cal_mv = 0;
    esp_err_t const cal_err = adc_cali_raw_to_voltage(s_batt_cali, (int)raw_avg, &cal_mv);
    if (cal_err != ESP_OK || cal_mv < 0) {
        ESP_LOGW(TAG, "battery ADC calibration convert failed: %s", esp_err_to_name(cal_err));
        return 0;
    }

    /* Pin mV -> pack mV: the pure conversion, host-testable —
     * ff_power_batt_conv.h's own doc comment has the arithmetic, the
     * uint64_t-overflow reasoning, and the clamp rationale. */
    uint16_t const pack_mv = ff_power_batt_pack_mv_from_cal_mv((uint32_t)cal_mv);

    if (!s_batt_first_reading_logged) {
        s_batt_first_reading_logged = true;
        ESP_LOGI(TAG,
                 "S25c first battery reading — raw_avg=%u calibration=%s cal_mv=%d pack_mv=%u "
                 "(sanity-check pack_mv against a multimeter on the pack)",
                 (unsigned)raw_avg, s_batt_cali_kind, cal_mv, (unsigned)pack_mv);
    }

    return pack_mv;
}
