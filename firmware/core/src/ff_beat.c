/**
 * ff_beat.c — see ff_beat.h.
 */
#include "ff_beat.h"

#include <math.h>
#include <string.h>

/** Silence/no-source loudness fade — see ff_beat.h's ff_beat_update doc
 *  comment ("source == NONE decays loudness toward 0"). Deliberately not
 *  named alongside the other time constants in ff_beat.h: this is the
 *  ONE constant with no bearing on the auto-ranging/onset math itself,
 *  purely a "how fast does the swarm visibly calm down" cosmetic pick —
 *  fast enough to read as an honest, prompt response to silence (the
 *  bench protocol's "cover the mic -> QUIET within 2s" acceptance
 *  check), slow enough not to look like a hard cut. */
#define FF_BEAT_NONE_FADE_MS 1200u

/* Generic one-pole-toward-target step: `attack_ms` applies while `target`
 * is ABOVE `current` (rising), `release_ms` while it is below (falling)
 * — the standard envelope-follower convention `ff_miclevel_envelope_
 * update` already uses, reused here verbatim for the MIC fast/slow
 * onset filters and the ceiling tracker (both want "fast toward a
 * LOUDER level, slow back down" — the ordinary rising-is-attack sense). */
static float one_pole_toward(float current, float target, uint32_t dt_ms, uint32_t attack_ms, uint32_t release_ms)
{
    uint32_t const tau_ms = (target > current) ? attack_ms : release_ms;
    float alpha = (tau_ms > 0u) ? ((float)dt_ms / (float)tau_ms) : 1.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    if (alpha < 0.0f) alpha = 0.0f;
    return current + alpha * (target - current);
}

/* The FLOOR tracker's fast direction is the OPPOSITE of the generic
 * envelope convention above: it must react FAST to a newly QUIETER
 * level (target below current — the ordinary "release" sense) and
 * SLOW to a louder one creeping it back up (target above current — the
 * ordinary "attack" sense) — see ff_beat.h's own doc comment, "Loudness:
 * auto-ranging floor/ceiling". A dedicated wrapper (rather than passing
 * `one_pole_toward`'s attack/release arguments pre-swapped at the call
 * site) keeps that inversion named and explicit instead of a silent
 * "why are these backwards" landmine for the next reader. */
static float floor_toward(float current, float target, uint32_t dt_ms, uint32_t fast_ms, uint32_t slow_ms)
{
    uint32_t const tau_ms = (target < current) ? fast_ms : slow_ms;
    float alpha = (tau_ms > 0u) ? ((float)dt_ms / (float)tau_ms) : 1.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    if (alpha < 0.0f) alpha = 0.0f;
    return current + alpha * (target - current);
}

static float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

void ff_beat_reset(ff_beat_t *b)
{
    if (b == NULL) return;
    memset(b, 0, sizeof(*b));
    /* Every other field's zero value IS its honest reset state
     * (0 loudness, 0 beat_count, NONE source, false ranging_init/
     * imu_have_prev/imu_rising) — see this struct's own field comments
     * (ff_beat.h). floor_dbfs/ceil_dbfs/fast_env_dbfs/slow_env_dbfs at a
     * literal 0.0f are fine too: ranging_init == false means the next
     * real sample re-seeds floor/ceil from scratch (see ff_beat_update)
     * rather than mapping against this transient 0. */
}

float ff_beat_level_dbfs(ff_beat_sample_t const *sample)
{
    if (sample == NULL) return FF_BEAT_FLOOR_DBFS;
    switch (sample->source) {
    case FF_BEAT_SOURCE_MIC:
        return (sample->env_dbfs < FF_BEAT_FLOOR_DBFS) ? FF_BEAT_FLOOR_DBFS : sample->env_dbfs;
    case FF_BEAT_SOURCE_IMU: {
        float const mag = fabsf(sample->accel_mag_g);
        if (mag <= 0.0f) return FF_BEAT_FLOOR_DBFS;
        float const db = 20.0f * log10f(mag / FF_BEAT_IMU_REF_G);
        return (db < FF_BEAT_FLOOR_DBFS) ? FF_BEAT_FLOOR_DBFS : db;
    }
    case FF_BEAT_SOURCE_NONE:
    default:
        return FF_BEAT_FLOOR_DBFS;
    }
}

/* Register one beat: bump the monotonic counter, arm the shared
 * refractory window, and update the BPM estimate from the interval
 * since the previous beat (only once a previous beat actually exists —
 * `beat_count` is checked BEFORE incrementing, so this reads "was there
 * a prior beat", not "will there be one after this"). */
