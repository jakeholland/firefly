/**
 * test_swarm.c — Unity coverage for ff_swarm.h/.c (docs/specs/S31-music-swarm.md),
 * covering the S31 polish motion model (owner feedback on PR #245,
 * 2026-09-08): continuous per-firefly drift, a shared beat-envelope pull
 * toward a fixed radius with relaxation, glow bounded to [0,1], and the
 * every-8th-beat accent.
 */
#include "ff_swarm.h"

#include <math.h>
#include <string.h>

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void init_seeds_all_60_particles_in_bounds(void)
{
    ff_swarm_t sw;
    ff_swarm_init(&sw, 12345u);
    for (uint32_t i = 0; i < FF_SWARM_PARTICLE_COUNT; i++) {
        ff_swarm_particle_t const *p = &sw.particles[i];
        TEST_ASSERT_TRUE(p->wander_r_px >= FF_SWARM_R_MIN_PX && p->wander_r_px <= FF_SWARM_R_MAX_PX);
        TEST_ASSERT_EQUAL_FLOAT(p->wander_r_px, p->r_px);
        TEST_ASSERT_TRUE(p->theta_deg >= 0.0f && p->theta_deg < 360.0f);
        TEST_ASSERT_TRUE(p->angular_speed_deg_s >= -FF_SWARM_ANGULAR_DRIFT_MAX_RAD_S * (180.0f / (float)M_PI)
                       && p->angular_speed_deg_s <=  FF_SWARM_ANGULAR_DRIFT_MAX_RAD_S * (180.0f / (float)M_PI));
        TEST_ASSERT_TRUE(p->radial_speed_px_s >= -FF_SWARM_RADIAL_DRIFT_MAX_PX_S
                       && p->radial_speed_px_s <=  FF_SWARM_RADIAL_DRIFT_MAX_PX_S);
        TEST_ASSERT_TRUE(p->glow >= 0.0f && p->glow <= 1.0f);
    }
}

/* Every 7th firefly (index 6, 13, 20, ...) is the live-green accent
 * color, per FF_SWARM_GREEN_EVERY_N's own doc comment — a domain fact
 * the renderer reads verbatim. */
static void every_seventh_firefly_is_the_green_accent(void)
{
    ff_swarm_t sw;
    ff_swarm_init(&sw, 42u);
    for (uint32_t i = 0; i < FF_SWARM_PARTICLE_COUNT; i++) {
        bool const expect_green = ((i % FF_SWARM_GREEN_EVERY_N) == (FF_SWARM_GREEN_EVERY_N - 1u));
        TEST_ASSERT_EQUAL_INT(expect_green, sw.particles[i].is_accent_green);
    }
}

static void zero_seed_is_remapped_not_degenerate(void)
{
    /* xorshift32 fixes at exactly 0 forever if seeded with 0 - every
     * particle would land at the SAME (0,0) personality, a visibly
     * broken swarm. ff_swarm_init must remap seed 0 to a real seed. */
    ff_swarm_t sw;
    ff_swarm_init(&sw, 0u);
    bool any_nonzero_theta = false;
    for (uint32_t i = 0; i < FF_SWARM_PARTICLE_COUNT; i++) {
        if (sw.particles[i].theta_deg != 0.0f) any_nonzero_theta = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(any_nonzero_theta, "seed 0 produced a degenerate (all-zero) swarm");
}

static void same_seed_same_stimulus_is_bit_reproducible(void)
{
    ff_swarm_t a, b;
    ff_swarm_init(&a, 777u);
    ff_swarm_init(&b, 777u);
    for (int i = 0; i < 200; i++) {
        bool const beat = (i % 25) == 0;
        ff_swarm_step(&a, 0.6f, beat, 1.0f / 30.0f);
        ff_swarm_step(&b, 0.6f, beat, 1.0f / 30.0f);
    }
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));
}

static void different_seeds_produce_different_layouts(void)
{
    ff_swarm_t a, b;
    ff_swarm_init(&a, 111u);
    ff_swarm_init(&b, 222u);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(&a, &b, sizeof(a)));
}

/* "continuously wandering; never static even in silence" — with
 * loudness 0 and no beat ever seen, every particle's angle must still
 * change from one frame to the next, forever. */
