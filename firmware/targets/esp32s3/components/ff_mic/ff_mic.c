/**
 * ff_mic.c — I2S1 RX HAL for the onboard mic. See ff_mic.h's top comment
 * for the full design rationale (wiring, power policy, honesty
 * contract); this file's own comments below cover the reader task, the
 * sample format conversion, and the sentinel/stuck-data check.
 *
 * ## Sample format — 32-bit words, top 16 bits kept
 * Waveshare's own reference firmware (`MIC_Speech.h`/`.c`) configures
 * this mic as a 24-bit-in-32-bit device: each I2S word's useful audio
 * lives in the TOP 24 bits, with the bottom 8 bits don't-care padding.
 * This driver takes the simpler of the two documented options
 * (docs/specs/S30-audio-input.md, "Format"): right-shift each signed
 * 32-bit word by 16 (not 8), keeping the top 16 bits as a plain int16
 * sample — 8 bits less precision than the full 24-bit value, but a
 * format every consumer downstream (ff_miclevel.h's dBFS convention,
 * `ff_mic_last_frame`'s int16 output, any future int16-PCM consumer)
 * already expects with zero extra conversion. A `>>16` on a two's-
 * complement `int32_t` is an arithmetic (sign-preserving) shift on
 * every toolchain this project targets (xtensa-esp32s3-elf gcc; -std=
 * gnu11's implementation-defined-but-universally-arithmetic behavior
 * for a negative left operand, the same assumption this codebase's
 * other bit-twiddling already makes without comment elsewhere).
 *
 * ## Reader task — always looping, always bounded, always feeding TWDT
 * ONE task, created once in `ff_mic_init`, that NEVER blocks
 * indefinitely: while idle (not started, or stopped), it waits on a
 * binary semaphore with a bounded timeout
 * (`FF_MIC_IDLE_POLL_TIMEOUT_MS`); while running, it calls
 * `i2s_channel_read` with a bounded timeout
 * (`FF_MIC_READ_TIMEOUT_MS`) — this HAL contains no `portMAX_DELAY`
 * anywhere, per this project's task-watchdog policy (2026-09-08 QA
 * hardening, PR #239: every task either resets the watchdog on a bound
 * loop or is not subscribed at all). This task subscribes itself
 * (`esp_task_wdt_add(NULL)`) once at startup and resets on EVERY loop
 * iteration, idle or running — the global TWDT policy app_main.c's own
 * `ff_configure_task_watchdog` sets (`trigger_panic = false`) is
 * log-only, so a genuinely wedged read here is surfaced, never a silent
 * reboot.
 *
 * ## Sentinel / stuck-data check (mirrors ff_compass.h's own
 * IMU_NO_DATA discipline, cited in ff_mic.h's top comment)
 * A frame is "stuck" if every one of its raw (pre-DC-removal) 32-bit
 * samples is either exactly 0, OR byte-identical to the previous
 * frame's corresponding sample — a MEMS mic that never actually started
 * clocking real data out (I2S peripheral itself reports a clean, non-
 * error read; the DATA line is just not toggling) produces exactly this
 * shape. `s_stuck_ms` accumulates the ACTUAL elapsed time (not an
 * assumed 20ms) between stuck frames; once it reaches
 * `FF_MIC_STUCK_PRESENT_FALSE_MS` (1000ms), `present` goes false —
 * logged once per transition, not per frame. A single real (non-stuck)
 * frame resets the counter to 0 and, if `present` had gone false,
 * brings it back true — self-healing, since nothing about this driver's
 * own state needs a hardware re-bring-up to recover (unlike
 * ff_compass's QMI8658, which DOES re-run its own init sequence on this
 * same shape of failure — this mic has no equivalent re-init step to
 * retry; the I2S peripheral itself never reported an error, so there is
 * nothing to re-configure).
 */
#include "ff_mic.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ff_miclevel.h" /* firmware/core — pure level math, shared with the host test */

static const char *TAG = "ff_mic";

/* ---- Hardware wiring (Waveshare ESP32-S3-Touch-LCD-1.46 reference
 * MIC_Speech.h/.c) — see ff_mic.h's top comment. No MCLK, no DOUT (RX-
 * only — symmetric with ff_audio's TX-only I2S_GPIO_UNUSED usage). ---- */
