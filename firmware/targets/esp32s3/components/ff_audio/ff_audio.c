/**
 * ff_audio.c — I2S0 tone-synth HAL for the PCM5101 DAC. See ff_audio.h's
 * top comment for the full design rationale (synthesis, preemption,
 * failure handling, light sleep); this file's own comments below cover
 * the two implementation calls ff_audio.h defers to here: the channel
 * enable/disable strategy and the post-light-sleep re-enable handling.
 *
 * ## Channel enable/disable strategy (interpretation call, no scope used)
 * Chosen: enable the I2S TX channel only while a pattern is actively
 * rendering, `i2s_channel_disable` at pattern end (`FF_AUDIO_KEEP_
 * CHANNEL_ENABLED` below, default 0) — NOT "leave it enabled and write
 * zero-filled silence between patterns". Reasoning, no oscilloscope
 * involved:
 *   - Sounds are short (<=1.5s, `FF_SOUND_PATTERN_MAX_MS`) and infrequent
 *     (six event kinds, none of them continuous) — the vast majority of
 *     this puck's runtime has no sound playing at all, so keeping the
 *     channel (and its DMA, and the I2S peripheral's clock) alive between
 *     patterns would burn power for no benefit essentially all the time.
 *   - `i2s_channel_enable`/`_disable` are register writes + a DMA
 *     start/stop, not a slow bring-up (nothing like `ff_display_panel_
 *     init`'s QSPI init sequence) — paying that cost once per pattern is
 *     cheap relative to how rarely patterns play.
 *   - The pop/click risk a hard enable/disable could cause is exactly
 *     what the 5ms attack / 10ms release envelope (ff_audio.h) already
 *     exists to prevent: every note ends with amplitude ramped to zero
 *     BEFORE `i2s_channel_disable` runs, so there is no discontinuity at
 *     the disable boundary to click on. A cold ENABLE similarly starts
 *     the DMA before any nonzero sample is queued (the attack ramp is the
 *     very first samples written), so there's no discontinuity there
 *     either.
 * `FF_AUDIO_KEEP_CHANNEL_ENABLED` is the tunable if on-glass testing
 * (docs/specs/S27-sounds.md's device on-glass steps) proves this
 * reasoning wrong — flip it to 1 to keep the channel enabled and add a
 * zero-fill idle write loop; nothing else in this file needs to change to
 * try that.
 *
 * ## Light sleep / I2S interaction (ESP-IDF 5.3 read, no scope used)
 * This project runs with `CONFIG_PM_ENABLE` unset (sdkconfig) — no
 * dynamic frequency scaling, no automatic PM-lock-based sleep
 * prevention. `esp_driver_i2s`'s own `esp_pm_lock` guard
 * (`i2s_std.c`, `#ifdef CONFIG_PM_ENABLE`) therefore never engages on
 * this build: nothing in the I2S driver itself stops
 * `esp_light_sleep_start()` (app_main.c, S26f) from being called while a
 * pattern is mid-flight. That is exactly why `ff_audio_busy()` exists and
 * is OR'd into app_main's `sleep_inhibit` (ff_audio.h's own "Light sleep"
 * section) — the busy-based inhibit is the ONLY thing preventing that,
 * not any I2S-driver-side protection.
 *
 * Given that inhibit, the I2S TX channel should in practice NEVER be
 * enabled (per the strategy above, only enabled while a pattern plays,
 * and busy is true for that entire window) at the moment
 * `esp_light_sleep_start()` actually runs — so there should be nothing
 * for a post-wake re-enable to fix. `ff_audio_channel_enable_with_
 * recovery` below is defensive for the race/edge case anyway (e.g. a
 * wake that lands in the handful of instructions between the busy flag
 * being read as false and the channel enable call, or any other gap this
 * reasoning missed): if `i2s_channel_enable` ever fails, this logs ONCE
 * and does a full re-init (delete + recreate the channel from scratch,
 * `ff_audio_alloc_and_init_channel`) rather than trust a half-recovered
 * peripheral state, then retries the enable. If even that fails, the
 * pattern is dropped (logged) and the render task loops back to wait for
 * the next event — a bad speaker moment, never a crashed or wedged
 * device.
 */
#include "ff_audio.h"

#include <math.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "ff_audio";

/* ---- Hardware wiring (Waveshare ESP32-S3-Touch-LCD-1.46 reference
 * Audio_Driver/PCM5101.c) — see ff_audio.h's top comment. MCLK and DIN
 * are not connected on this board; I2S_GPIO_UNUSED marks both. ---- */
