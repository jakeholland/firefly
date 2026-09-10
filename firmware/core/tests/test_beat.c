/**
 * test_beat.c — Unity coverage for ff_beat.h/.c (docs/specs/S31-music-swarm.md).
 *
 * Synthetic-signal cases named directly by the S31 deliverable:
 *   - a 128 BPM click train yields beats within +-30ms of each click;
 *   - silence yields no beats;
 *   - a slow swell (loudness rising/falling far slower than the onset
 *     detector's own fast/slow time constants) yields no beats;
 *   - a 2Hz IMU bounce yields 2 beats/s.
 * Plus a handful of smaller unit-level pins (reset state, loudness
 * auto-ranging honesty, level-dbfs conversion) in the same
 * proxy-resistant style ff_miclevel's own test file (test_miclevel.c)
 * uses.
 */
#include "ff_beat.h"

#include <math.h>

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define TICK_MS 20u /* 50 Hz nominal mic frame rate, ff_beat.h's own documented input cadence */

/* 2026-09-09 amendment (fix/s31-beat-real-audio): the onset detector now
 * runs on `low_band_dbfs`/`mid_band_dbfs` (ff_bandenergy.h), not
 * `env_dbfs` — see ff_beat.h's own top comment. Every test in THIS file
 * that only cares about a single broadband level jump (the click train,
 * the slow swell, the loudness-ranging tests) feeds the SAME value into
 * every level field: a click/swell is a genuine broadband event, so it
 * shows up identically in every band too — an honest, simplifying model
 * for a synthetic single-number test signal, not a claim that real
 * music ever behaves this way (see test_beat_music.c for tests that
 * DO give the two bands independently meaningful values). */
static ff_beat_sample_t mic_sample(float env_dbfs)
{
    ff_beat_sample_t s = {0};
    s.source = FF_BEAT_SOURCE_MIC;
    s.env_dbfs = env_dbfs;
    s.rms_dbfs = env_dbfs;
    s.low_band_dbfs = env_dbfs;
    s.mid_band_dbfs = env_dbfs;
    return s;
}

static ff_beat_sample_t imu_sample(float accel_mag_g)
{
    ff_beat_sample_t s = {0};
    s.source = FF_BEAT_SOURCE_IMU;
    s.accel_mag_g = accel_mag_g;
    return s;
}

/* --------------------------------------------------------------------
 * Reset / basic state.
 * ------------------------------------------------------------------- */

static void reset_is_the_least_claiming_state(void)
{
    ff_beat_t b;
    b.loudness = 0.77f; /* poison, so a no-op reset would be caught */
    ff_beat_reset(&b);
    TEST_ASSERT_EQUAL_INT(FF_BEAT_SOURCE_NONE, b.source);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b.loudness);
    TEST_ASSERT_EQUAL_UINT32(0u, b.beat_count);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b.bpm_estimate);
}

static void null_pointers_are_safe_no_ops(void)
{
    ff_beat_sample_t const s = mic_sample(-20.0f);
    ff_beat_update(NULL, &s, TICK_MS, 1000u); /* must not crash */
    ff_beat_t b;
    ff_beat_reset(&b);
    ff_beat_update(&b, NULL, TICK_MS, 1000u); /* must not crash, must not mutate b */
    TEST_ASSERT_EQUAL_INT(FF_BEAT_SOURCE_NONE, b.source);
}

static void level_dbfs_conversion_is_honest(void)
{
    ff_beat_sample_t mic = mic_sample(-42.0f);
    TEST_ASSERT_EQUAL_FLOAT(-42.0f, ff_beat_level_dbfs(&mic));

    ff_beat_sample_t none = {0};
    none.source = FF_BEAT_SOURCE_NONE;
    TEST_ASSERT_EQUAL_FLOAT(FF_BEAT_FLOOR_DBFS, ff_beat_level_dbfs(&none));

    /* IMU at exactly the reference g reads 0 "dBFS" on the shared scale. */
    ff_beat_sample_t imu_ref = imu_sample(FF_BEAT_IMU_REF_G);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, ff_beat_level_dbfs(&imu_ref));

    /* A zero-magnitude IMU sample floors, never fabricates/NaNs/-infs. */
    ff_beat_sample_t imu_zero = imu_sample(0.0f);
    TEST_ASSERT_EQUAL_FLOAT(FF_BEAT_FLOOR_DBFS, ff_beat_level_dbfs(&imu_zero));
}

