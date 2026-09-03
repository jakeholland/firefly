# ff_audio — S27 sounds device HAL (Waveshare ESP32-S3-Touch-LCD-1.46's
# onboard PCM5101 I2S DAC): turns a core ff_sound_pattern_t into synthesized
# PCM tones over I2S0. Pure HAL — no domain/policy logic (that's core's
# ff_sound.c and app's ff_shell.c, both unchanged by this component). See
# include/ff_audio.h and docs/specs/S27-sounds.md ("Amendments — device
# half").
#
# REQUIRES ff_core: pulls in ff_sound.h's ff_sound_event_t/ff_sound_pattern_t/
# ff_sound_pattern_for/ff_sound_preempts (this HAL only RENDERS a pattern,
# it never invents one). esp_driver_i2s: the I2S0 standard-mode TX driver.
# freertos: the render task + 1-deep mailbox queue this HAL is built around.
# PRIV_REQUIRES esp_driver_gpio: ff_audio.c's own #include "driver/gpio.h"
# (GPIO_NUM_* pin macros for the BCLK/WS/DOUT wiring) — private because no
# public header of this component (ff_audio.h) exposes a GPIO type.
idf_component_register(
    SRCS "ff_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES
        ff_core
        esp_driver_i2s
        freertos
    PRIV_REQUIRES esp_driver_gpio
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
