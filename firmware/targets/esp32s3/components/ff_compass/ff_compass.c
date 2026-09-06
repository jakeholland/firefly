/**
 * ff_compass.c — S15: GY-273 magnetometer + onboard QMI8658 accel,
 * tilt-compensated heading via ff_geo_heading_deg. See ff_compass.h for
 * the full hardware contract, the honesty contract, and the
 * calibration seam.
 */
#include "ff_compass.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ff_compass";

/* I2C transaction timeout for every read/write below. This runs in the
 * main render-loop task (app_main.c's FF_COMPASS_SAMPLE_PERIOD_MS tick,
 * 10 Hz) on the SAME i2c_master_bus_handle_t the SPD2010 touch driver
 * uses (ff_display_i2c_bus()) — a transaction that blocks here also
 * blocks touch reads on that shared handle. A successful transaction
 * at 400 kHz for a handful of bytes completes in well under 1 ms, so
 * there is no happy-path cost to keeping this short; a genuinely
 * wedged bus degrades to the -1 "unknown" sentinel quickly instead of
 * costing up to 2x this driver's own sample period (and stalling
 * touch along with it) on every affected tick. PR #212 review finding
 * #1: was 100 ms. */
#define FF_COMPASS_I2C_TIMEOUT_MS 20

/* Per-device SCL ceiling handed to i2c_master_bus_add_device — a cap on
 * THIS device's own transactions, not a bus reconfigure (the bus itself
 * was already brought up by ff_display_expander_init at its own
 * FF_I2C_HZ, 400000). All three candidate chips here are rated for
 * standard/fast-mode I2C well above this. */
#define FF_COMPASS_I2C_HZ 400000u

/* =====================================================================
 * QMI8658 (onboard IMU) — register map + bring-up bytes, cited from
 * Waveshare's own ESP-IDF 5.3.2 reference demo for this exact board:
 * https://github.com/yaosy1997/ESP32-S3-Touch-LCD-1.46-Test/blob/main/main/QMI8658/QMI8658.c
 * and QMI8658.h (register addresses, WHO_AM_I value, and the bit-layout
 * #defines the CTRL2 byte below is computed from).
 * ===================================================================== */
#define FF_QMI8658_ADDR_PRIMARY 0x6B
#define FF_QMI8658_ADDR_ALT 0x6A

#define FF_QMI8658_REG_WHO_AM_I 0x00
#define FF_QMI8658_WHO_AM_I_VAL 0x05
#define FF_QMI8658_REG_CTRL1 0x02
#define FF_QMI8658_REG_CTRL2 0x03
#define FF_QMI8658_REG_CTRL7 0x08
#define FF_QMI8658_REG_AX_L 0x35 /* 6-byte burst: AX_L,AX_H,AY_L,AY_H,AZ_L,AZ_H */

/* CTRL1 bit6 = address auto-increment, needed for the 6-byte AX_L..AZ_H
 * burst read below. Cited verbatim from the reference driver's own
 * init: `ctrl1 |= 0x40`. */
#define FF_QMI8658_CTRL1_VAL 0x40

/* CTRL2 = (ASCALE << QMI8658_ASCALE_OFFSET) | AODR, per the reference
 * header's own bit layout (QMI8658_ASCALE_OFFSET=4, ACC_RANGE_4G=0x1,
 * acc_odr_norm_30=0x8): (0x1 << 4) | 0x8 = 0x18. 30 Hz comfortably
 * covers this driver's own 10 Hz poll rate (app_main.c's
 * FF_COMPASS_SAMPLE_PERIOD_MS) while staying in the chip's "normal"
 * (not low-power) accel mode family. */
#define FF_QMI8658_CTRL2_VAL 0x18

/* CTRL7 = aEN | sys_hs. The reference driver writes 0x43 (aEN(bit0) |
 * gEN(bit1) | sys_hs(bit6)) because its own demo also runs the gyro;
 * ff_geo_heading_deg takes mag + accel only, so this driver has no use
 * for gyro data and leaves gEN off. bit6 (sys_hs, "high-speed internal
 * clock" vs. an ODR-derived clock per QST's own datasheet) is NOT
 * gyro-related, so it is kept rather than dropped along with gEN —
 * matching the reference driver's own clock source rather than
 * introducing an unverified deviation on a path this PR's own bench
 * note already flags as not independently verified (PR #212 review
 * finding #3). 0x43 & ~0x02 (gEN) = 0x41. */