/* --------------------------------------------------------------------
 * Loudness auto-ranging.
 * ------------------------------------------------------------------- */

/* A steady level, fed for long enough that the (fast, ~1s) floor/
 * ceiling attack settles, maps to loudness ~0 (floor snaps to it, then
 * the range-floor widen pushes the ceiling above it) — never a
 * fabricated mid-scale reading for a signal that has shown no dynamic
 * range at all. */
static void a_steady_level_settles_to_near_zero_loudness(void)
{
    ff_beat_t b;
    ff_beat_reset(&b);
    ff_beat_sample_t const s = mic_sample(-50.0f);
    for (int i = 0; i < 100; i++) { /* 2s @ 20ms */
        ff_beat_update(&b, &s, TICK_MS, (uint32_t)(i + 1) * TICK_MS);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, b.loudness);
}

/* A quiet baseline followed by a genuinely louder, sustained level maps
 * to a HIGH loudness once the ceiling has had a chance to track it —
 * proxy-resistant: a broken "always report 0.5" implementation would
 * fail this (and the steady-level test above, which pins near-0). */
static void a_sustained_loud_level_after_a_quiet_baseline_reads_loud(void)
{
    ff_beat_t b;
    ff_beat_reset(&b);
    ff_beat_sample_t const quiet = mic_sample(-55.0f);
    for (int i = 0; i < 100; i++) { /* 2s quiet baseline */
        ff_beat_update(&b, &quiet, TICK_MS, (uint32_t)(i + 1) * TICK_MS);
    }
    ff_beat_sample_t const loud = mic_sample(-20.0f);
    uint32_t now = 2000u;
    for (int i = 0; i < 75; i++) { /* 1.5s loud — >= the ~1s ceiling attack */
        now += TICK_MS;
        ff_beat_update(&b, &loud, TICK_MS, now);
    }
    TEST_ASSERT_GREATER_THAN_FLOAT(0.8f, b.loudness);
}

/* --------------------------------------------------------------------
 * S31 deliverable: 128 BPM click train -> beats within +-30ms.
 * ------------------------------------------------------------------- */

static void click_train_128bpm_yields_beats_within_30ms(void)
{
    ff_beat_t b;
    ff_beat_reset(&b);

    float const period_ms = 60000.0f / 128.0f; /* ~468.75ms */
    float next_click_ms = period_ms;            /* first click, not at t=0 (t=0 is the quiet baseline start) */
    uint32_t now = 0u;

    /* Expected click times actually fed (quantized to the 20ms grid the
     * synthetic stream runs on), and the beat time observed for each —
     * both recorded so the +-30ms check below compares against what was
     * REALLY fed, not the ideal unquantized period. */
    float expected_click_ms[16];
    int n_expected = 0;
    uint32_t observed_beat_ms[16];
    int n_observed = 0;
    uint32_t last_seen_count = 0u;

    for (int i = 0; i < 350; i++) { /* 7s @ 20ms - about 14-15 clicks at 128 BPM */
        now += TICK_MS;
        bool const is_click = ((float)now >= next_click_ms);
        ff_beat_sample_t const s = mic_sample(is_click ? -8.0f : -55.0f);
        if (is_click && n_expected < 16) {
            expected_click_ms[n_expected++] = (float)now;
            next_click_ms += period_ms;
        }
        ff_beat_update(&b, &s, TICK_MS, now);
        if (b.beat_count != last_seen_count) {
            last_seen_count = b.beat_count;
            if (n_observed < 16) observed_beat_ms[n_observed++] = b.last_beat_ms;
        }
    }

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(10, n_observed, "128 BPM click train produced too few beats over 7s");
    /* Every observed beat lines up with SOME click within +-30ms (a
     * one-to-one, in-order pairing — click trains never reorder). */
    int limit = (n_observed < n_expected) ? n_observed : n_expected;
    for (int i = 0; i < limit; i++) {
        float const diff = (float)observed_beat_ms[i] - expected_click_ms[i];
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(30.0f, 0.0f, diff, "beat did not land within +-30ms of its click");
    }
}

/* --------------------------------------------------------------------
 * S31 deliverable: silence yields no beats.
 * ------------------------------------------------------------------- */

