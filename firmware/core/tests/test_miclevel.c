/**
 * test_miclevel.c — S30 mic level math: RMS/peak dBFS from synthetic
 * frames, envelope attack/release, DC removal. See ff_miclevel.h.
 */
#include <math.h>
#include <string.h>

#include "unity.h"

#include "ff_miclevel.h"

/* Own pi constant rather than relying on <math.h>'s M_PI — not
 * guaranteed to be defined under every -std= this project builds with
 * (newlib/glibc gate it behind feature-test macros in strict-ANSI
 * modes); same reasoning ff_audio.c documents for its own FF_AUDIO_TWO_PI. */
#define TEST_MICLEVEL_TWO_PI (6.28318530717958647692)

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* ff_miclevel_to_dbfs                                                  */
/* ------------------------------------------------------------------- */

static void miclevel_to_dbfs_full_scale_is_zero_dbfs(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, ff_miclevel_to_dbfs(32768.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, ff_miclevel_to_dbfs(-32768.0f)); /* magnitude, sign-independent */
}

static void miclevel_to_dbfs_half_scale_is_about_minus_6db(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.05f, -6.0206f, ff_miclevel_to_dbfs(16384.0f));
}

static void miclevel_to_dbfs_zero_is_floored_not_infinite(void)
{
    float const v = ff_miclevel_to_dbfs(0.0f);
    TEST_ASSERT_EQUAL_FLOAT(FF_MICLEVEL_FLOOR_DBFS, v);
    TEST_ASSERT_TRUE_MESSAGE(isfinite(v), "must be a real, comparable number — never -inf/NaN");
}

static void miclevel_to_dbfs_tiny_value_is_floored(void)
{
    /* A vanishingly small nonzero magnitude computes to well below the
     * floor mathematically (20*log10(1e-6/32768) ~ -170 dB) — must clamp
     * to the floor, not return that raw (still-finite, but dishonestly
     * precise) number. */
    float const v = ff_miclevel_to_dbfs(0.000001f);
    TEST_ASSERT_EQUAL_FLOAT(FF_MICLEVEL_FLOOR_DBFS, v);
}

/* ------------------------------------------------------------------- */
/* ff_miclevel_frame_compute                                            */
/* ------------------------------------------------------------------- */

static void miclevel_frame_silence_is_floor(void)
{
    float samples[FF_MICLEVEL_FRAME_SAMPLES];
    memset(samples, 0, sizeof(samples));

    ff_miclevel_frame_t out;
    ff_miclevel_frame_compute(samples, FF_MICLEVEL_FRAME_SAMPLES, &out);

    TEST_ASSERT_EQUAL_FLOAT(FF_MICLEVEL_FLOOR_DBFS, out.rms_dbfs);
    TEST_ASSERT_EQUAL_FLOAT(FF_MICLEVEL_FLOOR_DBFS, out.peak_dbfs);
}

static void miclevel_frame_full_scale_square_wave_is_zero_dbfs(void)
{
    /* A synthetic +-32768 square wave: RMS of a full-scale square wave
     * IS full scale (every sample's magnitude is full scale), so both
     * RMS and peak read 0 dBFS — the cleanest possible sanity check that
     * the RMS math isn't silently attenuating (e.g. a stray /2 or a
     * wrong sqrt placement). */
    float samples[FF_MICLEVEL_FRAME_SAMPLES];
    for (size_t i = 0; i < FF_MICLEVEL_FRAME_SAMPLES; i++) {
        samples[i] = (i % 2u == 0u) ? 32768.0f : -32768.0f;
    }

    ff_miclevel_frame_t out;
    ff_miclevel_frame_compute(samples, FF_MICLEVEL_FRAME_SAMPLES, &out);

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, out.rms_dbfs);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, out.peak_dbfs);
}

static void miclevel_frame_sine_rms_is_about_3db_below_peak(void)
{
    /* A full-scale sine's RMS is peak/sqrt(2) — about 3.01 dB below its
     * own peak, regardless of amplitude (as long as it's not clipped).
     * This is THE standard proxy-resistant check for "is this actually
     * computing RMS, not e.g. average absolute value" (average absolute
     * value of a sine is peak*2/pi, about 3.9 dB below peak — a
     * different, wrong number that a mean-abs-value bug would produce
     * instead). */
    float samples[FF_MICLEVEL_FRAME_SAMPLES];
    float const amplitude = 10000.0f;
    for (size_t i = 0; i < FF_MICLEVEL_FRAME_SAMPLES; i++) {
        double const phase = TEST_MICLEVEL_TWO_PI * 5.0 * (double)i / (double)FF_MICLEVEL_FRAME_SAMPLES;
        samples[i] = amplitude * (float)sin(phase);
    }

    ff_miclevel_frame_t out;
    ff_miclevel_frame_compute(samples, FF_MICLEVEL_FRAME_SAMPLES, &out);

    TEST_ASSERT_FLOAT_WITHIN(0.15f, out.peak_dbfs - 3.0103f, out.rms_dbfs);
}