static void beat_fire(ff_beat_t *b, uint32_t now_ms)
{
    if (b->beat_count > 0u) {
        uint32_t const interval_ms = now_ms - b->last_beat_ms; /* short-session subtraction; see ff_beat.h's top comment */
        if (interval_ms > 0u) {
            b->bpm_estimate = 60000.0f / (float)interval_ms;
        }
    }
    b->beat_count++;
    b->last_beat_ms = now_ms;
    b->refractory_until_ms = now_ms + FF_BEAT_REFRACTORY_MS;
}

void ff_beat_update(ff_beat_t *b, ff_beat_sample_t const *sample, uint32_t dt_ms, uint32_t now_ms)
{
    if (b == NULL || sample == NULL) return;

    b->source = sample->source;

    if (sample->source == FF_BEAT_SOURCE_NONE) {
        /* No real signal to range/detect against — fade loudness toward
         * 0 and leave every other filter state exactly where it was, so
         * a source that comes back (mic re-present, IMU re-acquired)
         * resumes from a sane floor/ceiling instead of re-seeding from
         * scratch (see ff_beat.h's own doc comment). */
        b->loudness = one_pole_toward(b->loudness, 0.0f, dt_ms, FF_BEAT_NONE_FADE_MS, FF_BEAT_NONE_FADE_MS);
        return;
    }

    float const level = ff_beat_level_dbfs(sample);

    /* Auto-ranging floor/ceiling — see ff_beat.h's top comment,
     * "Loudness: auto-ranging floor/ceiling". */
    if (!b->ranging_init) {
        b->floor_dbfs = level;
        b->ceil_dbfs = level;
        b->ranging_init = true;
    } else {
        b->floor_dbfs = floor_toward(b->floor_dbfs, level, dt_ms, FF_BEAT_FLOOR_ATTACK_MS, FF_BEAT_FLOOR_RELEASE_MS);
        b->ceil_dbfs = one_pole_toward(b->ceil_dbfs, level, dt_ms, FF_BEAT_CEIL_ATTACK_MS, FF_BEAT_CEIL_RELEASE_MS);
    }
    if (b->ceil_dbfs - b->floor_dbfs < FF_BEAT_RANGE_FLOOR_DB) {
        /* Widen from the floor, never lower the floor to meet the
         * ceiling — the floor is the honestly-observed quiet baseline;
         * pinning a minimum SPREAD above it (rather than adjusting both
         * ends) keeps that meaning intact even in a dead-silent room
         * with no real dynamic range yet. */
        b->ceil_dbfs = b->floor_dbfs + FF_BEAT_RANGE_FLOOR_DB;
    }
    b->loudness = clamp01((level - b->floor_dbfs) / (b->ceil_dbfs - b->floor_dbfs));

    if (sample->source == FF_BEAT_SOURCE_MIC) {
        /* Fast-envelope-vs-slow-average onset detector — see this
         * header's top comment, "Beat/onset detection". */
        b->fast_env_dbfs = one_pole_toward(b->fast_env_dbfs, level, dt_ms, FF_BEAT_FAST_ATTACK_MS, FF_BEAT_FAST_RELEASE_MS);
        b->slow_env_dbfs = one_pole_toward(b->slow_env_dbfs, level, dt_ms, FF_BEAT_SLOW_ATTACK_MS, FF_BEAT_SLOW_RELEASE_MS);
        if ((b->fast_env_dbfs - b->slow_env_dbfs >= FF_BEAT_ONSET_THRESHOLD_DB) && (now_ms >= b->refractory_until_ms)) {
            beat_fire(b, now_ms);
        }
    } else { /* FF_BEAT_SOURCE_IMU */
        /* Vertical-axis bounce peak-picking, one-sample-lagged local
         * maximum above FF_BEAT_IMU_PEAK_THRESHOLD_G. A dB-domain
         * fast/slow detector (the MIC path above) fits a broadband
         * transient riding on a steady background; a footstep/dance
         * bounce is closer to a single roughly-sinusoidal excursion, so
         * a plain rising-then-falling peak test is the better-fitting
         * primitive here — see ff_beat.h's top comment. */
        if (b->imu_have_prev) {
            if (sample->accel_mag_g > b->imu_prev_mag_g) {
                b->imu_rising = true;
            } else {
                if (b->imu_rising && b->imu_prev_mag_g >= FF_BEAT_IMU_PEAK_THRESHOLD_G &&
                    now_ms >= b->refractory_until_ms) {
                    beat_fire(b, now_ms);
                }
                b->imu_rising = false;
            }
        } else {
            b->imu_have_prev = true;
        }
        b->imu_prev_mag_g = sample->accel_mag_g;
    }
}