#define FF_MIC_I2S_PORT   I2S_NUM_1 /* NOT I2S_NUM_0 — that is ff_audio's speaker TX channel */
#define FF_MIC_BCLK_GPIO  GPIO_NUM_15
#define FF_MIC_WS_GPIO    GPIO_NUM_2
#define FF_MIC_DIN_GPIO   GPIO_NUM_39

/* Bounded timeouts — see this file's top comment, "Reader task". Never
 * portMAX_DELAY anywhere in this HAL. */
#define FF_MIC_READ_TIMEOUT_MS       100u /* generous vs. one 20ms frame period — a slow/late read still completes */
#define FF_MIC_IDLE_POLL_TIMEOUT_MS  200u

/* See ff_mic.c's top comment, "Sentinel / stuck-data check". */
#define FF_MIC_STUCK_PRESENT_FALSE_MS 1000u

#define FF_MIC_TASK_STACK_BYTES 4096 /* matches ff_audio's own task — see this file's PR body for the "not yet
                                         measured on real hardware" caveat; verify via `perf` once flashed */
#define FF_MIC_TASK_PRIORITY    5

/* ---------------------------------------------------------------------
 * State. `s_chan`/`s_task` are touched by app_main's own task only
 * during ff_mic_init (before the reader task's first loop iteration)
 * and by the reader task only afterward — no lock needed for those two.
 * Everything ELSE below (status/level snapshot + the frame copy-out
 * buffer) is written by the reader task and read by up to two OTHER
 * tasks in practice — the render-loop task (DIAGNOSTICS' 2s push,
 * app_main.c) and whichever task the bench console's `mic`/`mic watch`
 * hook runs on (also the render-loop task in this codebase's own
 * wiring, but this HAL makes no assumption about that) — so every
 * access to the fields below this comment goes through `s_lock`, a real
 * mutex (never touched from an ISR, so a blocking mutex is safe — same
 * reasoning ff_audio.c's own state-lock comment gives).
 * ------------------------------------------------------------------- */
static i2s_chan_handle_t s_chan = NULL;
static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_start_sem = NULL; /* binary — reader task waits on this while idle */
static SemaphoreHandle_t s_lock = NULL;      /* mutex guarding everything below */

static bool s_initialized = false;
static bool s_init_failed_logged = false;

static volatile bool s_want_running = false; /* set by ff_mic_start/ff_mic_stop; read by the reader task each loop */

static bool s_present = false;
static bool s_running = false;
static uint32_t s_frames_read = 0;
static uint32_t s_read_errors = 0;
static uint32_t s_last_read_ms = 0; /* esp_timer_get_time()/1000 at the last completed read; 0 = never */

static ff_mic_level_t s_level; /* rms/peak/envelope dBFS, all FF_MICLEVEL_FLOOR_DBFS until the first frame */
static int16_t s_last_frame[FF_MIC_FRAME_SAMPLES];
static bool s_has_last_frame = false;

static ff_miclevel_dc_state_t s_dc;
static ff_miclevel_envelope_t s_env;

static bool s_stuck_logged = false;
static uint32_t s_stuck_ms = 0;

/* fix/s31-music-idle-drain (2026-09-09) — cumulative on-time tracking;
 * see ff_mic_status_t.total_on_ms's own doc comment (ff_mic.h) for what
 * this answers and why. `s_total_on_ms` is the sum of every COMPLETED
 * on-stretch (committed at ff_mic_stop); `s_on_since_ms` is the start
 * timestamp of the CURRENT stretch, meaningful only while s_want_running
 * — ff_mic_status() adds the live in-progress stretch to s_total_on_ms
 * on every call so the reported total never lags behind "right now" by
 * up to a whole on-stretch. */
static uint32_t s_total_on_ms = 0;
static uint32_t s_on_since_ms = 0;

/* Reader-task-exclusive scratch — file-scope static so it never lands on
 * the task's own (deliberately small) stack, same reasoning ff_audio.c's
 * s_chunk_buf comment gives. */
static int32_t s_raw_buf[FF_MIC_FRAME_SAMPLES];
static int32_t s_prev_raw_buf[FF_MIC_FRAME_SAMPLES];
static bool s_have_prev_raw = false;
static float s_float_buf[FF_MIC_FRAME_SAMPLES];

