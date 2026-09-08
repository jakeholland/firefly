/**
 * ff_compass.h — S15 device HAL: GY-273 magnetometer (QMC5883L,
 * HMC5883L, or QMC5883P — auto-detected) tilt-compensated with the
 * onboard QMI8658 accelerometer, on the SAME shared I2C bus `ff_display`
 * already owns
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
 * 2026-09-07 (second-board bench evidence): the reference demo's own
 * bring-up (WHO_AM_I check, then straight to CTRL1/CTRL2/CTRL7) is not
 * sufficient on every board — a second unit identified correctly and
 * every bring-up write returned ESP_OK, yet the accel engine never
 * produced real data (permanent 0x8000/0x7FFF sentinel samples). This
 * driver now also: soft-resets the chip first (RESET register 0x60 =
 * 0xB0) and re-verifies WHO_AM_I post-reset before configuring; polls
 * STATUS0 (0x2E) bit0 (aDA) for data-ready at bring-up; validates every
 * accel sample at read time (rejecting sentinel/out-of-range values
 * rather than trusting them); and re-runs the bring-up sequence,
 * rate-limited, if data stays invalid for more than a couple of
 * seconds. See ff_compass.c's FF_QMI8658_REG_RESET block comment for
 * the datasheet citations and every timing constant's rationale, and
 * `ff_compass_imu_state_t` below for the resulting three-state health
 * fact this makes visible.
 *
 * ## Hardware — GY-273 magnetometer (assumed wiring, auto-detected part)
 * The GY-273 is an AFTERMARKET module wired to the puck's own back
 * header (docs/hardware/comms-brain.md's header pin map: SDA/SCL on the
 * SAME pair the touch/expander bus already uses, GPIO11/GPIO10; VCC to
 * 3V3, GND to G; DRDY unconnected) — NOT part of Waveshare's board, so
 * there is no vendor reference driver for it. Most GY-273 boards
 * actually carry a QMC5883L at I2C address 0x0D even when
 * silkscreened "HMC5883L"; a genuine HMC5883L at 0x1E does turn up on
 * some. A THIRD case, confirmed on the coordinator's own bench
 * (2026-09-05, real puck, GY-273 wired and powered): current-production
 * GY-273 clones increasingly ship a **QMC5883P** instead — QST's
 * successor part, I2C address `0x2C`, CHIP_ID register 0x00 reading
 * 0x80 (QMC5883P datasheet, QST doc #13-52-19 Rev A, section 9.2.1).
 * `ff_compass_init` auto-detects which of the three is present from
 * each chip's own identification registers — see ff_compass.c for the
 * exact register values and their datasheet citations.
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
 * The SAME assumed-level fallback also covers a QMI8658 that identifies
 * and configures successfully but never produces plausible data
 * (`ff_compass_imu_state_t`'s `FF_COMPASS_IMU_NO_DATA` — the 2026-09-07
 * second-board case above): the honesty contract does not distinguish
 * "no chip" from "chip present but not trustworthy" in the accel value
 * it feeds `ff_geo_heading_deg` (both get the same assumed-level
 * vector), but it DOES distinguish them in `ff_compass_status()` and the
 * boot/runtime log, so a bench operator is never left thinking a
 * NO_DATA IMU is healthy just because `ff_compass_imu_present()` is
 * true.
 *
 * ## Calibration
 * `ff_settings_t.compass_cal` / `.cal_valid` (core/include/ff_settings.h)
 * is the ONE persisted calibration this codebase has. S12 step 3 (the
 * figure-eight ritual UI, docs/specs/S12-first-run.md's own 2026-09-03
 * amendment recorded the gap; this driver's own PR closes it) is now
 * the thing that populates it for real: `ff_compass_set_cal` below is
 * the runtime seam that ritual calls (via `app_main.c`) the moment a
 * calibration session FINISHes successfully, and again on CLEAR (with
 * `cal = NULL`) — see `ff_shell_cfg_t.compass_cal_changed`'s doc
 * comment (app/include/ff_shell.h). `app_main.c` ALSO still wires it
 * once at boot, from whatever `ff_shell_settings()` already loaded from
 * NVS, so a calibration from a PRIOR session survives a reboot without
 * needing this driver to touch settings/NVS itself — this driver never
 * reads or writes settings/NVS directly, on either path.
 *
 * `ff_compass_last_mag_board` below is the other half of that seam: the
 * ritual needs raw (pre-calibration) board-frame samples to fit
 * against, fed at the platform's own compass sample rate — see that
 * function's own doc comment.
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
 * debugging — `ff_compass_read()` handles all three transparently. */
typedef enum {
    FF_COMPASS_MAG_NONE = 0, /* ff_compass_init found none of the candidate chips */
    FF_COMPASS_MAG_QMC5883L,
    FF_COMPASS_MAG_HMC5883L,
    FF_COMPASS_MAG_QMC5883P,
} ff_compass_mag_kind_t;

/** ff_compass_mag_kind_name — lowercase chip name for `kind`
 * ("qmc5883l"/"hmc5883l"/"qmc5883p"), or "none" for
 * `FF_COMPASS_MAG_NONE`. Single source of truth for naming the part in
 * the boot log, `ff_compass_status()`'s console line
 * (docs/hardware/comms-brain.md's `i2c` bench command), and anywhere
 * else that needs to print which chip is in use. */
char const *ff_compass_mag_kind_name(ff_compass_mag_kind_t kind);

