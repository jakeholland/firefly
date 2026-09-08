# ff_mic — S30 mic bring-up device HAL (Waveshare ESP32-S3-Touch-LCD-1.46's
# onboard I2S MEMS mic): I2S1 RX + a reader task that turns raw 32-bit I2S
# frames into RMS/peak/envelope dBFS via firmware/core's ff_miclevel.h. Pure
# HAL — no domain/policy logic (this driver decides nothing about WHICH face
# reads the mic or why; see include/ff_mic.h). See docs/specs/S30-audio-input.md.
#
# REQUIRES ff_core: pulls in ff_miclevel.h's pure level math (RMS/peak dBFS,
# DC-block HPF, attack/release envelope) — this HAL only APPLIES that math to
# real samples, it never reimplements it (the same math is unit-tested on the
# host, firmware/core/tests/test_miclevel.c). esp_driver_i2s: the I2S1
# standard-mode RX driver. freertos: the reader task + start semaphore this
# HAL is built around.
# PRIV_REQUIRES esp_driver_gpio: ff_mic.c's own #include "driver/gpio.h"
# (GPIO_NUM_* pin macros for the BCLK/WS/DIN wiring) — private because no
# public header of this component (ff_mic.h) exposes a GPIO type. esp_system:
# esp_task_wdt.h (the reader task's own self-subscription — see ff_mic.c's
# top comment, "Reader task"). esp_timer: esp_timer_get_time() (ff_mic.c's
# own now-clock, ff_mic_now_ms).
idf_component_register(
    SRCS "ff_mic.c"
    INCLUDE_DIRS "include"
    REQUIRES
        ff_core
        esp_driver_i2s
        freertos
    PRIV_REQUIRES esp_driver_gpio esp_system esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