static void ff_mic_lock(void) { (void)xSemaphoreTake(s_lock, portMAX_DELAY); }
static void ff_mic_unlock(void) { (void)xSemaphoreGive(s_lock); }

static uint32_t ff_mic_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* True iff every sample in `buf` is exactly 0. */
static bool ff_mic_frame_is_all_zero(int32_t const *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (buf[i] != 0) return false;
    }
    return true;
}

static esp_err_t ff_mic_alloc_and_init_channel(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(FF_MIC_I2S_PORT, I2S_ROLE_MASTER);
    i2s_chan_handle_t rx_chan = NULL;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_chan); /* NULL tx, real rx — RX-only channel */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(FF_MIC_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, /* not connected — this mic has no MCLK input */
            .bclk = FF_MIC_BCLK_GPIO,
            .ws = FF_MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED, /* RX-only — nothing to transmit */
            .din = FF_MIC_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = 0,
                .bclk_inv = 0,
                .ws_inv = 0,
            },
        },
    };
    /* This board's mic answers on the RIGHT I2S slot — see ff_mic.h's
     * top comment ("Hardware"). */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;

    err = i2s_channel_init_std_mode(rx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        (void)i2s_del_channel(rx_chan);
        return err;
    }

    s_chan = rx_chan;
    return ESP_OK;
}

/* Process one successfully-read frame (s_raw_buf, FF_MIC_FRAME_SAMPLES
 * valid int32 words): sentinel check, DC removal, RMS/peak/envelope,
 * publish the snapshot under s_lock. Reader-task-only — no lock needed
 * for the s_raw_buf/s_prev_raw_buf/s_float_buf scratch. */
static void ff_mic_process_frame(uint32_t now_ms, uint32_t dt_ms)
{
    bool const stuck = s_have_prev_raw &&
                        (ff_mic_frame_is_all_zero(s_raw_buf, FF_MIC_FRAME_SAMPLES) ||
                         memcmp(s_raw_buf, s_prev_raw_buf, sizeof(s_raw_buf)) == 0);
    bool const all_zero_first_frame = !s_have_prev_raw && ff_mic_frame_is_all_zero(s_raw_buf, FF_MIC_FRAME_SAMPLES);

    for (size_t i = 0; i < FF_MIC_FRAME_SAMPLES; i++) {
        int16_t const sample16 = (int16_t)(s_raw_buf[i] >> 16);
        float const dc_removed = ff_miclevel_dc_remove(&s_dc, (float)sample16);
        s_float_buf[i] = dc_removed;
    }

    ff_miclevel_frame_t frame;
    ff_miclevel_frame_compute(s_float_buf, FF_MIC_FRAME_SAMPLES, &frame);
    ff_miclevel_envelope_update(&s_env, frame.rms_dbfs, dt_ms);

    ff_mic_lock();
    s_frames_read++;
    s_last_read_ms = now_ms;
    s_level.rms_dbfs = frame.rms_dbfs;
    s_level.peak_dbfs = frame.peak_dbfs;
    s_level.envelope_dbfs = s_env.value_dbfs;
    for (size_t i = 0; i < FF_MIC_FRAME_SAMPLES; i++) {
        s_last_frame[i] = (int16_t)s_float_buf[i];
    }
    s_has_last_frame = true;

    if (stuck || all_zero_first_frame) {
        s_stuck_ms += dt_ms;
        if (s_stuck_ms >= FF_MIC_STUCK_PRESENT_FALSE_MS) {
            if (s_present && !s_stuck_logged) {
                ESP_LOGW(TAG, "no-data for %ums straight — present=false (stuck/all-zero I2S1 read)",
                         (unsigned)s_stuck_ms);
                s_stuck_logged = true;
            }
            s_present = false;
        }
    } else {
        if (!s_present) {
            ESP_LOGI(TAG, "real data resumed — present=true");
        }
        s_stuck_ms = 0;
        s_stuck_logged = false;
        s_present = true;
    }
    ff_mic_unlock();

    memcpy(s_prev_raw_buf, s_raw_buf, sizeof(s_raw_buf));
    s_have_prev_raw = true;
}

