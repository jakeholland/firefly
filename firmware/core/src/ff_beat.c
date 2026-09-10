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
 * update` already uses, reused here verbatim for the ceiling tracker
 * (wants "fast toward a LOUDER level, slow back down" — the ordinary
 * rising-is-attack sense). */
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
     * imu_have_prev/imu_rising/mic_*_track.have_prev/mic_*_track.primed)
     * — see this struct's own field comments (ff_beat.h). floor_dbfs/
     * ceil_dbfs at a literal 0.0f are fine too: ranging_init == false
     * means the next real sample re-seeds floor/ceil from scratch (see
     * ff_beat_update) rather than mapping against this transient 0; the
     * band trackers' own `primed == false` gets the identical treatment
     * in `band_tracker_update` below. */
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

/* Median of `n` floats, via an in-place insertion sort — `n` is always
 * FF_BEAT_MUSIC_FLUX_WINDOW_N (75) or up to FF_BEAT_BPM_HISTORY_N (5) in
 * this file, small enough that an O(n^2) sort is cheaper (and far
 * simpler to get right) than a partial-selection algorithm at either
 * 50Hz (flux) or once-per-beat (BPM) call rates — see ff_beat.h's own
 * "Beat/onset detection" doc comment for why a MEDIAN, not a mean, is
 * the right statistic here (resistant to the very transients/outliers
 * it exists to detect against). `arr` is sorted in place — always the
 * CALLER's own scratch copy, never a live history ring itself. */
static float median_of(float *arr, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        float const key = arr[i];
        size_t j = i;
        while (j > 0u && arr[j - 1u] > key) {
            arr[j] = arr[j - 1u];
            j--;
        }
        arr[j] = key;
    }
    return arr[n / 2u];
}

/* One band's onset-flux update (2026-09-09 amendment) — see
 * ff_beat_band_tracker_t's own doc comment (ff_beat.h) for the state
 * this owns, and ff_beat.h's top comment ("Beat/onset detection",
 * "WHY FLUX") for why a frame-to-frame RISE, not a level-vs-median
 * comparison, correctly ignores a slow swell while still catching a
 * real attack. Returns true iff THIS band's flux just cleared its own
 * adaptive threshold — the caller ORs this across both bands
 * (`ff_beat_update` below) rather than this function knowing anything
 * about the other band. */
static bool band_tracker_update(ff_beat_band_tracker_t *t, float level_db)
{
    float flux_db = 0.0f;
    if (t->have_prev) {
        float const delta = level_db - t->prev_level_db;
        flux_db = (delta > 0.0f) ? delta : 0.0f; /* half-wave rectified — only RISES are onset evidence */
    }
    t->prev_level_db = level_db;
    t->have_prev = true;

    if (!t->primed) {
        for (size_t i = 0; i < FF_BEAT_MUSIC_FLUX_WINDOW_N; i++) {
            t->flux_history_db[i] = flux_db;
        }
        t->next = 0u;
        t->primed = true;
    } else {
        t->flux_history_db[t->next] = flux_db;
        t->next = (uint8_t)((t->next + 1u) % FF_BEAT_MUSIC_FLUX_WINDOW_N);
    }

    float sorted[FF_BEAT_MUSIC_FLUX_WINDOW_N];
    memcpy(sorted, t->flux_history_db, sizeof(sorted));
    float const median_flux_db = median_of(sorted, FF_BEAT_MUSIC_FLUX_WINDOW_N);

    float adaptive_threshold_db = median_flux_db + FF_BEAT_MUSIC_FLUX_MARGIN_DB;
    if (adaptive_threshold_db < FF_BEAT_MUSIC_FLUX_MIN_DB) adaptive_threshold_db = FF_BEAT_MUSIC_FLUX_MIN_DB;

    return flux_db >= adaptive_threshold_db;
}

/* Fold a raw BPM into [FF_BEAT_BPM_MIN, FF_BEAT_BPM_MAX] by repeated
 * doubling/halving — see ff_beat.h's own top comment, `bpm_estimate`'s
 * paragraph. Bounded by construction: both constants are firmly
 * positive and more than an octave apart, so a positive `bpm` always
 * reaches the range in a finite number of steps — no risk of looping
 * forever or folding down to exactly 0. */
static float fold_bpm_to_range(float bpm)
{
    while (bpm < FF_BEAT_BPM_MIN) bpm *= 2.0f;
    while (bpm > FF_BEAT_BPM_MAX) bpm *= 0.5f;
    return bpm;
}

/* Register one beat: bump the monotonic counter, arm the shared
 * refractory window, and update the BPM estimate (2026-09-09 amendment:
 * median-of-recent-intervals, octave-folded — see ff_beat.h's own top
 * comment) from the interval since the previous beat (only once a prior
 * beat actually exists — `beat_count` is checked BEFORE incrementing,
 * so this reads "was there a prior beat", not "will there be one after
 * this"). */
static void beat_fire(ff_beat_t *b, uint32_t now_ms)
{
    if (b->beat_count > 0u) {
        uint32_t const interval_ms = now_ms - b->last_beat_ms; /* short-session subtraction; see ff_beat.h's top comment */
        if (interval_ms > 0u) {
            b->ioi_history_ms[b->ioi_next] = (float)interval_ms;
            b->ioi_next = (uint8_t)((b->ioi_next + 1u) % FF_BEAT_BPM_HISTORY_N);
            if (b->ioi_count < FF_BEAT_BPM_HISTORY_N) b->ioi_count++;

            float sorted[FF_BEAT_BPM_HISTORY_N];
            memcpy(sorted, b->ioi_history_ms, (size_t)b->ioi_count * sizeof(float));
            float const median_ioi_ms = median_of(sorted, b->ioi_count);
            b->bpm_estimate = fold_bpm_to_range(60000.0f / median_ioi_ms);
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
     * "Loudness: auto-ranging floor/ceiling". UNCHANGED by the
     * 2026-09-09 amendment: still driven by the BROADBAND level. */
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
        /* Band-limited onset-flux detector — see ff_beat.h's top
         * comment, "Beat/onset detection" (2026-09-09 amendment). Both
         * trackers are always updated (a band's own history must keep
         * moving every sample regardless of whether the OTHER band just
         * fired), and the beat fires on EITHER clearing its threshold. */
        bool const low_onset = band_tracker_update(&b->mic_low_track, sample->low_band_dbfs);
        bool const mid_onset = band_tracker_update(&b->mic_mid_track, sample->mid_band_dbfs);
        if ((low_onset || mid_onset) && now_ms >= b->refractory_until_ms) {
            beat_fire(b, now_ms);
        }
    } else { /* FF_BEAT_SOURCE_IMU */
        /* Vertical-axis bounce peak-picking, one-sample-lagged local
         * maximum above FF_BEAT_IMU_PEAK_THRESHOLD_G — UNCHANGED by the
         * 2026-09-09 amendment. A dB-domain band/flux detector (the MIC
         * path above) fits a multi-band audio mixture; a footstep/dance
         * bounce is closer to a single roughly-sinusoidal excursion on
         * ONE channel already, so a plain rising-then-falling peak test
         * is the better-fitting primitive here — see ff_beat.h's top
         * comment. */
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
