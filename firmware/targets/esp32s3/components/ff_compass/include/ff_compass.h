/**
 * ff_compass.h — S15 device HAL: GY-273 magnetometer (QMC5883L or
 * HMC5883L, auto-detected) tilt-compensated with the onboard QMI8658
 * accelerometer, on the SAME shared I2C bus `ff_display` already owns
 * (SDA GPIO11 / SCL GPIO10 — docs/hardware/comms-brain.md's header pin
 * map; ff_display.c's own "Shared I2C bus" comment). Today `heading_deg`
 * is -1 forever (docs/specs/S12-first-run.md's 2026-09-03 amendment: "no
 * IMU/magnetometer driver anywhere under firmware/targets/esp32s3/") —
 * this component is that driver.
 *
 * Pure HAL, the same discipline `ff_power.h` documents for the battery
 * ADC: this file drives two I2C chips and turns their raw samples into
 * a heading via `ff_geo_heading_deg` (firmware/core, pure math) — it
 * makes no UI/display decision and owns no persisted state of its own
 * (the one exception, `ff_compass_set_cal`, is a plain in-RAM setter —
 * see below). `ff_compass_read()` is a push-API SOURCE: app_main.c
 * samples it on its own periodic tick (mirroring `ff_power_batt_mv()`'s
 * own call site) and forwards the result straight to
 * `ff_shell_set_heading` (app/include/ff_shell.h) with no
 * interpretation in between.
 *
 * ## Hardware — onboard IMU (verified)
 * The board's onboard 6-axis IMU is a QMI8658, I2C address 0x6B (alt
 * strap 0x6A). Waveshare's own ESP-IDF 5.3.2 reference demo for this
 * exact board (ESP32-S3-Touch-LCD-1.46) carries a QMI8658 driver:
 * https://github.com/yaosy1997/ESP32-S3-Touch-LCD-1.46-Test/blob/main/main/QMI8658/QMI8658.c
 * (and its QMI8658.h) — WHO_AM_I register 0x00 reads 0x05, CTRL2 (0x03)
 * sets accel full-scale/ODR, CTRL7 (0x08) enables the accel/gyro
 * engines, and the accel output is six bytes at 0x35..0x3A (AX_L, AX_H,
 * AY_L, AY_H, AZ_L, AZ_H, little-endian per axis). Register addresses
 * and the WHO_AM_I value are confirmed against that file; the exact
 * bring-up bytes this driver writes (and how they were computed from
 * that file's own bit-layout `#define`s) are cited again at each use
 * site in ff_compass.c.
 *
 * ## Hardware — GY-273 magnetometer (assumed wiring, auto-detected part)
 * The GY-273 is an AFTERMARKET module wired to the puck's own back
 * header (docs/hardware/comms-brain.md's header pin map: SDA/SCL on the
 * SAME pair the touch/expander bus already uses, GPIO11/GPIO10; VCC to
 * 3V3, GND to G; DRDY unconnected) — NOT part of Waveshare's board, so
 * there is no vendor reference driver for it. Most GY-273 boards
 * actually carry a QMC5883L at I2C address 0x0D even when
 * silkscreened "HMC5883L"; a genuine HMC5883L at 0x1E does turn up on
 * some. `ff_compass_init` auto-detects which is present from each
 * chip's own identification registers — see ff_compass.c for the exact
 * register values and their datasheet citations.
 *
 * ## Honesty contract (CLAUDE.md: "honest data over pretty data")
 * `ff_compass_init` NEVER assumes a magnetometer is present: if neither
 * candidate chip identifies correctly, it logs "no magnetometer" once
 * and leaves `ff_compass_present()` false forever — `ff_compass_read()`
 * then always returns the shell's own -1 "unknown/unreliable" sentinel
 * (the exact contract `ff_shell_set_heading`, app/include/ff_shell.h,
 * already documents), never a fabricated heading. The IMU gets the same
 * treatment, with one documented exception: if the QMI8658 fails to
 * identify, `ff_compass_read()` falls back to an ASSUMED-LEVEL accel
 * reading (0, 0, +1g in board frame) so a magnetometer-only board still
 * gets an (untilt-corrected) heading rather than none at all — logged
 * once, unconditionally ("compass: no IMU — assuming level"), so this
 * degrade is never silent. See this driver's introducing PR body for
 * the field consequence: tilt rejection (the >60 degree cutoff
 * `ff_geo_heading_deg` otherwise applies) is unavailable on that path,
 * since a synthesized always-level accel can never indicate tilt.
 *
 * ## Calibration
 * `ff_settings_t.compass_cal` / `.cal_valid` (core/include/ff_settings.h)
 * is the ONE persisted calibration this codebase has — S12's
 * figure-eight ritual UI that would ever populate it for real has not
 * shipped (S12-first-run.md's own 2026-09-03 amendment). `ff_compass_set_cal`
 * below is the runtime seam that ritual will call once it exists;
 * app_main.c wires it ONCE at boot, from whatever `ff_shell_settings()`
 * already loaded from NVS (identity/`cal_valid == false` on every puck
 * today, since nothing has ever written a real one) — this driver
 * itself never reads or writes settings/NVS.
 */
#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "ff_geo.h" /* ff_geo_cal_t, ff_vec3_t, ff_geo_heading_deg — plain math, zero I/O; see this header's top comment */