#define FF_QMI8658_CTRL7_VAL 0x41

/* =====================================================================
 * QMC5883L — the far more common chip on a GY-273 board even when
 * silkscreened "HMC5883L". Register map + CTRL1 byte per the QMC5883L
 * datasheet ("Register 09H — Control Register 1", "Register 0BH —
 * SET/RESET Period", "Register 0DH — Chip ID").
 * ===================================================================== */
#define FF_QMC5883L_ADDR 0x0D

#define FF_QMC5883L_REG_DATA 0x00 /* 6-byte burst: XL,XH,YL,YH,ZL,ZH */
#define FF_QMC5883L_REG_SETRESET 0x0B
#define FF_QMC5883L_REG_CTRL1 0x09
#define FF_QMC5883L_REG_CHIPID 0x0D

#define FF_QMC5883L_CHIPID_VAL 0xFF
#define FF_QMC5883L_SETRESET_VAL 0x01 /* datasheet's own fixed recommended value */

/* CTRL1 (0x09) field layout, per the datasheet: OSR[7:6] | RNG[5:4] |
 * ODR[3:2] | MODE[1:0]. OSR=00 (512), RNG=00 (2 Gauss), ODR=00 (10 Hz),
 * MODE=01 (continuous) -> 0b00000001. */
#define FF_QMC5883L_CTRL1_VAL 0x01

/* =====================================================================
 * HMC5883L — the genuine part some GY-273 boards actually carry.
 * Register map + CRA/CRB/mode bytes per the HMC5883L datasheet
 * ("Configuration Register A/B", "Mode Register", "Identification
 * Register A/B/C", "Data Output X/Y/Z Registers").
 * ===================================================================== */
#define FF_HMC5883L_ADDR 0x1E

#define FF_HMC5883L_REG_CRA 0x00
#define FF_HMC5883L_REG_CRB 0x01
#define FF_HMC5883L_REG_MODE 0x02
#define FF_HMC5883L_REG_DATA 0x03 /* X_MSB,X_LSB,Z_MSB,Z_LSB,Y_MSB,Y_LSB — datasheet's own X,Z,Y order, NOT X,Y,Z */
#define FF_HMC5883L_REG_ID_A 0x0A /* ID A/B/C read back ASCII "H43" */

#define FF_HMC5883L_CRA_VAL 0x70  /* 8-sample average, 15 Hz ODR, normal measurement — datasheet default */
#define FF_HMC5883L_CRB_VAL 0x20  /* GN2:0=001, +-1.3 Ga, 1090 LSB/Gauss -- true POR default (PR #212 review: was 0xA0/+-4.7 Ga/390 LSB/Gauss, which is GN2:0=101, not the default, and throws away resolution Earth's field (~0.25-0.65 Ga) doesn't need) */
#define FF_HMC5883L_MODE_VAL 0x00 /* continuous-measurement mode */

/* =====================================================================
 * Axis mapping: sensor frame -> board frame (ff_geo.h's own convention:
 * +x right, +y forward/"top of puck", +z up out of the screen;
 * stationary level accel reads ~(0,0,+1g)). ONE table, two small
 * per-axis (source, sign) pairs below — flip a _SIGN or swap a _SRC
 * here to correct after a bench check; nothing else in this file
 * encodes either mapping.
 *
 * Magnetometer (GY-273): ASSUMED mounting, unverified on real hardware
 * — the module sits flat against the case wall at the lanyard end,
 * silkscreen component-side facing the same way as the main board's
 * (both toward the glass), with the module's printed +X arrow pointing
 * toward the puck's own +y (away from the lanyard, toward the top of
 * the puck as worn) and its +Y arrow toward the puck's own -x. That
 * mounting is a 90-degree rotation about the shared +z axis: mag +x ->
 * board +y, mag +y -> board -x, mag +z -> board +z (both boards'
 * components face the glass, so no z-flip). VERIFY ON BENCH — if the
 * Radar arrow turns the wrong way (or 90/180 degrees off) when the
 * puck is rotated flat, this is the first place to look; see this
 * driver's introducing PR body for the bench procedure.
 *
 * IMU (onboard QMI8658): identity, on the working assumption that the
 * IMU's silkscreen axes already match the board's own (it is soldered
 * to the SAME rigid PCB as the screen, not a separately-mounted
 * aftermarket module) — NOT independently verified against
 * Waveshare's schematic. If tilt-compensation looks inverted (heading
 * flips when the puck is tipped rather than staying put), check this
 * table before the magnetometer one above.
 */