static void continuous_drift_in_silence_changes_positions_every_frame(void)
{
    ff_swarm_t sw;
    ff_swarm_init(&sw, 9001u);

    float prev_theta[FF_SWARM_PARTICLE_COUNT];
    for (uint32_t i = 0; i < FF_SWARM_PARTICLE_COUNT; i++) {
        prev_theta[i] = sw.particles[i].theta_deg;
    }

    /* 3 seconds of total silence, no beats at all. Compared with a bare
     * `!=`, not Unity's own TEST_ASSERT_NOT_EQUAL_FLOAT: that macro's
     * default tolerance is RELATIVE to the expected value's own
     * magnitude (~0.003 absolute near theta==335 degrees), which reads
     * two float values as "equal" whenever a slow-drifting particle
     * (angular_speed_deg_s drawn near the low end of its +/-0.2rad/s
     * range) moves less than that per frame — a false failure on a
     * particle that IS still advancing every frame, just slowly. A
     * plain `!=` is exact and has no such floor. */
    for (int frame = 0; frame < 90; frame++) {
        ff_swarm_step(&sw, 0.0f, false, 1.0f / 30.0f);
        for (uint32_t i = 0; i < FF_SWARM_PARTICLE_COUNT; i++) {
            ff_swarm_particle_t const *p = &sw.particles[i];
            TEST_ASSERT_TRUE_MESSAGE(prev_theta[i] != p->theta_deg,
                                      "a firefly's angle did not change frame-to-frame in silence");
            prev_theta[i] = p->theta_deg;
        }
    }
}

/* "pulled toward radius ~120px with strength proportional to
 * envelope*(0.6+loudness), then drift back out" — a strong beat must
 * move EVERY particle's rendered radius strictly closer to
 * FF_SWARM_PULL_TARGET_R_PX than it was the step before; once the
 * envelope has fully decayed (well past one assumed beat interval with
 * no further beat), the radius must have relaxed back away from the
 * pull target toward its own free drift trajectory. */
static void beat_pulls_toward_target_radius_then_relaxes(void)
{
    ff_swarm_t sw;
    ff_swarm_init(&sw, 314u);

    float dist_before[FF_SWARM_PARTICLE_COUNT];
    for (uint32_t i = 0; i < FF_SWARM_PARTICLE_COUNT; i++) {
        dist_before[i] = fabsf(sw.particles[i].r_px - FF_SWARM_PULL_TARGET_R_PX);
    }

    ff_swarm_step(&sw, 1.0f, true, 1.0f / 30.0f);

    float dist_peak[FF_SWARM_PARTICLE_COUNT];
    for (uint32_t i = 0; i < FF_SWARM_PARTICLE_COUNT; i++) {
        dist_peak[i] = fabsf(sw.particles[i].r_px - FF_SWARM_PULL_TARGET_R_PX);
        TEST_ASSERT_LESS_THAN_FLOAT_MESSAGE(
            dist_before[i], dist_peak[i],
            "a beat did not pull this firefly's radius closer to the pull target");
    }
    TEST_ASSERT_GREATER_THAN_FLOAT(0.0f, sw.envelope);

    /* Well past one assumed beat interval with no further beat: the
     * envelope must have decayed to (near) zero. */
    for (int frame = 0; frame < 60; frame++) { /* 2s @ 30fps >> the 0.5s default interval */
        ff_swarm_step(&sw, 0.0f, false, 1.0f / 30.0f);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, sw.envelope);

    for (uint32_t i = 0; i < FF_SWARM_PARTICLE_COUNT; i++) {
        float const dist_relaxed = fabsf(sw.particles[i].r_px - FF_SWARM_PULL_TARGET_R_PX);
        TEST_ASSERT_GREATER_THAN_FLOAT_MESSAGE(
            dist_peak[i], dist_relaxed,
            "this firefly's radius did not relax back away from the pull target once the envelope decayed");
    }
}

/* Glow must never leave [0,1], even when driven with out-of-range
 * loudness and a dense run of beats (including 8th-beat accents). */
