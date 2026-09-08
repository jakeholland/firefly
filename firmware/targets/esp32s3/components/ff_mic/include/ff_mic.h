/**
 * ff_mic.h — S30 device HAL: the Waveshare ESP32-S3-Touch-LCD-1.46's
 * onboard I2S MEMS microphone (INMP441/MSM261-class, 24-bit-in-32-bit,
 * part number not published by Waveshare — behavior verified against
 * Waveshare's own reference firmware, `MIC_Speech.h`/`.c`). Mirrors
 * `ff_audio.h`'s structure and failure-handling style — see that
 * header's top comment for the shared conventions (non-fatal init,
 * "log once, not per call", honest absence) — but this is the OTHER
 * direction: RX, not TX, and on a SEPARATE I2S peripheral so it never
 * disturbs `ff_audio`'s speaker path.
 *
 * Spec: docs/specs/S30-audio-input.md.
 *
 * ## Hardware (verified against Waveshare's own reference firmware,
 * `MIC_Speech.h`/`.c`, https://github.com/yaosy1997/ESP32-S3-Touch-LCD-1.46-Test)
 * I2S MEMS mic, NOT PDM. `MIC_SCK` (bit clock) = GPIO15, `MIC_WS` (word
 * select) = GPIO2, `MIC_SD` (data in) = GPIO39. No MCLK. I2S_NUM_1 (the
 * speaker already owns I2S_NUM_0 — see ff_audio.h's own wiring —
 * MUST NOT share a port), master role, standard (Philips) mode, 16 kHz,
 * 32-bit slot width carrying the mic's 24 significant bits in the TOP
 * bits of each word (the bottom 8 bits are the mic's own don't-care
 * padding — Waveshare's reference driver, and this one, right-shift by
 * 8 for a 24-bit value, or by 16 for a 16-bit-ish value with headroom;
 * this driver uses the latter — see ff_mic.c's own top comment), mono,
 * `slot_mask = I2S_STD_SLOT_RIGHT` (this specific mic's fixed I2S
 * address-pin strap — confirmed against the reference firmware; the
 * OTHER slot reads silence on this board, not a mirror of the same
 * signal). No inversions.
 *
 * ## Power policy — off unless a face asks
 * The reader task and the I2S1 channel are both OFF by default:
 * `ff_mic_init` brings up the channel in a DISABLED state and does NOT
 * start reading. `ff_mic_start()`/`ff_mic_stop()` are the on/off switch
 * (mirrors `ff_audio`'s own "channel enabled only while a pattern is
 * playing" policy, ff_audio.c's top comment) — no product feature reads
 * the mic yet (S30 is bring-up only; S29/Music will build on this), so
 * "always listening" would be pure battery cost for zero benefit today.
 *
 * ## Honesty contract (CLAUDE.md: "honest data over pretty data",
 * mirrors ff_compass.h's own sentinel-rejection discipline exactly)
 * `ff_mic_status_t.present` is the ONE fact every consumer (console,
 * DIAGNOSTICS) actually branches on, and it goes false in exactly two
 * cases, indistinguishable to a caller (this driver logs which one
 * happened; it does not invent a third public "reason" enum a caller
 * would have to keep in sync with two independent failure paths for no
 * behavioral payoff — see ff_mic.c's own doc comment):
 *   - `ff_mic_init` never brought the I2S1 channel up at all (a
 *     hardware/driver-level failure — GPIO conflict, allocation
 *     failure, etc.);
 *   - the reader task's own SENTINEL check trips: every sample in every
 *     frame read for a full 1000ms is either exactly zero OR identical
 *     to the previous frame's own corresponding sample (the "stuck
 *     data" case a MEMS mic can produce if it never actually started
 *     clocking data out despite the I2S peripheral itself reporting a
 *     clean read) — see ff_mic.c's own `ff_mic_frame_is_sentinel`.
 * Both cases read the exact same way everywhere this driver is
 * observed: `present=false`, `running` forced false, no level, no
 * fabricated frame. A puck with no working mic still boots and is fully
 * usable — this HAL is never a boot blocker (never even called from a
 * boot-order-sensitive path at all; `ff_mic_init` is safe to call
 * anywhere after `ff_display_expander_init` per this header's own GPIO
 * matrix note below).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 20 ms frames at 16 kHz — see ff_miclevel.h (firmware/core), the pure
 *  level-math module this driver's reader task calls into. */
#define FF_MIC_SAMPLE_RATE_HZ 16000u
#define FF_MIC_FRAME_SAMPLES 320u