typedef enum { FF_AXIS_X = 0, FF_AXIS_Y = 1, FF_AXIS_Z = 2 } ff_compass_axis_t;

#define FF_MAG_BOARD_X_SRC FF_AXIS_Y
#define FF_MAG_BOARD_X_SIGN ((int8_t)-1)
#define FF_MAG_BOARD_Y_SRC FF_AXIS_X
#define FF_MAG_BOARD_Y_SIGN ((int8_t)1)
#define FF_MAG_BOARD_Z_SRC FF_AXIS_Z
#define FF_MAG_BOARD_Z_SIGN ((int8_t)1)

#define FF_IMU_BOARD_X_SRC FF_AXIS_X
#define FF_IMU_BOARD_X_SIGN ((int8_t)1)
#define FF_IMU_BOARD_Y_SRC FF_AXIS_Y
#define FF_IMU_BOARD_Y_SIGN ((int8_t)1)
#define FF_IMU_BOARD_Z_SRC FF_AXIS_Z
#define FF_IMU_BOARD_Z_SIGN ((int8_t)1)

static float ff_compass_axis_get(ff_vec3_t v, ff_compass_axis_t a)
{
    switch (a) {
    case FF_AXIS_X: return v.x;
    case FF_AXIS_Y: return v.y;
    case FF_AXIS_Z: return v.z;
    }
    return 0.0f; /* unreachable for a value from this file's own enum, but -Werror wants a return on every path */
}

static ff_vec3_t ff_compass_remap(ff_vec3_t raw, ff_compass_axis_t xs, int8_t xsign, ff_compass_axis_t ys,
                                   int8_t ysign, ff_compass_axis_t zs, int8_t zsign)
{
    ff_vec3_t out;
    out.x = (float)xsign * ff_compass_axis_get(raw, xs);
    out.y = (float)ysign * ff_compass_axis_get(raw, ys);
    out.z = (float)zsign * ff_compass_axis_get(raw, zs);
    return out;
}

/* =====================================================================
 * State — module-private, mirroring ff_power.c's own file-scope-static
 * discipline for HAL handles.
 * ===================================================================== */
static i2c_master_dev_handle_t s_mag_dev;
static i2c_master_dev_handle_t s_imu_dev;
static ff_compass_mag_kind_t s_mag_kind = FF_COMPASS_MAG_NONE;
static bool s_imu_present;

static ff_geo_cal_t s_cal;
static bool s_cal_valid;

/* Last heading `ff_compass_read()` returned, for `ff_compass_status()`'s
 * one-shot diagnostic snapshot (ff_compass.h). Explicitly -1 ("unknown"
 * — the SAME sentinel `ff_compass_read()` itself uses), not the
 * zero-init a plain `static float` would give: 0 deg is a real,
 * fabricated-looking heading (due north) and would be dishonest as a
 * "nothing has been read yet" default. */
static float s_last_heading_deg = -1.0f;

/* ff_compass_read() runs at 10 Hz (app_main.c's own
 * FF_COMPASS_SAMPLE_PERIOD_MS) from the main render-loop task — a bus
 * fault (NACK/timeout) on that path can repeat every tick for as long
 * as the fault persists. Log the first occurrence of each failure kind
 * so it isn't silent, then go quiet rather than spamming the log at
 * 10 Hz for the rest of the session (PR #212 review: gate on this). */
static bool s_mag_read_warned;
static bool s_imu_read_warned;

/* =====================================================================
 * Small I2C helpers shared by every probe/bring-up/read below.
 * ===================================================================== */
static esp_err_t ff_compass_add_dev(i2c_master_bus_handle_t bus, uint16_t addr, i2c_master_dev_handle_t *out)
{
    i2c_device_config_t const cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = FF_COMPASS_I2C_HZ,
    };
    return i2c_master_bus_add_device(bus, &cfg, out);
}