static void ff_mic_task_fn(void *arg)
{
    (void)arg;

    esp_err_t const wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_task_wdt_add failed: %s — this task is not covered by the watchdog", esp_err_to_name(wdt_err));
    }

    uint32_t last_frame_ms = 0;
    bool have_last_frame_ms = false;

    for (;;) {
        esp_task_wdt_reset(); /* every loop iteration, idle or running — see this file's top comment */

        bool running_now;
        ff_mic_lock();
        running_now = s_want_running;
        ff_mic_unlock();

        if (!running_now) {
            (void)xSemaphoreTake(s_start_sem, pdMS_TO_TICKS(FF_MIC_IDLE_POLL_TIMEOUT_MS));
            continue;
        }

        size_t bytes_read = 0;
        /* `i2s_channel_read`'s `timeout_ms` is literally milliseconds
         * (esp_driver_i2s/include/driver/i2s_common.h's own doc comment)
         * — pass FF_MIC_READ_TIMEOUT_MS directly, NOT pdMS_TO_TICKS-
         * wrapped (that would silently shrink a 100ms budget to ~20ms
         * at this project's CONFIG_FREERTOS_HZ=100). */
        esp_err_t const err =
            i2s_channel_read(s_chan, s_raw_buf, sizeof(s_raw_buf), &bytes_read, FF_MIC_READ_TIMEOUT_MS);
        uint32_t const now_ms = ff_mic_now_ms();

        if (err != ESP_OK || bytes_read != sizeof(s_raw_buf)) {
            ff_mic_lock();
            s_read_errors++;
            ff_mic_unlock();
            have_last_frame_ms = false; /* a dropped frame breaks the dt_ms chain — don't compute a bogus gap */
            continue;
        }

        uint32_t const dt_ms = have_last_frame_ms ? (now_ms - last_frame_ms) : (FF_MIC_FRAME_SAMPLES * 1000u) /
                                                                                    FF_MIC_SAMPLE_RATE_HZ;
        ff_mic_process_frame(now_ms, dt_ms);
        last_frame_ms = now_ms;
        have_last_frame_ms = true;
    }
}

esp_err_t ff_mic_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = ff_mic_alloc_and_init_channel();
    if (err != ESP_OK) {
        if (!s_init_failed_logged) {
            ESP_LOGE(TAG, "ff_mic_init failed (%s) — continuing bring-up with mic disabled", esp_err_to_name(err));
            s_init_failed_logged = true;
        }
        return err; /* caller (app_main) treats this as non-fatal */
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "state mutex allocation failed");
        (void)i2s_del_channel(s_chan);
        s_chan = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_start_sem = xSemaphoreCreateBinary();
    if (s_start_sem == NULL) {
        ESP_LOGE(TAG, "start semaphore allocation failed");
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        (void)i2s_del_channel(s_chan);
        s_chan = NULL;
        return ESP_ERR_NO_MEM;
    }

    memset(&s_level, 0, sizeof(s_level));
    s_level.rms_dbfs = FF_MICLEVEL_FLOOR_DBFS;
    s_level.peak_dbfs = FF_MICLEVEL_FLOOR_DBFS;
    s_level.envelope_dbfs = FF_MICLEVEL_FLOOR_DBFS;
    ff_miclevel_dc_reset(&s_dc);
    ff_miclevel_envelope_reset(&s_env);

    BaseType_t const rc =
        xTaskCreate(ff_mic_task_fn, "ff_mic", FF_MIC_TASK_STACK_BYTES, NULL, FF_MIC_TASK_PRIORITY, &s_task);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "reader task creation failed");
        vSemaphoreDelete(s_start_sem);
        s_start_sem = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        (void)i2s_del_channel(s_chan);
        s_chan = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    /* Off by default — see ff_mic.h's top comment, "Power policy". The
     * channel stays disabled and the task stays idle until ff_mic_start(). */
    ESP_LOGI(TAG, "I2S1 RX up (16kHz mono, BCLK=GPIO15 WS=GPIO2 DIN=GPIO39), reader task idle (off by default)");
    return ESP_OK;
}

