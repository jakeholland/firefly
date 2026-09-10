/**
 * test_bandenergy.c — Unity coverage for ff_bandenergy.h/.c
 * (docs/specs/S31-music-swarm.md's 2026-09-09 amendment).
 *
 * Proxy-resistant the same way test_miclevel.c's own tone tests are: a
 * tone placed IN a band must read louder in that band than in the
 * other, and a tone placed OUTSIDE both bands must read quiet in both —
 * a broken "just return the broadband RMS twice" implementation would
 * fail every one of these.
 */
#include "ff_bandenergy.h"

#include <math.h>

#include "ff_miclevel.h" /* FF_MICLEVEL_FRAME_SAMPLES, FF_MICLEVEL_FLOOR_DBFS */
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* Feed `n_frames` frames of a `freq_hz` sine (amplitude `amp`, sample
 * rate FF_BANDENERGY_SAMPLE_RATE_HZ) through `st`, returning the LAST
 * frame's band energy — enough settling time for the one-pole cascade
 * (slowest corner 60Hz, time constant ~2.7ms) to reach steady state
 * well within a handful of 20ms frames. */
static ff_bandenergy_frame_t feed_tone(ff_bandenergy_t *st, float freq_hz, float amp, int n_frames)
{
    ff_bandenergy_frame_t out = {0};
    float samples[FF_MICLEVEL_FRAME_SAMPLES];
    static uint32_t s_phase_samples = 0; /* continuous phase across frames within one call sequence */
    for (int f = 0; f < n_frames; f++) {
        for (size_t i = 0; i < FF_MICLEVEL_FRAME_SAMPLES; i++) {
            float const t = (float)(s_phase_samples + i) / (float)FF_BANDENERGY_SAMPLE_RATE_HZ;
            samples[i] = amp * sinf(2.0f * (float)M_PI * freq_hz * t);
        }
        s_phase_samples += (uint32_t)FF_MICLEVEL_FRAME_SAMPLES;
        ff_bandenergy_frame_compute(st, samples, FF_MICLEVEL_FRAME_SAMPLES, &out);
    }
    return out;
}

/* 100Hz sits near the geometric center of the LOW band (60-200Hz) but
 * is also less than an octave from the LOW/MID shared corner (200Hz) —
 * the two single-pole (6dB/octave) filter stages this module uses (see
 * ff_bandenergy.h's own "Filter design" doc comment for the deliberate
 * "stable over surgical" tradeoff) do not reject a near-corner
 * frequency hard, so the real separation here is a couple dB, not the
 * double-digit rejection a resonant (biquad) bandpass would give — a
 * modest but real, DIRECTIONALLY CORRECT margin is exactly what this
 * proxy-resistant check exists to pin (see the 1000Hz case just below
 * for a frequency comfortably inside the MID band, where the same
 * filters DO separate by double digits). */
static void a_low_band_tone_reads_louder_low_than_mid(void)
{
    ff_bandenergy_t st;
    ff_bandenergy_reset(&st);
    ff_bandenergy_frame_t const out = feed_tone(&st, 100.0f, 8000.0f, 30);
    TEST_ASSERT_GREATER_THAN_FLOAT_MESSAGE(out.mid_dbfs + 2.0f, out.low_dbfs,
                                            "a 100Hz tone did not read at least 2dB louder in LOW than MID");
}

static void a_mid_band_tone_reads_louder_mid_than_low(void)
{
    ff_bandenergy_t st;
    ff_bandenergy_reset(&st);
    ff_bandenergy_frame_t const out = feed_tone(&st, 1000.0f, 8000.0f, 30);
    TEST_ASSERT_GREATER_THAN_FLOAT_MESSAGE(out.low_dbfs + 10.0f, out.mid_dbfs,
                                            "a 1000Hz tone did not read at least 10dB louder in MID than LOW");
}

/* A tone well above both bands (6kHz — comfortably clear of the 2000Hz
 * MID ceiling, and comfortably below this module's own 16kHz sample
 * rate's Nyquist limit so no aliasing muddies what frequency is
 * actually being fed) must read quiet in BOTH — this is what pins a
 * broken "MID is just a highpass with no upper corner" implementation,
 * which the low-vs-mid comparisons above alone cannot catch (a tone
 * above the MID band would still read "louder in MID than LOW" under
 * that bug, passing the test above for the wrong reason). */
static void a_tone_above_both_bands_reads_quiet_in_both(void)
{
    ff_bandenergy_t st;
    ff_bandenergy_reset(&st);
    ff_bandenergy_frame_t const out = feed_tone(&st, 6000.0f, 8000.0f, 30);
    TEST_ASSERT_LESS_THAN_FLOAT_MESSAGE(-20.0f, out.low_dbfs, "a 10kHz tone leaked into the LOW band");
    TEST_ASSERT_LESS_THAN_FLOAT_MESSAGE(-20.0f, out.mid_dbfs, "a 10kHz tone leaked into the MID band");
}

static void silence_floors_both_bands(void)
{
    ff_bandenergy_t st;
    ff_bandenergy_reset(&st);
    float const zeros[FF_MICLEVEL_FRAME_SAMPLES] = {0};
    ff_bandenergy_frame_t out = {0};
    for (int i = 0; i < 5; i++) ff_bandenergy_frame_compute(&st, zeros, FF_MICLEVEL_FRAME_SAMPLES, &out);
    TEST_ASSERT_EQUAL_FLOAT(FF_MICLEVEL_FLOOR_DBFS, out.low_dbfs);
    TEST_ASSERT_EQUAL_FLOAT(FF_MICLEVEL_FLOOR_DBFS, out.mid_dbfs);
}

static void null_and_empty_are_safe_and_honest(void)
{
    ff_bandenergy_frame_t out = {-1.0f, -1.0f};
    ff_bandenergy_frame_compute(NULL, NULL, 0u, &out);
    TEST_ASSERT_EQUAL_FLOAT(FF_MICLEVEL_FLOOR_DBFS, out.low_dbfs);
    TEST_ASSERT_EQUAL_FLOAT(FF_MICLEVEL_FLOOR_DBFS, out.mid_dbfs);

    ff_bandenergy_t st;
    ff_bandenergy_reset(&st);
    float const sample = 1000.0f;
    ff_bandenergy_frame_t out2 = {-1.0f, -1.0f};
    ff_bandenergy_frame_compute(&st, &sample, 0u, &out2); /* n==0 */
    TEST_ASSERT_EQUAL_FLOAT(FF_MICLEVEL_FLOOR_DBFS, out2.low_dbfs);
    TEST_ASSERT_EQUAL_FLOAT(FF_MICLEVEL_FLOOR_DBFS, out2.mid_dbfs);

    ff_bandenergy_frame_compute(NULL, &sample, 1u, &out2); /* st==NULL must not crash */
    ff_bandenergy_reset(NULL);                             /* must not crash */

    ff_bandenergy_frame_t *null_out = NULL;
    ff_bandenergy_frame_compute(&st, &sample, 1u, null_out); /* out==NULL must not crash */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(a_low_band_tone_reads_louder_low_than_mid);
    RUN_TEST(a_mid_band_tone_reads_louder_mid_than_low);
    RUN_TEST(a_tone_above_both_bands_reads_quiet_in_both);
    RUN_TEST(silence_floors_both_bands);
    RUN_TEST(null_and_empty_are_safe_and_honest);
    return UNITY_END();
}