static void silence_yields_no_beats(void)
{
    ff_beat_t b;
    ff_beat_reset(&b);
    ff_beat_sample_t const s = mic_sample(FF_BEAT_FLOOR_DBFS);
    uint32_t now = 0u;
    for (int i = 0; i < 250; i++) { /* 5s */
        now += TICK_MS;
        ff_beat_update(&b, &s, TICK_MS, now);
    }
    TEST_ASSERT_EQUAL_UINT32(0u, b.beat_count);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, b.loudness);
}

/* --------------------------------------------------------------------
 * S31 deliverable: a slow swell yields no beats.
 * ------------------------------------------------------------------- */

static void slow_swell_yields_no_beats(void)
{
    ff_beat_t b;
    ff_beat_reset(&b);
    uint32_t now = 0u;
    /* A full swell cycle every 8s (-55 -> -20 -> -55 dBFS), FAR slower
     * than the onset detector's fast (~5-50ms) or slow (~100-400ms)
     * time constants, so fast_env and slow_env track together the whole
     * way and never cross FF_BEAT_ONSET_THRESHOLD_DB apart. */
    for (int i = 0; i < 500; i++) { /* 10s */
        now += TICK_MS;
        float const phase = (float)now / 8000.0f * 2.0f * (float)M_PI;
        float const env = -37.5f + 17.5f * sinf(phase); /* midpoint -37.5, +-17.5dB swing */
        ff_beat_sample_t const s = mic_sample(env);
        ff_beat_update(&b, &s, TICK_MS, now);
    }
    TEST_ASSERT_EQUAL_UINT32(0u, b.beat_count);
}

/* --------------------------------------------------------------------
 * S31 deliverable: 2Hz IMU bounce yields 2 beats/s.
 * ------------------------------------------------------------------- */

static void imu_bounce_2hz_yields_2_beats_per_second(void)
{
    ff_beat_t b;
    ff_beat_reset(&b);
    uint32_t now = 0u;
    float const freq_hz = 2.0f;
    int const total_ticks = 150; /* 3.0s */
    for (int i = 0; i < total_ticks; i++) {
        now += TICK_MS;
        float const t_s = (float)now / 1000.0f;
        float const mag = 0.30f * sinf(2.0f * (float)M_PI * freq_hz * t_s); /* one positive peak per cycle */
        ff_beat_sample_t const s = imu_sample(mag);
        ff_beat_update(&b, &s, TICK_MS, now);
    }
    /* 3.0s @ 2Hz -> 6 positive peaks; allow +-1 for edge effects at the
     * very start/end of the window. */
    TEST_ASSERT_INT_WITHIN(1, 6, (int)b.beat_count);
    TEST_ASSERT_EQUAL_INT(FF_BEAT_SOURCE_IMU, b.source);
}

/* A MIC-source click train must NOT be detected via the IMU peak-picker
 * (and vice versa) — pins that switching `sample->source` mid-stream
 * (e.g. the shell's own mic-present -> absent -> IMU fallback) engages
 * the right detector, not a leftover from the other one. */
static void source_selects_the_matching_detector(void)
{
    ff_beat_t b;
    ff_beat_reset(&b);
    uint32_t now = 0u;
    /* A steady, non-onset MIC level for a while (settles ranging), then
     * switch to IMU bounce - the IMU path must still fire beats even
     * though it never touched the MIC fast/slow filters. */
    ff_beat_sample_t const quiet_mic = mic_sample(-50.0f);
    for (int i = 0; i < 100; i++) {
        now += TICK_MS;
        ff_beat_update(&b, &quiet_mic, TICK_MS, now);
    }
    TEST_ASSERT_EQUAL_UINT32(0u, b.beat_count);

    for (int i = 0; i < 100; i++) { /* 2s of 2Hz IMU bounce -> ~4 beats */
        now += TICK_MS;
        float const t_s = (float)now / 1000.0f;
        float const mag = 0.30f * sinf(2.0f * (float)M_PI * 2.0f * t_s);
        ff_beat_sample_t const s = imu_sample(mag);
        ff_beat_update(&b, &s, TICK_MS, now);
    }
    TEST_ASSERT_GREATER_THAN_UINT32(0u, b.beat_count);
    TEST_ASSERT_EQUAL_INT(FF_BEAT_SOURCE_IMU, b.source);
}