static esp_err_t ff_compass_reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t const buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf), FF_COMPASS_I2C_TIMEOUT_MS);
}

static esp_err_t ff_compass_reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(dev, &reg, 1, out, n, FF_COMPASS_I2C_TIMEOUT_MS);
}

/* =====================================================================
 * Probe + bring-up: onboard QMI8658 IMU.
 * ===================================================================== */
static void ff_compass_probe_imu(i2c_master_bus_handle_t bus)
{
    uint16_t const addrs[2] = {FF_QMI8658_ADDR_PRIMARY, FF_QMI8658_ADDR_ALT};

    for (size_t i = 0; i < 2; i++) {
        i2c_master_dev_handle_t dev = NULL;
        if (ff_compass_add_dev(bus, addrs[i], &dev) != ESP_OK) {
            continue;
        }

        uint8_t who = 0;
        esp_err_t err = ff_compass_reg_read(dev, FF_QMI8658_REG_WHO_AM_I, &who, 1);
        if (err == ESP_OK && who == FF_QMI8658_WHO_AM_I_VAL) {
            esp_err_t const c1 = ff_compass_reg_write(dev, FF_QMI8658_REG_CTRL1, FF_QMI8658_CTRL1_VAL);
            esp_err_t const c2 = ff_compass_reg_write(dev, FF_QMI8658_REG_CTRL2, FF_QMI8658_CTRL2_VAL);
            esp_err_t const c7 = ff_compass_reg_write(dev, FF_QMI8658_REG_CTRL7, FF_QMI8658_CTRL7_VAL);
            if (c1 == ESP_OK && c2 == ESP_OK && c7 == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(10)); /* let the accel engine settle before the first real read */
                s_imu_dev = dev;
                s_imu_present = true;
                ESP_LOGI(TAG,
                         "QMI8658 IMU found @0x%02X (WHO_AM_I=0x%02X) — accel up (CTRL2=0x%02X CTRL7=0x%02X)",
                         addrs[i], who, FF_QMI8658_CTRL2_VAL, FF_QMI8658_CTRL7_VAL);
                return;
            }
            ESP_LOGW(TAG, "QMI8658 @0x%02X identified but a bring-up write failed — treating as absent",
                     addrs[i]);
        }
        i2c_master_bus_rm_device(dev);
    }

    ESP_LOGW(TAG, "compass: no IMU — assuming level (tilt compensation unavailable on this path)");
}

/* =====================================================================
 * Probe + bring-up: GY-273 magnetometer (QMC5883L, then HMC5883L).
 * ===================================================================== */
static bool ff_compass_probe_qmc5883l(i2c_master_bus_handle_t bus)
{
    i2c_master_dev_handle_t dev = NULL;
    if (ff_compass_add_dev(bus, FF_QMC5883L_ADDR, &dev) != ESP_OK) {
        return false;
    }

    uint8_t chip_id = 0;
    if (ff_compass_reg_read(dev, FF_QMC5883L_REG_CHIPID, &chip_id, 1) == ESP_OK &&
        chip_id == FF_QMC5883L_CHIPID_VAL) {
        esp_err_t const sr = ff_compass_reg_write(dev, FF_QMC5883L_REG_SETRESET, FF_QMC5883L_SETRESET_VAL);
        esp_err_t const c1 = ff_compass_reg_write(dev, FF_QMC5883L_REG_CTRL1, FF_QMC5883L_CTRL1_VAL);
        if (sr == ESP_OK && c1 == ESP_OK) {
            s_mag_dev = dev;
            s_mag_kind = FF_COMPASS_MAG_QMC5883L;
            ESP_LOGI(TAG,
                     "QMC5883L magnetometer found @0x%02X (chip id 0x%02X) — continuous, 10 Hz, 2G, OSR 512 "
                     "(CTRL1=0x%02X)",
                     FF_QMC5883L_ADDR, chip_id, FF_QMC5883L_CTRL1_VAL);
            return true;
        }
        ESP_LOGW(TAG, "QMC5883L identified but a bring-up write failed — treating as absent");
    }
    i2c_master_bus_rm_device(dev);
    return false;
}