static void miclevel_frame_peak_tracks_single_loud_sample(void)
{
    /* A frame that is silent except for ONE loud sample: RMS must stay
     * low (averaged over 320 samples) while peak reports the loud
     * sample almost exactly — proves peak isn't secretly reading the
     * same RMS computation under a different name. */
    float samples[FF_MICLEVEL_FRAME_SAMPLES];
    memset(samples, 0, sizeof(samples));
    samples[160] = 20000.0f;

    ff_miclevel_frame_t out;
    ff_miclevel_frame_compute(samples, FF_MICLEVEL_FRAME_SAMPLES, &out);

    TEST_ASSERT_FLOAT_WITHIN(0.01f, ff_miclevel_to_dbfs(20000.0f), out.peak_dbfs);
    TEST_ASSERT_TRUE_MESSAGE(out.rms_dbfs < out.peak_dbfs - 10.0f,
                              "RMS over 319 zeros + one loud sample must read far below that sample's own peak");
}

static void miclevel_frame_empty_is_floor_not_crash(void)
{
    ff_miclevel_frame_t out;
    ff_miclevel_frame_compute(NULL, 0u, &out);
    TEST_ASSERT_EQUAL_FLOAT(FF_MICLEVEL_FLOOR_DBFS, out.rms_dbfs);
    TEST_ASSERT_EQUAL_FLOAT(FF_MICLEVEL_FLOOR_DBFS, out.peak_dbfs);
}

