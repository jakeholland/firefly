/**
 * ff_miclevel.h — S30 mic bring-up: pure, host-testable LEVEL MATH shared
 * by the esp32s3 device's real I2S1 mic reader (`firmware/targets/esp32s3/
 * components/ff_mic`) and this repo's Unity tests. Zero I/O, zero
 * hardware knowledge — exactly the `firmware/core/` placement rule
 * (CLAUDE.md: "All logic goes in firmware/core/ ... no I/O") applied to
 * "how loud is this frame of PCM samples", the same split `ff_sound.h`
 * already draws between the CORE pattern table and `ff_audio.h`'s device
 * HAL.
 *
 * Spec: docs/specs/S30-audio-input.md.
 *
 * ## What lives here
 *   - `ff_miclevel_dc_remove` — a one-pole DC-blocking high-pass filter,
 *     applied to raw samples BEFORE any RMS/peak math (a MEMS mic's raw
 *     output usually rides on a nonzero DC bias; leaving it in would
 *     inflate every RMS/peak reading and defeat the "all-zero or stuck
 *     data" sentinel check below, which a large constant DC offset could
 *     otherwise masquerade as real signal for).
 *   - `ff_miclevel_frame_compute` — RMS + peak of one already-DC-removed
 *     20 ms frame (320 samples at 16 kHz — `FF_MICLEVEL_FRAME_SAMPLES`),
 *     expressed in dBFS against 16-bit full scale.
 *   - `ff_miclevel_envelope_update` — a 300 ms attack/release envelope
 *     follower over successive frames' RMS dBFS, the smoothed number a
 *     bench operator actually watches (`mic watch`) rather than a raw
 *     20 ms sample that jitters frame to frame.
 *
 * ## dBFS convention
 * Full scale is `INT16_MAX + 1` (32768) — the device's I2S reader right-
 * shifts each 32-bit I2S word so the mic's useful top bits land in the
 * lower 16, see docs/specs/S30-audio-input.md's "Format" section — so a
 * sample of exactly +32767 or -32768 is 0 dBFS. `ff_miclevel_to_dbfs`
 * floors at `FF_MICLEVEL_FLOOR_DBFS` (-120) rather than returning -inf
 * for a literal zero/silent input: a real number a caller can always
 * print/compare, never a NaN/inf that would corrupt a min/max or a
 * render-key comparison downstream (this codebase's usual "no fabricated
 * data, but also no `-inf` landmine" balance — see `ff_batt.h` for the
 * same floor-not-infinity treatment on a different unit).
 *
 * ## Interpretation calls (flagged per AGENTS.md — no spec pins these
 * exact numbers, since S30 is being written by this same change)
 *   - DC-blocking pole `FF_MICLEVEL_DC_POLE` = 0.995: a standard one-pole
 *     DC blocker `y[n] = x[n] - x[n-1] + R*y[n-1]`; at 16 kHz this puts
 *     the -3dB corner around 25 Hz — well below any voice/clap content
 *     this mic is meant to pick up, comfortably above true DC.
 *   - Envelope attack/release: 30 ms attack (fast enough that a sudden
 *     loud transient — the bench protocol's finger-snap test — is
 *     visible within one `mic watch` print cycle), 300 ms release (the
 *     deliverable's own stated envelope window; slow enough that the
 *     printed number doesn't flicker between two adjacent watch prints
 *     250 ms apart). Both are `#define`s (`FF_MICLEVEL_ENV_ATTACK_MS` /
 *     `FF_MICLEVEL_ENV_RELEASE_MS`), tunable on glass without touching
 *     the math.
 */
#ifndef FF_MICLEVEL_H
#define FF_MICLEVEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 20 ms at 16 kHz. */
#define FF_MICLEVEL_FRAME_SAMPLES 320u
#define FF_MICLEVEL_SAMPLE_RATE_HZ 16000u