#ifdef __cplusplus
extern "C" {
#endif

/** ff_compass_mag_kind_t — which magnetometer chip (if any) `ff_compass_init`
 * found on the bus. Logged and exposed mainly for bring-up/bench
 * debugging — `ff_compass_read()` handles both transparently. */
typedef enum {
    FF_COMPASS_MAG_NONE = 0, /* ff_compass_init found neither candidate chip */
    FF_COMPASS_MAG_QMC5883L,
    FF_COMPASS_MAG_HMC5883L,
} ff_compass_mag_kind_t;

/**
 * ff_compass_init — probe the shared I2C bus `bus` (from
 * `ff_display_i2c_bus()`, called after `ff_display_expander_init()` has
 * brought that bus up) for the onboard QMI8658 IMU and either GY-273
 * magnetometer chip, bringing up whichever it finds. Never opens a
 * second I2C master on these pins — see `ff_display_i2c_bus`'s own doc
 * comment for why that would not even work.
 *
 * Non-fatal, "log and continue" HAL posture (matching every other
 * device bring-up in this codebase, e.g. `ff_power_batt_init`): a
 * missing magnetometer, a missing IMU, or both, are LOGGED findings,
 * not errors — `ff_compass_present()`/`ff_compass_imu_present()` report
 * the outcome and `ff_compass_read()` degrades honestly (see this
 * header's top comment). Returns `ESP_ERR_INVALID_ARG` only if `bus` is
 * NULL (a caller bug — the display bus was never brought up); `ESP_OK`
 * in every other case, including "found nothing at all".
 */
esp_err_t ff_compass_init(i2c_master_bus_handle_t bus);

/** ff_compass_present — true once `ff_compass_init` has identified a
 * magnetometer chip (either kind). False before init, or if neither
 * candidate chip identified correctly. */
bool ff_compass_present(void);

/** ff_compass_mag_kind — which magnetometer chip is in use, or
 * `FF_COMPASS_MAG_NONE` if `ff_compass_present()` is false. */
ff_compass_mag_kind_t ff_compass_mag_kind(void);

/** ff_compass_imu_present — true once `ff_compass_init` has identified
 * the onboard QMI8658. False before init, or if it never ACKed/
 * identified — in which case `ff_compass_read()` falls back to an
 * assumed-level accel (see this header's top comment). */
bool ff_compass_imu_present(void);

/**
 * ff_compass_status_t / ff_compass_status — a one-shot snapshot for a
 * bench diagnostic (the debug console's `i2c` command,
 * docs/hardware/comms-brain.md), bundling three already-honest facts
 * this driver tracks into one call rather than three: mag/imu presence
 * (the same `ff_compass_present`/`ff_compass_imu_present` above) plus
 * whether the MOST RECENT `ff_compass_read()` call produced a real
 * heading. "Most recent", not "fresh right now": this reports the last
 * sample the periodic 10 Hz caller (app_main.c) already took, not a
 * new I2C transaction of its own — a status query is diagnostic, not
 * another consumer of bus time. Before the first `ff_compass_read()`
 * call ever happens (e.g. queried moments after boot, or with
 * `CONFIG_FF_COMPASS=n` so nothing ever calls it), `heading_valid` is
 * false and `last_heading_deg` is the same -1 "unknown" sentinel
 * `ff_compass_read()` itself would return — never a fabricated 0. */
typedef struct {
    bool mag_present;
    ff_compass_mag_kind_t mag_kind;
    bool imu_present;
    bool heading_valid;     /* true iff last_heading_deg is a real (non-negative) heading */
    float last_heading_deg; /* meaningful only when heading_valid; -1 otherwise */
} ff_compass_status_t;

ff_compass_status_t ff_compass_status(void);

/**
 * ff_compass_set_cal — install (or clear, if `cal` is NULL) the active
 * compass calibration `ff_compass_read()` applies via
 * `ff_geo_heading_deg`. `cal` is COPIED — the caller's storage need not
 * outlive this call. This is the runtime seam a future figure-eight
 * calibration UI (S12) will call after a completed ritual
 * (`ff_geo_cal_finish`); no such UI exists yet, so today's only caller
 * is app_main.c's boot-time load of `ff_shell_settings()->compass_cal`
 * when `->cal_valid` is true (see this header's "Calibration" section).
 * Safe to call at any time, including before `ff_compass_init`.
 */
void ff_compass_set_cal(ff_geo_cal_t const *cal);

/**
 * ff_compass_read — sample the magnetometer (and, if present, the
 * IMU's accelerometer), remap both into board frame, and return the
 * tilt-compensated heading via `ff_geo_heading_deg` — degrees [0, 360),
 * 0 = north, clockwise, OR a NEGATIVE value for "unknown/unreliable"
 * (no magnetometer present, an I2C read failed this sample, tilt
 * exceeded the reliable range, or the calibrated mag reading is
 * degenerate — see `ff_geo_heading_deg`'s own doc comment for the full
 * list). NEVER fabricates a heading it does not have real sensor
 * evidence for. Safe to call before `ff_compass_init` (returns -1,
 * `ff_compass_present()` is false) and at any rate — this function is
 * a plain synchronous I2C read plus pure math, meant to be polled
 * periodically (10 Hz — app_main.c's own `FF_COMPASS_SAMPLE_PERIOD_MS`)
 * by its one caller.
 */
float ff_compass_read(void);

#ifdef __cplusplus
}
#endif
