# ff_compass — S15 device HAL: GY-273 magnetometer (QMC5883L or
# HMC5883L, auto-detected at init) tilt-compensated with the onboard
# QMI8658 6-axis IMU's accelerometer, feeding ff_geo_heading_deg
# (firmware/core, pure math) via a push API — ff_compass_read() ->
# app_main.c -> ff_shell_set_heading, the same shape S25 slice c's
# ff_power_batt_mv() -> ff_shell_set_batt_mv already uses. See
# include/ff_compass.h for the hardware contract and citations.
#
# esp_driver_i2c: i2c_master_dev_handle_t + the transmit/receive/probe
# calls this HAL issues directly against devices on the bus ff_display
# already owns (this component never calls i2c_new_master_bus itself —
# see ff_display_i2c_bus()). ff_core: ff_geo.h (ff_geo_heading_deg,
# ff_geo_cal_t, ff_vec3_t) — plain math, zero core STATE touched, same
# category ff_display already includes core headers under (that
# component's own top-of-file comment). freertos: one short vTaskDelay
# after the IMU's bring-up writes, letting the accel engine settle
# before the first read.
idf_component_register(
    SRCS "ff_compass.c"
    INCLUDE_DIRS "include"
    REQUIRES
        ff_core
        log
        esp_driver_i2c
    PRIV_REQUIRES freertos
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