static void miclevel_frame_null_out_is_safe_no_op(void)
{
    float samples[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    ff_miclevel_frame_compute(samples, 4u, NULL); /* must not crash */
    TEST_PASS();
}

/* ------------------------------------------------------------------- */
/* ff_miclevel_dc_remove                                                */
/* ------------------------------------------------------------------- */

static void miclevel_dc_remove_strips_constant_offset(void)
{
    /* A large constant DC bias, held for many samples: the one-pole
     * blocker must converge toward zero output (the block, not merely
     * attenuate-a-little proxy check — feed enough samples for the pole
     * to settle). */
    ff_miclevel_dc_state_t st;
    ff_miclevel_dc_reset(&st);

    float y = 0.0f;
    for (int i = 0; i < 2000; i++) {
        y = ff_miclevel_dc_remove(&st, 8000.0f);
    }
    TEST_ASSERT_TRUE_MESSAGE(fabsf(y) < 50.0f, "DC blocker must converge near zero on a held constant input");
}

static void miclevel_dc_remove_passes_ac_signal_through(void)
{
    /* A zero-mean AC signal, once the filter has settled past its own
     * startup transient, must come through close to unattenuated — the
     * DC blocker's whole point is "kill DC, keep AC", so an over-eager
     * implementation (e.g. one that also damps real signal) is a real
     * finding, not just "it removes DC". */
    ff_miclevel_dc_state_t st;
    ff_miclevel_dc_reset(&st);

    float peak_after_settle = 0.0f;
    for (int i = 0; i < 4000; i++) {
        double const phase = TEST_MICLEVEL_TWO_PI * 100.0 * (double)i / (double)FF_MICLEVEL_SAMPLE_RATE_HZ;
        float const x = 10000.0f * (float)sin(phase);
        float const y = ff_miclevel_dc_remove(&st, x);
        if (i > 3000) { /* well past the settling transient */
            float const a = fabsf(y);
            if (a > peak_after_settle) peak_after_settle = a;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(peak_after_settle > 9000.0f, "a real AC signal must pass through close to unattenuated");
}

static void miclevel_dc_remove_null_state_returns_input_unchanged(void)
{
    TEST_ASSERT_EQUAL_FLOAT(1234.0f, ff_miclevel_dc_remove(NULL, 1234.0f));
}

/* ------------------------------------------------------------------- */
/* ff_miclevel_envelope_update                                          */
/* ------------------------------------------------------------------- */

static void miclevel_envelope_reset_is_floor(void)
{
    ff_miclevel_envelope_t env;
    ff_miclevel_envelope_update(&env, -10.0f, 0u); /* garbage before reset — reset must still win */
    ff_miclevel_envelope_reset(&env);
    TEST_ASSERT_EQUAL_FLOAT(FF_MICLEVEL_FLOOR_DBFS, env.value_dbfs);
}

static void miclevel_envelope_attacks_faster_than_it_releases(void)
{
    /* The core, proxy-resistant property this envelope exists for: a
     * sudden loud frame must move the envelope UP quickly (attack), and
     * a subsequent return to silence must move it back DOWN more
     * slowly (release) — never symmetric, and never instantaneous
     * either way (an envelope that snaps straight to the input on every
     * update would pass a weaker "it moves toward the target" check
     * while providing none of the smoothing `mic watch` needs). */
    ff_miclevel_envelope_t env;
    ff_miclevel_envelope_reset(&env);

    /* One 20 ms frame at a loud level: partial rise, not instant. */
    ff_miclevel_envelope_update(&env, -10.0f, 20u);
    float const after_one_attack_frame = env.value_dbfs;
    TEST_ASSERT_TRUE_MESSAGE(after_one_attack_frame > FF_MICLEVEL_FLOOR_DBFS + 1.0f, "must have risen off the floor");
    TEST_ASSERT_TRUE_MESSAGE(after_one_attack_frame < -10.0f - 0.01f, "must not have snapped instantly to -10 dBFS");

    /* Drive it to (near) full settle at -10 dBFS. */
    for (int i = 0; i < 50; i++) {
        ff_miclevel_envelope_update(&env, -10.0f, 20u);
    }
    float const settled = env.value_dbfs;
    TEST_ASSERT_FLOAT_WITHIN(0.5f, -10.0f, settled);

    /* Now silence: one 20 ms frame's worth of release must move it DOWN
     * by LESS than one attack frame moved it UP over the same 20 ms
     * (release is the slower time constant, FF_MICLEVEL_ENV_RELEASE_MS
     * > FF_MICLEVEL_ENV_ATTACK_MS). */
    ff_miclevel_envelope_update(&env, FF_MICLEVEL_FLOOR_DBFS, 20u);
    float const release_drop = settled - env.value_dbfs;
    float const attack_rise = after_one_attack_frame - FF_MICLEVEL_FLOOR_DBFS;
    TEST_ASSERT_TRUE_MESSAGE(release_drop < attack_rise,
                              "one 20ms release step must move less than one 20ms attack step — release is slower");
}

static void miclevel_envelope_converges_to_target_over_many_frames(void)
{
    ff_miclevel_envelope_t env;
    ff_miclevel_envelope_reset(&env);
    for (int i = 0; i < 200; i++) {
        ff_miclevel_envelope_update(&env, -40.0f, 20u);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.1f, -40.0f, env.value_dbfs);
}

static void miclevel_envelope_zero_dt_does_not_move(void)
{
    ff_miclevel_envelope_t env;
    ff_miclevel_envelope_reset(&env);
    ff_miclevel_envelope_update(&env, -5.0f, 0u);
    TEST_ASSERT_EQUAL_FLOAT(FF_MICLEVEL_FLOOR_DBFS, env.value_dbfs);
}

static void miclevel_envelope_huge_dt_clamps_to_target_not_overshoot(void)
{
    ff_miclevel_envelope_t env;
    ff_miclevel_envelope_reset(&env);
    ff_miclevel_envelope_update(&env, -20.0f, 1000000u); /* an absurd dt — alpha must clamp to 1.0, not blow up */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -20.0f, env.value_dbfs);
}

static void miclevel_envelope_null_is_safe_no_op(void)
{
    ff_miclevel_envelope_reset(NULL);
    ff_miclevel_envelope_update(NULL, -5.0f, 20u);
    TEST_PASS();
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(miclevel_to_dbfs_full_scale_is_zero_dbfs);
    RUN_TEST(miclevel_to_dbfs_half_scale_is_about_minus_6db);
    RUN_TEST(miclevel_to_dbfs_zero_is_floored_not_infinite);
    RUN_TEST(miclevel_to_dbfs_tiny_value_is_floored);

    RUN_TEST(miclevel_frame_silence_is_floor);
    RUN_TEST(miclevel_frame_full_scale_square_wave_is_zero_dbfs);
    RUN_TEST(miclevel_frame_sine_rms_is_about_3db_below_peak);
    RUN_TEST(miclevel_frame_peak_tracks_single_loud_sample);
    RUN_TEST(miclevel_frame_empty_is_floor_not_crash);
    RUN_TEST(miclevel_frame_null_out_is_safe_no_op);

    RUN_TEST(miclevel_dc_remove_strips_constant_offset);
    RUN_TEST(miclevel_dc_remove_passes_ac_signal_through);
    RUN_TEST(miclevel_dc_remove_null_state_returns_input_unchanged);

    RUN_TEST(miclevel_envelope_reset_is_floor);
    RUN_TEST(miclevel_envelope_attacks_faster_than_it_releases);
    RUN_TEST(miclevel_envelope_converges_to_target_over_many_frames);
    RUN_TEST(miclevel_envelope_zero_dt_does_not_move);
    RUN_TEST(miclevel_envelope_huge_dt_clamps_to_target_not_overshoot);
    RUN_TEST(miclevel_envelope_null_is_safe_no_op);

    return UNITY_END();
}
