# ff_power — S25 battery power latch (Waveshare ESP32-S3-Touch-LCD-1.46 soft
# power circuit). Pure HAL: drives the SYS_EN hold line (GPIO7) so the puck
# stays powered from battery after PWR is released. See include/ff_power.h and
# docs/specs/S25-power-latch.md. Direct-GPIO only, so it needs just the driver
# (gpio) + log — no I2C/expander deps.
idf_component_register(
    SRCS "ff_power.c"
    INCLUDE_DIRS "include"
    REQUIRES
        log
        esp_driver_gpio
        driver
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
