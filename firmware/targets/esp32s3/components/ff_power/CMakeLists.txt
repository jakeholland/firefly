# ff_power — S25 battery power latch (Waveshare ESP32-S3-Touch-LCD-1.46 soft
# power circuit) + PWR/BOOT button sampling + S25 slice c battery-sense ADC.
# Pure HAL: drives the SYS_EN hold line (GPIO7) so the puck stays powered
# from battery after PWR is released, samples PWR/BOOT (GPIO6/GPIO0), and
# reads pack voltage off ADC1 channel 7 (GPIO8). See include/ff_power.h and
# docs/specs/S25-power-latch.md. esp_adc: adc_oneshot + adc_cali (slice c).
idf_component_register(
    SRCS "ff_power.c"
    INCLUDE_DIRS "include"
    REQUIRES
        log
        esp_driver_gpio
        driver
        esp_adc
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