/**
 * ff_mic_init — allocate I2S_NUM_1 in standard (Philips) RX mode for
 * this board's onboard I2S mic (BCLK GPIO15, WS GPIO2, DIN GPIO39; no
 * MCLK, no DOUT — this is an RX-only path, symmetric with ff_audio's
 * TX-only one) and start the reader task IN ITS IDLE (not-running)
 * state — see this header's top comment, "Power policy". The channel
 * itself is left DISABLED (mirrors `ff_audio_init`'s own "allocate now,
 * enable only while actually used" strategy) until `ff_mic_start()`.
 *
 * Call once, any time after the GPIO matrix is free to route (in
 * practice: after `ff_display_expander_init` releases the shared reset
 * lines — this driver touches no I2C, no shared bus, and no GPIO any
 * other component owns, so ordering relative to ff_compass_init/
 * ff_audio_init/ff_display bring-up does not matter the way it did for
 * ff_audio's own boot-order fix; see that header's own "Boot order"
 * history for why THAT one mattered and this one doesn't).
 *
 * Non-fatal on failure, same "log and continue" posture as every other
 * HAL bring-up in this codebase (`ff_audio_init`, `ff_compass_init`):
 * logs the underlying `esp_err_t` and returns it, but the caller is
 * expected to IGNORE the return value and continue booting regardless.
 * Every subsequent call into this HAL after a failed init is a no-op
 * (`ff_mic_status().present == false`, `ff_mic_start()`/`ff_mic_stop()`
 * do nothing), logged once.
 *
 * Idempotent: a second call when already initialized is a no-op that
 * returns ESP_OK immediately.
 */
esp_err_t ff_mic_init(void);

/**
 * ff_mic_start — enable the I2S1 RX channel and let the reader task
 * begin pulling 20ms frames. Non-blocking: signals the (already-running,
 * already-created) reader task and returns immediately. Safe to call
 * when already running (a no-op) or before `ff_mic_init` ever succeeded
 * (a no-op, logged once via the same `ff_mic_init`-failure log
 * `ff_mic_status()` would also reflect).
 *
 * Resets the DC-blocking filter and envelope state (ff_miclevel.h) on
 * every start — a stale filter state from a previous session (or from
 * the brief window between init and this call) must never bleed into a
 * fresh one, mirrors `ff_batt_filter_t`'s own reset-on-restart
 * discipline.
 */
void ff_mic_start(void);

/**
 * ff_mic_stop — disable the I2S1 RX channel and idle the reader task.
 * Safe to call when already stopped, or before `ff_mic_init` ever
 * succeeded (both no-ops). `ff_mic_status().running` reads false
 * immediately after this call returns (the channel-disable + task-idle
 * transition is synchronous from this call's own perspective, even
 * though the reader task itself notices and stops on its own next loop
 * iteration — never more than one frame period, 20ms, later).
 */
void ff_mic_stop(void);

/**
 * ff_mic_status_t / ff_mic_status — a live snapshot. `present` is this
 * driver's ONE honesty gate — see this header's top comment for exactly
 * when it goes false. `running` is meaningful only when `present` is
 * true (mirrors `ff_compass_status_t`'s own "meaningful only when
 * present" convention for its own sub-fields). `frames_read`/
 * `read_errors` are lifetime counters since the last `ff_mic_start()`
 * (reset to 0 by every `ff_mic_start()` call, NOT by `ff_mic_init` —
 * consistent with "how has THIS session gone" being the actionable bench
 * question, mirroring `mc_stats_t`'s own since-connect framing).
 * `last_read_age_ms` is 0 while `!running`, and while `running` is the
 * time since the reader task's last completed `i2s_channel_read` call
 * (bounded — that call never uses `portMAX_DELAY`, so this age can never
 * silently freeze at a stale value while the channel is nominally
 * running; a genuinely stuck read shows up here AND, after 1s, flips
 * `present` false via the sentinel check).
 */
typedef struct {
    bool present;
    bool running;
    uint32_t sample_rate_hz; /* FF_MIC_SAMPLE_RATE_HZ once present, 0 otherwise */
    uint32_t frames_read;
    uint32_t read_errors;
    uint32_t last_read_age_ms;
} ff_mic_status_t;

ff_mic_status_t ff_mic_status(void);

/**
 * ff_mic_level_t / ff_mic_level — the latest 20ms frame's RMS and peak
 * (dBFS, `ff_miclevel_to_dbfs`'s convention — see firmware/core/include/
 * ff_miclevel.h), plus the running 300ms-ish attack/release envelope
 * over successive frames' RMS. All three fields are `FF_MICLEVEL_
 * FLOOR_DBFS` (never NaN/-inf) before the first frame is ever read, or
 * whenever `ff_mic_status().present` is false — this function does not
 * itself gate on presence (a caller who wants the honest "is this even
 * real" answer reads `ff_mic_status()` too, same "one call per fact,
 * caller composes" shape `ff_compass_status`/`ff_compass_last_mag_board`
 * already establish as two separate calls rather than one struct with
 * every field union'd together).
 */
typedef struct {
    float rms_dbfs;
    float peak_dbfs;
    float envelope_dbfs;
} ff_mic_level_t;

ff_mic_level_t ff_mic_level(void);

/**
 * ff_mic_last_frame — copy up to `n` int16 samples of the latest
 * completed 20ms frame (DC-removed, the same samples `ff_mic_level`'s
 * RMS/peak were computed from) into `dst`. Returns the number of
 * samples actually copied (0 if `dst` is NULL, `n` is 0, `ff_mic_init`
 * never succeeded, or no frame has been read yet — never a partially-
 * written buffer past what it returns). Exists for future consumers
 * (S29/Music's own envelope/FFT work, docs/specs/S30-audio-input.md's
 * own "what S29/Music will build on this" section) — no caller in this
 * PR reads it; kept here, tested, so that work has a stable seam to
 * build on rather than adding its own copy-out path later.
 */
size_t ff_mic_last_frame(int16_t *dst, size_t n);

#ifdef __cplusplus
}
#endif
