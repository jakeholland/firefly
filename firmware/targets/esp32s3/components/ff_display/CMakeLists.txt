# ff_display — the S15 slice b display + touch HAL (Waveshare
# ESP32-S3-Touch-LCD-1.46: SPD2010 QSPI panel + SPD2010 I2C touch, behind
# a TCA9554 IO expander). The device analogue of the sim's SDL display +
# pointer indev; see include/ff_display.h.
#
# All the peripheral drivers are managed components (pinned in
# main/idf_component.yml) — this component just REQUIRES them so their
# headers resolve. esp_driver_i2c / esp_driver_spi / esp_driver_gpio are
# IDF built-ins for the shared I2C bus, the QSPI panel bus and the
# backlight pin.
#
# Three .c files (tech-debt sprint, device-target slice): ff_display.c is
# the production HAL; ff_display_debug.c holds the two bring-up-only
# diagnostic draw surfaces (STAGE-1 test pattern, CONFIG_FF_GLASS_RULER);
# ff_display_cal.c holds the S15d crosshair-calibration flow. They share
# a handful of file-scope handles/state via the PRIVATE
# ff_display_internal.h (source dir, not include/ — not installed, not
# part of this component's public interface). SRCS listed explicitly, not
# GLOB'd (house lesson #112).
idf_component_register(
    SRCS "ff_display.c" "ff_display_debug.c" "ff_display_cal.c"
    INCLUDE_DIRS "include"
    REQUIRES
        ff_core
        log
        lvgl
        esp_lvgl_port
        esp_lcd
        esp_lcd_spd2010
        esp_lcd_touch_spd2010
        esp_io_expander_tca9554
        esp_driver_i2c
        esp_driver_spi
        esp_driver_gpio
        esp_driver_ledc
        driver
        esp_timer # S26 slice (g) — esp_timer_get_time() for the boot-splash timing log
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