/* --------------------------------------------------------------------
 * 2026-09-09 amendment (fix/s31-beat-real-audio): synthetic REAL-MUSIC
 * signals — the on-device evidence this amendment fixes (loudness=0.82,
 * bpm=0.0 for an entire 15s real-music set) could never have been
 * caught by the click-train test above, which is a genuine broadband
 * transient in near-silence — exactly the one case the OLD detector was
 * tuned for. These two tests instead model a 128 BPM four-on-the-floor
 * kick with a COMPRESSED broadband envelope (only a 3-5dB swing on
 * `env_dbfs`, mirroring a mastered track sitting near a limiter's
 * ceiling throughout) but a real per-band attack/decay punch on each
 * kick — the shape a low band (or, for the "phone speaker" variant,
 * only the mid band) actually shows even under that same broadband
 * compression. See ff_bandenergy.h/ff_beat.h's own top comments for why
 * this is exactly the fix's own thesis: band-limited flux survives
 * broadband compression that would otherwise hide the kick entirely.
 * ------------------------------------------------------------------- */

/** A single kick's percussive envelope: a fast (one-tick) linear attack
 *  to `peak_db` above baseline, then an exponential decay back down
 *  with time constant `decay_tau_ms` — `phase_ms` is time since this
 *  kick's own onset (see the callers below for how that is tracked on
 *  the test's own 20ms tick grid, mirroring `click_train_128bpm_...`'s
 *  own `next_click_ms` bookkeeping). `peak_db == 0` (the phone-speaker
 *  variant's suppressed band) always returns 0 — an honest "no bump
 *  here", not a fabricated tiny one. */
static float kick_bump_db(float phase_ms, float attack_ms, float decay_tau_ms, float peak_db)
{
    if (peak_db == 0.0f || phase_ms < 0.0f) return 0.0f;
    if (phase_ms <= attack_ms) {
        return peak_db * (phase_ms / attack_ms);
    }
    return peak_db * expf(-(phase_ms - attack_ms) / decay_tau_ms);
}

/** Runs `n_ticks` of a `bpm`-tempo kick through a fresh detector.
 *  `low_peak_db`/`mid_peak_db` let the two variants below (normal vs.
 *  phone-speaker-suppressed-low) share one driver. Fills
 *  `expected_kick_ms`/`observed_beat_ms` (capacity `cap`, actual counts
 *  in `*n_expected`/`*n_observed`) exactly like
 *  `click_train_128bpm_yields_beats_within_30ms` does, and returns the
 *  detector's own final `bpm_estimate`. */
static float run_synthetic_kick(float bpm, float low_peak_db, float mid_peak_db, int n_ticks, float *expected_kick_ms,
                                 int cap, int *n_expected, uint32_t *observed_beat_ms, int *n_observed,
                                 ff_beat_t *out_final)
{
    ff_beat_t b;
    ff_beat_reset(&b);

    float const period_ms = 60000.0f / bpm;
    float const attack_ms = 20.0f;
    float const decay_tau_ms = 80.0f;
    float const env_baseline_db = -8.0f;   /* compressed broadband floor — near the assumed limiter ceiling */
    float const env_swing_db = 4.0f;       /* 3-5dB broadband swing per the deliverable's own "compressed" spec */
    float const low_baseline_db = -25.0f;
    float const mid_baseline_db = -30.0f;

    float next_kick_ms = period_ms; /* first kick, not at t=0 (t=0 is the quiet count-in) */
    float last_kick_start_ms = -1000.0f; /* far enough in the past that phase_ms is huge (bump == 0) before the first kick */
    uint32_t now = 0u;
    *n_expected = 0;
    *n_observed = 0;
    uint32_t last_seen_count = 0u;

    for (int i = 0; i < n_ticks; i++) {
        now += TICK_MS;
        if ((float)now >= next_kick_ms && *n_expected < cap) {
            expected_kick_ms[(*n_expected)++] = (float)now;
            last_kick_start_ms = (float)now;
            next_kick_ms += period_ms;
        }
        float const phase_ms = (float)now - last_kick_start_ms;
        float const low_bump = kick_bump_db(phase_ms, attack_ms, decay_tau_ms, low_peak_db);
        float const mid_bump = kick_bump_db(phase_ms, attack_ms, decay_tau_ms, mid_peak_db);
        /* The broadband envelope gets a much SMALLER, capped bump — the
         * compressed-mix behavior this whole fix is about: the kick is
         * clearly visible per-band but nearly invisible broadband. */
        float const env_bump = (low_bump + mid_bump > 0.0f) ? env_swing_db : 0.0f;

        ff_beat_sample_t s = {0};
        s.source = FF_BEAT_SOURCE_MIC;
        s.env_dbfs = env_baseline_db + env_bump;
        s.rms_dbfs = s.env_dbfs;
        s.low_band_dbfs = low_baseline_db + low_bump;
        s.mid_band_dbfs = mid_baseline_db + mid_bump;

        ff_beat_update(&b, &s, TICK_MS, now);
        if (b.beat_count != last_seen_count) {
            last_seen_count = b.beat_count;
            if (*n_observed < cap) observed_beat_ms[(*n_observed)++] = b.last_beat_ms;
        }
    }

    if (out_final != NULL) *out_final = b;
    return b.bpm_estimate;
}