#define FF_AUDIO_I2S_PORT       I2S_NUM_0
#define FF_AUDIO_SAMPLE_RATE_HZ 44100u
#define FF_AUDIO_BCLK_GPIO      GPIO_NUM_48
#define FF_AUDIO_WS_GPIO        GPIO_NUM_38
#define FF_AUDIO_DOUT_GPIO      GPIO_NUM_47

/* ---- Synthesis tunables (ff_audio.h's top comment has the full
 * reasoning for each). ---- */
/** ~25% of INT16 full scale (32767). Maintainer-tunable on glass — no
 * calibrated-loudness reference exists for this speaker/enclosure yet;
 * flagged as an interpretation call in ff_audio.h. */
#define FF_AUDIO_AMPLITUDE      8192
#define FF_AUDIO_ATTACK_MS      5u
#define FF_AUDIO_RELEASE_MS     10u
#define FF_AUDIO_CHUNK_MS       10u
#define FF_AUDIO_CHUNK_FRAMES   ((FF_AUDIO_SAMPLE_RATE_HZ * FF_AUDIO_CHUNK_MS) / 1000u) /* 441 */

/* See this file's top comment, "Channel enable/disable strategy". */
#define FF_AUDIO_KEEP_CHANNEL_ENABLED 0

#define FF_AUDIO_TASK_STACK_BYTES 4096
#define FF_AUDIO_TASK_PRIORITY    5

/* Own 2*pi constant rather than relying on <math.h>'s M_PI, which is not
 * guaranteed to be defined under every -std= this project might build
 * with (newlib gates it behind feature-test macros in strict-ANSI modes). */
#define FF_AUDIO_TWO_PI (6.28318530717958647692f)

/* ---------------------------------------------------------------------
 * State. `s_tx_chan` is touched by the main/app_main task only during
 * `ff_audio_init` (before the render task exists) and by the render task
 * only afterward — no lock needed for it. `s_busy` / `s_stop_requested` /
 * `s_playing_ev` are shared between whichever task calls `ff_audio_play`/
 * `ff_audio_stop`/`ff_audio_busy` (today, always app_main's main loop)
 * and the render task, so every access goes through `s_lock`.
 * ------------------------------------------------------------------- */
static i2s_chan_handle_t s_tx_chan = NULL;
static QueueHandle_t     s_mailbox = NULL; /* 1-deep: at most one pending "please play this" request */
static TaskHandle_t      s_task = NULL;
static bool               s_initialized = false;
static bool               s_noop_logged = false;            /* ff_audio_play called pre-/post-failed-init, logged once */
static bool               s_reenable_failure_logged = false; /* see this file's top comment, "Light sleep" */

static portMUX_TYPE       s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool      s_busy = false;
static volatile bool      s_stop_requested = false;
static ff_sound_event_t   s_playing_ev = FF_SOUND_COUNT; /* meaningful only while s_busy */

/* Chunk scratch buffer — stereo interleaved int16 frames, FF_AUDIO_CHUNK_
 * FRAMES of them (~10ms at 44.1kHz). File-scope static: only the render
 * task ever touches it, so this avoids putting ~1.7KB on that task's own
 * stack for no reason. */
static int16_t s_chunk_buf[FF_AUDIO_CHUNK_FRAMES * 2];

/* ---------------------------------------------------------------------
 * ff_audio_alloc_and_init_channel — allocate a fresh I2S0 TX channel and
 * bring it up in standard (Philips) mode for the PCM5101. Used both by
 * ff_audio_init (first bring-up) and by the post-enable-failure recovery
 * path (this file's top comment, "Light sleep"). On any failure, cleans
 * up whatever it allocated and leaves s_tx_chan NULL.
 * ------------------------------------------------------------------- */
static esp_err_t ff_audio_alloc_and_init_channel(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(FF_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        s_tx_chan = NULL;
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(FF_AUDIO_SAMPLE_RATE_HZ),
        /* Mono content duplicated to both stereo slots — see ff_audio.h's
         * top comment, "Mono to stereo". */
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, /* not connected on this board */
            .bclk = FF_AUDIO_BCLK_GPIO,
            .ws = FF_AUDIO_WS_GPIO,
            .dout = FF_AUDIO_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED, /* not connected — TX-only, no recording path */
            .invert_flags = {
                .mclk_inv = 0,
                .bclk_inv = 0,
                .ws_inv = 0,
            },
        },
    };
    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        (void)i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return err;
    }
    return ESP_OK;
}

/* Enable s_tx_chan, recovering with a full re-init if the enable itself
 * fails (this file's top comment, "Light sleep / I2S interaction"). */