/** Full-scale divisor for dBFS: a sample of +-32768 is 0 dBFS. */
#define FF_MICLEVEL_FULL_SCALE 32768.0f

/** Never returned below this — see this header's top comment,
 *  "dBFS convention". */
#define FF_MICLEVEL_FLOOR_DBFS (-120.0f)

/** DC-blocking one-pole coefficient — see this header's top comment,
 *  "Interpretation calls". */
#define FF_MICLEVEL_DC_POLE 0.995f

/** Envelope attack/release time constants — see this header's top
 *  comment, "Interpretation calls". */
#define FF_MICLEVEL_ENV_ATTACK_MS 30u
#define FF_MICLEVEL_ENV_RELEASE_MS 300u

/**
 * ff_miclevel_dc_state_t / ff_miclevel_dc_remove — one-pole DC blocker,
 * one sample in, one sample out, `y[n] = x[n] - x[n-1] + R*y[n-1]`.
 * `ff_miclevel_dc_reset` zeroes the state (call once when the mic reader
 * starts, so a stale x[n-1]/y[n-1] from a previous session never leaks
 * into a fresh one — mirrors `ff_batt_filter_t`'s own reset-on-(re)start
 * discipline).
 */
typedef struct {
    float prev_in;
    float prev_out;
} ff_miclevel_dc_state_t;

void ff_miclevel_dc_reset(ff_miclevel_dc_state_t *st);
float ff_miclevel_dc_remove(ff_miclevel_dc_state_t *st, float x);

/**
 * ff_miclevel_frame_t / ff_miclevel_frame_compute — RMS + peak (dBFS) of
 * `n` already-DC-removed samples. `n == 0` returns both fields at
 * `FF_MICLEVEL_FLOOR_DBFS` (an empty frame has no signal to report,
 * never a fabricated 0 dBFS).
 */
typedef struct {
    float rms_dbfs;
    float peak_dbfs;
} ff_miclevel_frame_t;

void ff_miclevel_frame_compute(float const *samples, size_t n, ff_miclevel_frame_t *out);

/**
 * ff_miclevel_envelope_t / ff_miclevel_envelope_reset /
 * ff_miclevel_envelope_update — a 300 ms-ish attack/release envelope
 * follower over successive frames' RMS dBFS (this header's top comment,
 * "Interpretation calls", has the exact constants). `value_dbfs` starts
 * at `FF_MICLEVEL_FLOOR_DBFS` on reset (silence, honestly, not a
 * fabricated mid-scale value) and moves toward each new
 * `frame_rms_dbfs` at the attack rate when rising, the release rate when
 * falling. `dt_ms` is the caller's own elapsed-time-since-last-update
 * (the mic reader's frame period, ordinarily 20 ms) — passed explicitly,
 * not assumed, so a dropped/late frame still integrates correctly
 * (mirrors `ff_batt_filter_push`'s own explicit-`now_ms` convention,
 * `ff_batt.h`).
 */
typedef struct {
    float value_dbfs;
} ff_miclevel_envelope_t;

void ff_miclevel_envelope_reset(ff_miclevel_envelope_t *env);
void ff_miclevel_envelope_update(ff_miclevel_envelope_t *env, float frame_rms_dbfs, uint32_t dt_ms);

/**
 * ff_miclevel_to_dbfs — a single sample magnitude (already-DC-removed,
 * linear, may be negative — the caller decides whether it's feeding
 * this an RMS or a peak) to dBFS against `FF_MICLEVEL_FULL_SCALE`,
 * floored at `FF_MICLEVEL_FLOOR_DBFS`. Exposed directly (not just via
 * `ff_miclevel_frame_compute`) since the mic reader's "stuck data"
 * sentinel check (docs/specs/S30-audio-input.md) reasons about raw
 * sample magnitudes, not a whole frame's RMS/peak.
 */
float ff_miclevel_to_dbfs(float linear_magnitude);

#ifdef __cplusplus
}
#endif

#endif /* FF_MICLEVEL_H */
