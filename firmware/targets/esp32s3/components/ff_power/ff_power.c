/**
 * ff_power.c — S25 slices a+b / S26 slice b. See ff_power.h for the
 * hardware contract.
 */
#include "ff_power.h"

#include "driver/gpio.h"
#include "esp_log.h"

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