static esp_err_t ff_audio_channel_enable_with_recovery(void)
{
    esp_err_t err = i2s_channel_enable(s_tx_chan);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    if (!s_reenable_failure_logged) {
        ESP_LOGW(TAG,
                 "i2s_channel_enable failed (%s) — attempting a full channel re-init "
                 "(see ff_audio.c's top comment, \"Light sleep / I2S interaction\")",
                 esp_err_to_name(err));
        s_reenable_failure_logged = true;
    }

    (void)i2s_channel_disable(s_tx_chan); /* best-effort; may itself error if already in a bad state */
    (void)i2s_del_channel(s_tx_chan);
    s_tx_chan = NULL;

    esp_err_t reinit_err = ff_audio_alloc_and_init_channel();
    if (reinit_err != ESP_OK) {
        ESP_LOGE(TAG, "channel re-init after enable failure also failed: %s", esp_err_to_name(reinit_err));
        return reinit_err;
    }
    return i2s_channel_enable(s_tx_chan);
}

static bool ff_audio_should_stop(void)
{
    bool r;
    portENTER_CRITICAL(&s_lock);
    r = s_stop_requested;
    portEXIT_CRITICAL(&s_lock);
    return r;
}

/* Render one pattern step ({freq_hz, ms}) in ~10ms chunks, checking for a
 * preemption/stop request at each chunk boundary. freq_hz == 0 is a REST
 * (ff_sound.h) — true silence, no envelope. A real note gets a 5ms linear
 * attack and 10ms linear release (clamped to fit if a future pattern ever
 * has a step shorter than attack+release — none in today's table does;
 * FF_SOUND_PATTERN's shortest step is TAP's 20ms). Returns false if the
 * step was cut short by a stop/preemption request, true if it played to
 * completion. */
static bool ff_audio_render_step(ff_sound_step_t step)
{
    uint32_t const total_frames = (FF_AUDIO_SAMPLE_RATE_HZ * (uint32_t)step.ms) / 1000u;
    if (total_frames == 0u) {
        return true; /* a zero-length step is a no-op, not an error */
    }

    uint32_t attack_ms = FF_AUDIO_ATTACK_MS;
    uint32_t release_ms = FF_AUDIO_RELEASE_MS;
    if (attack_ms + release_ms > (uint32_t)step.ms) {
        /* Defensive clamp — see this function's doc comment. Split the
         * step evenly between attack and release rather than let one
         * swallow the whole note. */
        uint32_t const half = (uint32_t)step.ms / 2u;
        attack_ms = half;
        release_ms = (uint32_t)step.ms - half;
    }
    uint32_t const attack_frames = (FF_AUDIO_SAMPLE_RATE_HZ * attack_ms) / 1000u;
    uint32_t const release_frames = (FF_AUDIO_SAMPLE_RATE_HZ * release_ms) / 1000u;
    uint32_t const release_start_frame = (total_frames > release_frames) ? (total_frames - release_frames) : 0u;

    bool const is_rest = (step.freq_hz == 0u);
    float const phase_inc = FF_AUDIO_TWO_PI * (float)step.freq_hz / (float)FF_AUDIO_SAMPLE_RATE_HZ;
    float phase = 0.0f;

    for (uint32_t frame_idx = 0u; frame_idx < total_frames;) {
        if (ff_audio_should_stop()) {
            return false;
        }

        uint32_t const remaining = total_frames - frame_idx;
        uint32_t const frames_this_chunk = (remaining < FF_AUDIO_CHUNK_FRAMES) ? remaining : FF_AUDIO_CHUNK_FRAMES;

        for (uint32_t i = 0u; i < frames_this_chunk; i++) {
            int16_t sample = 0;
            if (!is_rest) {
                uint32_t const f = frame_idx + i;
                float gain = 1.0f;
                if (attack_frames > 0u && f < attack_frames) {
                    gain = (float)f / (float)attack_frames;
                } else if (release_frames > 0u && f >= release_start_frame) {
                    uint32_t const rel_f = f - release_start_frame;
                    gain = 1.0f - ((float)rel_f / (float)release_frames);
                    if (gain < 0.0f) {
                        gain = 0.0f;
                    }
                }
                float const s = sinf(phase) * (float)FF_AUDIO_AMPLITUDE * gain;
                sample = (int16_t)s;
                phase += phase_inc;
                if (phase >= FF_AUDIO_TWO_PI) {
                    phase -= FF_AUDIO_TWO_PI;
                }
            }
            s_chunk_buf[2u * i] = sample;     /* left */
            s_chunk_buf[2u * i + 1u] = sample; /* right — duplicated mono, see ff_audio.h */
        }

        size_t bytes_written = 0;
        size_t const bytes_to_write = (size_t)frames_this_chunk * 2u * sizeof(int16_t);
        esp_err_t const err =
            i2s_channel_write(s_tx_chan, s_chunk_buf, bytes_to_write, &bytes_written, pdMS_TO_TICKS(200));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_write failed: %s — aborting this pattern", esp_err_to_name(err));
            return false;
        }

        frame_idx += frames_this_chunk;
    }
    return true;
}