void ff_mic_start(void)
{
    if (!s_initialized) return;

    ff_mic_lock();
    if (s_want_running) {
        ff_mic_unlock();
        return; /* already running — no-op */
    }
    s_want_running = true;
    s_present = true; /* optimistic until proven stuck — see this file's top comment */
    s_running = true;
    s_stuck_ms = 0;
    s_stuck_logged = false;
    s_has_last_frame = false;
    s_level.rms_dbfs = FF_MICLEVEL_FLOOR_DBFS;
    s_level.peak_dbfs = FF_MICLEVEL_FLOOR_DBFS;
    s_level.envelope_dbfs = FF_MICLEVEL_FLOOR_DBFS;
    ff_miclevel_dc_reset(&s_dc);
    ff_miclevel_envelope_reset(&s_env);
    s_have_prev_raw = false;
    ff_mic_unlock();

    esp_err_t const err = i2s_channel_enable(s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s — mic did not actually start", esp_err_to_name(err));
        ff_mic_lock();
        s_want_running = false;
        s_present = false;
        s_running = false;
        ff_mic_unlock();
        return;
    }

    ff_mic_lock();
    s_on_since_ms = ff_mic_now_ms(); /* fix/s31-music-idle-drain — this stretch's own start, for total_on_ms */
    ff_mic_unlock();

    (void)xSemaphoreGive(s_start_sem); /* wake the reader task out of its idle poll immediately */
    ESP_LOGI(TAG, "mic started (total on-time %us)", (unsigned)(s_total_on_ms / 1000u));
}

void ff_mic_stop(void)
{
    if (!s_initialized) return;

    ff_mic_lock();
    bool const was_running = s_want_running;
    s_want_running = false;
    s_running = false;
    if (was_running) {
        /* fix/s31-music-idle-drain — commit this completed stretch into
         * the cumulative total; see ff_mic_status_t.total_on_ms's own
         * doc comment for why this (unlike frames_read/read_errors) is
         * never reset. Wraparound-safe unsigned subtraction over any
         * realistic on-stretch length, same convention ff_clock.h's
         * ff_time_reached documents for every other now-vs-past
         * millisecond delta in this codebase. */
        s_total_on_ms += (ff_mic_now_ms() - s_on_since_ms);
    }
    ff_mic_unlock();

    if (!was_running) return;

    (void)i2s_channel_disable(s_chan);
    ESP_LOGI(TAG, "mic stopped (total on-time %us)", (unsigned)(s_total_on_ms / 1000u));
}

ff_mic_status_t ff_mic_status(void)
{
    ff_mic_status_t st;
    memset(&st, 0, sizeof(st));
    if (!s_initialized) return st;

    ff_mic_lock();
    st.present = s_present;
    st.running = s_running;
    st.sample_rate_hz = s_present ? FF_MIC_SAMPLE_RATE_HZ : 0u;
    st.frames_read = s_frames_read;
    st.read_errors = s_read_errors;
    st.last_read_age_ms = (s_running && s_last_read_ms != 0u) ? (ff_mic_now_ms() - s_last_read_ms) : 0u;
    /* fix/s31-music-idle-drain — the committed total plus whatever has
     * elapsed of the CURRENT stretch so far, so a caller reading this
     * mid-stretch (the common case: the mic is running right now) never
     * sees a total that lags "right now" by up to one whole on-stretch. */
    st.total_on_ms = s_total_on_ms + (s_want_running ? (ff_mic_now_ms() - s_on_since_ms) : 0u);
    ff_mic_unlock();
    return st;
}

ff_mic_level_t ff_mic_level(void)
{
    ff_mic_level_t out = {FF_MICLEVEL_FLOOR_DBFS, FF_MICLEVEL_FLOOR_DBFS, FF_MICLEVEL_FLOOR_DBFS};
    if (!s_initialized) return out;

    ff_mic_lock();
    out = s_level;
    ff_mic_unlock();
    return out;
}

size_t ff_mic_last_frame(int16_t *dst, size_t n)
{
    if (dst == NULL || n == 0u || !s_initialized) return 0u;

    ff_mic_lock();
    if (!s_has_last_frame) {
        ff_mic_unlock();
        return 0u;
    }
    size_t const copy_n = (n < FF_MIC_FRAME_SAMPLES) ? n : FF_MIC_FRAME_SAMPLES;
    memcpy(dst, s_last_frame, copy_n * sizeof(int16_t));
    ff_mic_unlock();
    return copy_n;
}