static bool ff_compass_probe_hmc5883l(i2c_master_bus_handle_t bus)
{
    i2c_master_dev_handle_t dev = NULL;
    if (ff_compass_add_dev(bus, FF_HMC5883L_ADDR, &dev) != ESP_OK) {
        return false;
    }

    uint8_t id[3] = {0};
    if (ff_compass_reg_read(dev, FF_HMC5883L_REG_ID_A, id, sizeof(id)) == ESP_OK && id[0] == 'H' &&
        id[1] == '4' && id[2] == '3') {
        esp_err_t const cra = ff_compass_reg_write(dev, FF_HMC5883L_REG_CRA, FF_HMC5883L_CRA_VAL);
        esp_err_t const crb = ff_compass_reg_write(dev, FF_HMC5883L_REG_CRB, FF_HMC5883L_CRB_VAL);
        esp_err_t const mode = ff_compass_reg_write(dev, FF_HMC5883L_REG_MODE, FF_HMC5883L_MODE_VAL);
        if (cra == ESP_OK && crb == ESP_OK && mode == ESP_OK) {
            s_mag_dev = dev;
            s_mag_kind = FF_COMPASS_MAG_HMC5883L;
            ESP_LOGI(TAG,
                     "HMC5883L magnetometer found @0x%02X (id \"%c%c%c\") — continuous mode (CRA=0x%02X "
                     "CRB=0x%02X)",
                     FF_HMC5883L_ADDR, id[0], id[1], id[2], FF_HMC5883L_CRA_VAL, FF_HMC5883L_CRB_VAL);
            return true;
        }
        ESP_LOGW(TAG, "HMC5883L identified but a bring-up write failed — treating as absent");
    }
    i2c_master_bus_rm_device(dev);
    return false;
}

static void ff_compass_probe_mag(i2c_master_bus_handle_t bus)
{
    /* QMC5883L first — the far more common chip on a GY-273 board even
     * when silkscreened HMC (see ff_compass.h's top comment). */
    if (ff_compass_probe_qmc5883l(bus)) {
        return;
    }
    if (ff_compass_probe_hmc5883l(bus)) {
        return;
    }

    ESP_LOGW(TAG,
             "compass: no magnetometer found (checked QMC5883L @0x%02X, HMC5883L @0x%02X) — heading will read "
             "unknown (-1) forever",
             FF_QMC5883L_ADDR, FF_HMC5883L_ADDR);
}

/* =====================================================================
 * Public API.
 * ===================================================================== */
esp_err_t ff_compass_init(i2c_master_bus_handle_t bus)
{
    s_mag_dev = NULL;
    s_imu_dev = NULL;
    s_mag_kind = FF_COMPASS_MAG_NONE;
    s_imu_present = false;

    if (bus == NULL) {
        ESP_LOGE(TAG, "ff_compass_init called with a NULL I2C bus handle (ff_display_i2c_bus not up yet?)");
        return ESP_ERR_INVALID_ARG;
    }

    ff_compass_probe_imu(bus);
    ff_compass_probe_mag(bus);

    /* Non-fatal either way — see this function's own doc comment
     * (ff_compass.h) and ff_power_batt_init's matching "log and
     * continue" posture. */
    return ESP_OK;
}

bool ff_compass_present(void)
{
    return s_mag_kind != FF_COMPASS_MAG_NONE;
}

ff_compass_mag_kind_t ff_compass_mag_kind(void)
{
    return s_mag_kind;
}

bool ff_compass_imu_present(void)
{
    return s_imu_present;
}

void ff_compass_set_cal(ff_geo_cal_t const *cal)
{
    if (cal == NULL) {
        s_cal_valid = false;
        return;
    }
    s_cal = *cal;
    s_cal_valid = true;
}