static void ff_audio_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        ff_sound_event_t ev;
        if (xQueueReceive(s_mailbox, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ff_sound_pattern_t const *pattern = ff_sound_pattern_for(ev);
        if (pattern == NULL) {
            /* ff_audio_play already filters this; stay defensive rather
             * than trust the caller unconditionally. */
            portENTER_CRITICAL(&s_lock);
            s_busy = (uxQueueMessagesWaiting(s_mailbox) > 0u);
            if (!s_busy) {
                s_playing_ev = FF_SOUND_COUNT;
            }
            portEXIT_CRITICAL(&s_lock);
            continue;
        }

        portENTER_CRITICAL(&s_lock);
        s_stop_requested = false; /* a genuinely new play clears any earlier stop request */
        portEXIT_CRITICAL(&s_lock);

        esp_err_t const enable_err = ff_audio_channel_enable_with_recovery();
        if (enable_err == ESP_OK) {
            for (uint8_t i = 0u; i < pattern->n; i++) {
                if (ff_audio_should_stop()) {
                    break;
                }
                if (!ff_audio_render_step(pattern->steps[i])) {
                    break;
                }
            }
#if !FF_AUDIO_KEEP_CHANNEL_ENABLED
            (void)i2s_channel_disable(s_tx_chan); /* see this file's top comment, "Channel enable/disable strategy" */
#endif
        } else {
            ESP_LOGE(TAG, "channel enable failed for event %d — pattern dropped", (int)ev);
        }

        portENTER_CRITICAL(&s_lock);
        bool const more_pending = uxQueueMessagesWaiting(s_mailbox) > 0u;
        if (!more_pending) {
            s_busy = false;
            s_playing_ev = FF_SOUND_COUNT;
        }
        portEXIT_CRITICAL(&s_lock);
    }
}

esp_err_t ff_audio_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = ff_audio_alloc_and_init_channel();
    if (err != ESP_OK) {
        return err; /* already logged; caller (app_main) treats this as non-fatal */
    }

    s_mailbox = xQueueCreate(1, sizeof(ff_sound_event_t));
    if (s_mailbox == NULL) {
        ESP_LOGE(TAG, "mailbox queue allocation failed");
        (void)i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t const rc =
        xTaskCreate(ff_audio_task_fn, "ff_audio", FF_AUDIO_TASK_STACK_BYTES, NULL, FF_AUDIO_TASK_PRIORITY, &s_task);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "render task creation failed");
        vQueueDelete(s_mailbox);
        s_mailbox = NULL;
        (void)i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "I2S0 TX up (44.1kHz/16-bit/stereo, BCLK=GPIO48 WS=GPIO38 DOUT=GPIO47), render task started");
    return ESP_OK;
}

void ff_audio_play(ff_sound_event_t ev)
{
    if (!s_initialized) {
        if (!s_noop_logged) {
            ESP_LOGW(TAG, "ff_audio_play(%d) called but ff_audio_init never succeeded — no-op (logged once)", (int)ev);
            s_noop_logged = true;
        }
        return;
    }
    if (ff_sound_pattern_for(ev) == NULL) {
        return; /* out of vocabulary — reject, not guess (ff_audio.h) */
    }

    bool start = false;
    portENTER_CRITICAL(&s_lock);
    if (!s_busy) {
        s_busy = true;
        s_playing_ev = ev;
        start = true;
    } else if (ff_sound_preempts(ev, s_playing_ev)) {
        s_stop_requested = true;
        s_playing_ev = ev; /* the render task's next iteration is this one now */
        start = true;
    }
    /* else: not preempting a currently-playing (higher-or-equal-tier)
     * pattern — silently dropped, see ff_audio.h's "Preemption,
     * concurrency, and the one-speaker rule". */
    portEXIT_CRITICAL(&s_lock);

    if (start) {
        (void)xQueueOverwrite(s_mailbox, &ev);
    }
}

void ff_audio_stop(void)
{
    if (!s_initialized) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_stop_requested = true;
    portEXIT_CRITICAL(&s_lock);
    if (s_mailbox != NULL) {
        (void)xQueueReset(s_mailbox); /* drop anything queued to start next too — a full stop is a full stop */
    }
}

bool ff_audio_busy(void)
{
    bool r;
    portENTER_CRITICAL(&s_lock);
    r = s_busy;
    portEXIT_CRITICAL(&s_lock);
    return r;
}