/** ff_compass_imu_state_t — bring-up/data-health state of the onboard
 * QMI8658, a finer-grained fact than `ff_compass_imu_present()` alone
 * (added after 2026-09-07 bench evidence: a SECOND board identified the
 * chip correctly and every bring-up write returned ESP_OK, yet the
 * accel engine never produced a real sample — the WHO_AM_I probe alone
 * cannot distinguish that from a genuinely healthy IMU). See
 * ff_compass.c's FF_QMI8658_REG_RESET block comment for the full
 * bring-up sequence and datasheet citations this state reflects. */
typedef enum {
    FF_COMPASS_IMU_ABSENT = 0, /* ff_compass_init never identified a QMI8658 at all */
    FF_COMPASS_IMU_NO_DATA,    /* identified and configured, but every accel sample so far failed validation
                                   (sentinel/out-of-range) — heading falls back to assumed-level, same as ABSENT */
    FF_COMPASS_IMU_OK,         /* identified, configured, and at least one accel sample has validated */
} ff_compass_imu_state_t;

/** ff_compass_imu_state_name — lowercase name for `state`
 * ("absent"/"no-data"/"ok"), for the boot log and `ff_compass_status()`'s
 * console line (docs/hardware/comms-brain.md's `i2c` bench command),
 * mirroring `ff_compass_mag_kind_name`'s role for the magnetometer. */
char const *ff_compass_imu_state_name(ff_compass_imu_state_t state);

/**
 * ff_compass_init — probe the shared I2C bus `bus` (from
 * `ff_display_i2c_bus()`, called after `ff_display_expander_init()` has
 * brought that bus up) for the onboard QMI8658 IMU and any of the three
 * candidate GY-273 magnetometer chips, bringing up whichever it finds.
 * Never opens a second I2C master on these pins — see
 * `ff_display_i2c_bus`'s own doc comment for why that would not even
 * work.
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
    ff_compass_imu_state_t imu_state; /* finer-grained than imu_present alone — see that type's own doc comment */
    bool heading_valid;               /* true iff last_heading_deg is a real (non-negative) heading */
    float last_heading_deg;           /* meaningful only when heading_valid; -1 otherwise */
} ff_compass_status_t;

ff_compass_status_t ff_compass_status(void);

/**
 * ff_compass_last_mag_board — S12 step 3: the board-frame magnetometer
 * vector (after the per-chip axis remap, BEFORE calibration) from the
 * MOST RECENT `ff_compass_read()` call — the exact same vector that
 * call handed `ff_geo_heading_deg`. This is the sample source the
 * figure-eight calibration ritual (`ff_shell_compass_cal_sample`,
 * app/include/ff_shell.h) needs: feeding it anything else (raw
 * sensor-frame bytes, an already-calibrated vector, or a value from a
 * SECOND I2C transaction that might not agree with the one the live
 * heading used) would fit a calibration against axes the heading
 * computation doesn't actually use.
 *
 * "Most recent", not "fresh right now" — same convention
 * `ff_compass_status()`'s own doc comment establishes for
 * `last_heading_deg`: this reports the last periodic sample
 * (app_main.c's 10 Hz `ff_compass_read()` tick) already took, not a new
 * bus transaction of its own. Returns the zero vector before the first
 * `ff_compass_read()` call ever happens, or on every call while no
 * magnetometer is present — `ff_compass_read()` never writes this
 * field on that early-return path, so it stays at its honest
 * "nothing real yet" zero default (this header's own top comment: this
 * driver never fabricates a reading it has no sensor evidence for).
 */
ff_vec3_t ff_compass_last_mag_board(void);

/**
 * ff_compass_last_accel_board — S31 Music/Swarm: the board-frame
 * ACCELEROMETER vector (after the per-axis remap, the same `accel_board`
 * value `ff_geo_heading_deg` was handed) from the MOST RECENT
 * `ff_compass_read()` call — the small accessor S31's own spec asked
 * for ("ff_compass for the IMU accel access... add a small accessor if
 * there is none"; there was none before this). Units are g (gravity is
 * (0,0,1) when the board sits level) — the Music face's beat detector
 * (`firmware/core/ff_beat.h`) derives a vertical-axis bounce magnitude
 * from this for its IMU fallback path.
 *
 * Same "most recent, not a fresh transaction" contract as
 * `ff_compass_last_mag_board` above, and the SAME early-return caveat:
 * `ff_compass_read()` returns its honest -1 "unknown" sentinel (and
 * touches neither this nor the mag accessor) whenever NO magnetometer
 * is present at all, because a full sample today reads mag+accel
 * together for the tilt-compensated heading computation — there is no
 * IMU-only read path. **Known limitation, not fixed by this PR**: on a
 * puck with the onboard QMI8658 IMU present but no GY-273 magnetometer
 * wired (the magnetometer is an aftermarket add-on — this header's own
 * top comment), this accessor never updates at all, even though the
 * IMU itself is healthy — Music's IMU fallback is unavailable on such a
 * puck today (it honestly reports NO SOURCE instead, never a fabricated
 * bounce). Splitting `ff_compass_read()` into independent mag-only/
 * IMU-only sampling would be the real fix; out of scope here (S31 is
 * additive, not a rework of S15's read path) — see
 * docs/specs/S31-music-swarm.md's own "Questions" section.
 */
ff_vec3_t ff_compass_last_accel_board(void);

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
