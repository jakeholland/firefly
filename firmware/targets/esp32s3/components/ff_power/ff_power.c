/**
 * ff_power.c — S25 slice a. See ff_power.h for the hardware contract.
 */
#include "ff_power.h"

#include "driver/gpio.h"
#include "esp_log.h"

/* SYS_EN / PWR_Control — the battery keep-alive latch. Drive HIGH to hold the
 * rail on; LOW is a soft power-off (deferred slice b). Direct ESP32-S3 GPIO,
 * NOT a TCA9554 expander pin, so this needs no I2C bring-up and can run first.
 * Pin from the board's reference driver (PWR_Key.h: PWR_Control_PIN 7). */
#define FF_PIN_PWR_HOLD GPIO_NUM_7

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
