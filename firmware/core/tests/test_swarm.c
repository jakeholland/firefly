/**
 * test_swarm.c — Unity coverage for ff_swarm.h/.c (docs/specs/S31-music-swarm.md).
 */
#include "ff_swarm.h"

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
        TEST_ASSERT_TRUE(p->base_r > 0.0f && p->base_r <= 1.0f);
        TEST_ASSERT_TRUE(p->base_theta_deg >= 0.0f && p->base_theta_deg < 360.0f);
        TEST_ASSERT_EQUAL_FLOAT(p->base_r, p->r);
        TEST_ASSERT_EQUAL_FLOAT(p->base_theta_deg, p->theta_deg);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, p->lean);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, p->flare);
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
        if (sw.particles[i].base_theta_deg != 0.0f) any_nonzero_theta = true;
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

/* A beat pulls particles inward (their CURRENT r drops toward a smaller
 * target than a quiet, beat-free run at the same elapsed time) -
 * proxy-resistant: measures actual radius, not just a "did something
 * change" flag. */
static void a_beat_leans_particles_toward_centre(void)
{
    ff_swarm_t with_beat, without_beat;
    ff_swarm_init(&with_beat, 42u);
    ff_swarm_init(&without_beat, 42u);

    ff_swarm_step(&with_beat, 1.0f, true, 1.0f / 30.0f);
    ff_swarm_step(&without_beat, 1.0f, false, 1.0f / 30.0f);

    for (uint32_t i = 0; i < FF_SWARM_PARTICLE_COUNT; i++) {
        TEST_ASSERT_LESS_THAN_FLOAT(without_beat.particles[i].r, with_beat.particles[i].r - 0.0001f);
    }
}

/* Loudness raises glow; silence (loudness 0, never a beat) settles to
 * the idle floor, never fully dark. */
static void loudness_raises_glow_and_idle_never_goes_dark(void)
{
    ff_swarm_t quiet, loud;
    ff_swarm_init(&quiet, 9u);
    ff_swarm_init(&loud, 9u);
    for (int i = 0; i < 60; i++) { /* 2s @ 30fps, long enough to settle */
        ff_swarm_step(&quiet, 0.0f, false, 1.0f / 30.0f);
        ff_swarm_step(&loud, 1.0f, false, 1.0f / 30.0f);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.02f, FF_SWARM_IDLE_GLOW, quiet.particles[0].glow);
    TEST_ASSERT_GREATER_THAN_FLOAT(quiet.particles[0].glow, loud.particles[0].glow);
    TEST_ASSERT_TRUE(quiet.particles[0].glow > 0.0f); /* never fully dark */
}

/* A lean fully decays back out after several decay windows with no
 * further beats — "then wander off again". */
static void lean_decays_back_out_after_the_beat(void)
{
    ff_swarm_t sw;
    ff_swarm_init(&sw, 5u);
    ff_swarm_step(&sw, 1.0f, true, 1.0f / 30.0f);
    float const leaned_r = sw.particles[0].r;
    for (int i = 0; i < 90; i++) { /* 3s of no further beats - several lean-decay windows */
        ff_swarm_step(&sw, 0.0f, false, 1.0f / 30.0f);
    }
    TEST_ASSERT_GREATER_THAN_FLOAT(leaned_r, sw.particles[0].r);
    /* `lean` itself (not `r`, which keeps wandering by its own bounded
     * r_amp even once fully settled — see ff_swarm_step's own wander
     * term) is the field that must actually reach ~0. */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, sw.particles[0].lean);
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
    TEST_ASSERT_EQUAL_FLOAT(before.r, sw.particles[0].r);
    TEST_ASSERT_EQUAL_FLOAT(before.theta_deg, sw.particles[0].theta_deg);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(init_seeds_all_60_particles_in_bounds);
    RUN_TEST(zero_seed_is_remapped_not_degenerate);
    RUN_TEST(same_seed_same_stimulus_is_bit_reproducible);
    RUN_TEST(different_seeds_produce_different_layouts);
    RUN_TEST(a_beat_leans_particles_toward_centre);
    RUN_TEST(loudness_raises_glow_and_idle_never_goes_dark);
    RUN_TEST(lean_decays_back_out_after_the_beat);
    RUN_TEST(null_and_nonpositive_dt_are_safe);
    return UNITY_END();
}