static void assert_beats_track_kicks_within_40ms(float const *expected_kick_ms, int n_expected,
                                                  uint32_t const *observed_beat_ms, int n_observed)
{
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(10, n_observed, "128 BPM synthetic kick produced too few beats over 7s");
    int const limit = (n_observed < n_expected) ? n_observed : n_expected;
    for (int i = 0; i < limit; i++) {
        float const diff = (float)observed_beat_ms[i] - expected_kick_ms[i];
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(40.0f, 0.0f, diff, "beat did not land within +-40ms of its kick");
    }
}

/** The deliverable's own headline case: a 128 BPM kick with a sustained
 *  bass line, compressed broadband dynamic range, detected via the LOW
 *  band (the kick's own fundamental). */
static void synthetic_music_128bpm_kick_yields_beats_within_40ms_and_bpm_within_3(void)
{
    float expected_kick_ms[16];
    uint32_t observed_beat_ms[16];
    int n_expected = 0, n_observed = 0;
    ff_beat_t final_state;

    float const bpm_estimate = run_synthetic_kick(128.0f, /*low_peak_db=*/15.0f, /*mid_peak_db=*/2.0f, 350,
                                                   expected_kick_ms, 16, &n_expected, observed_beat_ms, &n_observed,
                                                   &final_state);

    assert_beats_track_kicks_within_40ms(expected_kick_ms, n_expected, observed_beat_ms, n_observed);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(3.0f, 128.0f, bpm_estimate, "BPM estimate not within +-3 of 128");
    (void)final_state; /* loudness auto-ranging is unchanged by this amendment and has its own dedicated coverage
                           above (a_steady_level_settles_to_near_zero_loudness et al.) — this test's own synthetic
                           envelope shape has no particular loudness value to pin */
}

/** "Phone speaker" variant — high-passed at ~150Hz, per the deliverable:
 *  the LOW band never moves (`low_peak_db == 0`, an honest "the bass is
 *  gone"), so detection must come entirely from the MID band instead —
 *  this is what actually exercises the detector's OR-across-bands
 *  fusion (ff_beat.c's own `low_onset || mid_onset`), which the LOW-only
 *  case above cannot distinguish from a "MID is ignored" bug. */
static void synthetic_phone_speaker_kick_yields_beats_within_40ms_and_bpm_within_3(void)
{
    float expected_kick_ms[16];
    uint32_t observed_beat_ms[16];
    int n_expected = 0, n_observed = 0;
    ff_beat_t final_state;

    float const bpm_estimate = run_synthetic_kick(128.0f, /*low_peak_db=*/0.0f, /*mid_peak_db=*/15.0f, 350,
                                                   expected_kick_ms, 16, &n_expected, observed_beat_ms, &n_observed,
                                                   &final_state);

    assert_beats_track_kicks_within_40ms(expected_kick_ms, n_expected, observed_beat_ms, n_observed);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(3.0f, 128.0f, bpm_estimate, "BPM estimate not within +-3 of 128");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(reset_is_the_least_claiming_state);
    RUN_TEST(null_pointers_are_safe_no_ops);
    RUN_TEST(level_dbfs_conversion_is_honest);
    RUN_TEST(a_steady_level_settles_to_near_zero_loudness);
    RUN_TEST(a_sustained_loud_level_after_a_quiet_baseline_reads_loud);
    RUN_TEST(click_train_128bpm_yields_beats_within_30ms);
    RUN_TEST(silence_yields_no_beats);
    RUN_TEST(slow_swell_yields_no_beats);
    RUN_TEST(imu_bounce_2hz_yields_2_beats_per_second);
    RUN_TEST(source_selects_the_matching_detector);
    RUN_TEST(synthetic_music_128bpm_kick_yields_beats_within_40ms_and_bpm_within_3);
    RUN_TEST(synthetic_phone_speaker_kick_yields_beats_within_40ms_and_bpm_within_3);
    return UNITY_END();
}