float ff_compass_read(void)
{
    if (s_mag_kind == FF_COMPASS_MAG_NONE || s_mag_dev == NULL) {
        s_last_heading_deg = -1.0f; /* honest "unknown" — no magnetometer, never a fabricated heading */
        return -1.0f;
    }

    ff_vec3_t mag_raw = {0};
    uint8_t buf[6];

    if (s_mag_kind == FF_COMPASS_MAG_QMC5883L) {
        if (ff_compass_reg_read(s_mag_dev, FF_QMC5883L_REG_DATA, buf, sizeof(buf)) != ESP_OK) {
            if (!s_mag_read_warned) {
                s_mag_read_warned = true;
                ESP_LOGW(TAG, "magnetometer read failed (NACK/timeout) — heading reports -1 until it recovers "
                              "(logged once)");
            }
            s_last_heading_deg = -1.0f; /* -1 sentinel, never a stale heading */
            return -1.0f;
        }
        mag_raw.x = (float)(int16_t)((buf[1] << 8) | buf[0]);
        mag_raw.y = (float)(int16_t)((buf[3] << 8) | buf[2]);
        mag_raw.z = (float)(int16_t)((buf[5] << 8) | buf[4]);
    } else { /* FF_COMPASS_MAG_HMC5883L */
        if (ff_compass_reg_read(s_mag_dev, FF_HMC5883L_REG_DATA, buf, sizeof(buf)) != ESP_OK) {
            if (!s_mag_read_warned) {
                s_mag_read_warned = true;
                ESP_LOGW(TAG, "magnetometer read failed (NACK/timeout) — heading reports -1 until it recovers "
                              "(logged once)");
            }
            s_last_heading_deg = -1.0f; /* -1 sentinel, never a stale heading */
            return -1.0f;
        }
        /* HMC5883L's own data order is X, Z, Y (not X,Y,Z), big-endian
         * per axis — see this file's FF_HMC5883L_REG_DATA comment. */
        mag_raw.x = (float)(int16_t)((buf[0] << 8) | buf[1]);
        mag_raw.z = (float)(int16_t)((buf[2] << 8) | buf[3]);
        mag_raw.y = (float)(int16_t)((buf[4] << 8) | buf[5]);
    }

    ff_vec3_t accel_board;
    if (s_imu_present && s_imu_dev != NULL) {
        uint8_t abuf[6];
        if (ff_compass_reg_read(s_imu_dev, FF_QMI8658_REG_AX_L, abuf, sizeof(abuf)) == ESP_OK) {
            ff_vec3_t const accel_raw = {
                .x = (float)(int16_t)((abuf[1] << 8) | abuf[0]),
                .y = (float)(int16_t)((abuf[3] << 8) | abuf[2]),
                .z = (float)(int16_t)((abuf[5] << 8) | abuf[4]),
            };
            accel_board = ff_compass_remap(accel_raw, FF_IMU_BOARD_X_SRC, FF_IMU_BOARD_X_SIGN, FF_IMU_BOARD_Y_SRC,
                                            FF_IMU_BOARD_Y_SIGN, FF_IMU_BOARD_Z_SRC, FF_IMU_BOARD_Z_SIGN);
        } else {
            /* A transient read failure degrades to level-assumed rather
             * than feeding ff_geo_heading_deg stale/garbage tilt data. */
            if (!s_imu_read_warned) {
                s_imu_read_warned = true;
                ESP_LOGW(TAG, "IMU accel read failed (NACK/timeout) — assuming level this sample (logged once)");
            }
            accel_board = (ff_vec3_t){0.0f, 0.0f, 1.0f};
        }
    } else {
        accel_board = (ff_vec3_t){0.0f, 0.0f, 1.0f}; /* assume level — logged once at init, see ff_compass_probe_imu */
    }

    ff_vec3_t const mag_board = ff_compass_remap(mag_raw, FF_MAG_BOARD_X_SRC, FF_MAG_BOARD_X_SIGN,
                                                  FF_MAG_BOARD_Y_SRC, FF_MAG_BOARD_Y_SIGN, FF_MAG_BOARD_Z_SRC,
                                                  FF_MAG_BOARD_Z_SIGN);

    float const heading = ff_geo_heading_deg(mag_board, accel_board, s_cal_valid ? &s_cal : NULL);
    s_last_heading_deg = heading;
    return heading;
}

ff_compass_status_t ff_compass_status(void)
{
    ff_compass_status_t st = {0};
    st.mag_present = ff_compass_present();
    st.mag_kind = ff_compass_mag_kind();
    st.imu_present = ff_compass_imu_present();
    st.heading_valid = (s_last_heading_deg >= 0.0f);
    st.last_heading_deg = s_last_heading_deg;
    return st;
}