static void glow_is_always_bounded_to_unit_interval(void)
{
    ff_swarm_t sw;
    ff_swarm_init(&sw, 2026u);

    for (int frame = 0; frame < 400; frame++) {
        bool const beat = (frame % 9) == 0; /* dense enough to hit several 8th-beat accents */
        float const loudness = 1.5f - (float)(frame % 10) * 0.3f; /* sweeps above 1 and below 0 */
        ff_swarm_step(&sw, loudness, beat, 1.0f / 30.0f);
        for (uint32_t i = 0; i < FF_SWARM_PARTICLE_COUNT; i++) {
            float const g = sw.particles[i].glow;
            TEST_ASSERT_TRUE_MESSAGE(g >= 0.0f && g <= 1.0f, "glow left [0,1]");
        }
    }
}

/* "a stronger accent every 8th beat" — two swarms, seeded and stepped
 * IDENTICALLY through 7 beats, diverge only on the 8th: one gets it,
 * one doesn't. Because both share the same seed and the same dt/t_s
 * sequence up to that point, each particle's twinkle term is IDENTICAL
 * between the two swarms at the moment of comparison — isolating the
 * accent's own contribution instead of being swamped by twinkle noise.
 * A moderate loudness (0.2) keeps the shared glow away from the [0,1]
 * clamp ceiling so the accent's effect is actually visible. */
static void every_eighth_beat_gets_a_stronger_glow_accent(void)
{
    ff_swarm_t with_accent, without_accent;
    ff_swarm_init(&with_accent, 55u);
    ff_swarm_init(&without_accent, 55u);

    for (int beat_n = 0; beat_n < 7; beat_n++) {
        for (int frame = 0; frame < 15; frame++) { /* ~0.5s between beats */
            bool const beat = (frame == 0);
            ff_swarm_step(&with_accent, 0.2f, beat, 1.0f / 30.0f);
            ff_swarm_step(&without_accent, 0.2f, beat, 1.0f / 30.0f);
        }
        (void)beat_n;
    }
    TEST_ASSERT_EQUAL_UINT32(7u, with_accent.beat_index);
    TEST_ASSERT_EQUAL_UINT32(7u, without_accent.beat_index);

    /* The 8th beat: with_accent gets it, without_accent does not. */
    ff_swarm_step(&with_accent, 0.2f, true, 1.0f / 30.0f);
    ff_swarm_step(&without_accent, 0.2f, false, 1.0f / 30.0f);

    TEST_ASSERT_TRUE(with_accent.drop_active);
    TEST_ASSERT_FALSE(without_accent.drop_active);
    TEST_ASSERT_GREATER_THAN_FLOAT(without_accent.particles[0].glow, with_accent.particles[0].glow);
}

/* Null / non-positive dt are safe no-ops. */
static void null_and_nonpositive_dt_are_safe(void)
{
    ff_swarm_step(NULL, 0.5f, true, 1.0f / 30.0f); /* must not crash */
    ff_swarm_t sw;
    ff_swarm_init(&sw, 3u);
    ff_swarm_particle_t const before = sw.particles[0];
    ff_swarm_step(&sw, 0.5f, true, 0.0f);
    ff_swarm_step(&sw, 0.5f, true, -1.0f);
    TEST_ASSERT_EQUAL_FLOAT(before.r_px, sw.particles[0].r_px);
    TEST_ASSERT_EQUAL_FLOAT(before.theta_deg, sw.particles[0].theta_deg);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(init_seeds_all_60_particles_in_bounds);
    RUN_TEST(every_seventh_firefly_is_the_green_accent);
    RUN_TEST(zero_seed_is_remapped_not_degenerate);
    RUN_TEST(same_seed_same_stimulus_is_bit_reproducible);
    RUN_TEST(different_seeds_produce_different_layouts);
    RUN_TEST(continuous_drift_in_silence_changes_positions_every_frame);
    RUN_TEST(beat_pulls_toward_target_radius_then_relaxes);
    RUN_TEST(glow_is_always_bounded_to_unit_interval);
    RUN_TEST(every_eighth_beat_gets_a_stronger_glow_accent);
    RUN_TEST(null_and_nonpositive_dt_are_safe);
    return UNITY_END();
}
