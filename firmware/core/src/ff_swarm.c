/**
 * ff_swarm.c — see ff_swarm.h.
 */
#include "ff_swarm.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* xorshift32 — a small, fast, fully-deterministic PRNG (Marsaglia,
 * "Xorshift RNGs", 2003) used ONLY to derive each particle's fixed
 * "personality" at init time (initial radius/angle, drift velocities,
 * twinkle phase) — never touched again by ff_swarm_step, which is
 * otherwise a pure function of its own arguments. Never seeded with 0
 * (see ff_swarm_init's own doc comment: x^=x<<13 etc. all fix 0 at 0
 * forever). */
static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* [0,1) from the top 24 bits — plenty of resolution for a cosmetic
 * particle-personality draw, and avoids the low bits of a linear-
 * feedback generator (weaker statistically) ever surfacing directly. */
static float rand01(uint32_t *state)
{
    return (float)(xorshift32(state) >> 8) / (float)(1u << 24);
}

/* [-1,1) — the shared "signed unit draw" every per-particle drift
 * velocity below is scaled from. */
static float rand_signed(uint32_t *state)
{
    return rand01(state) * 2.0f - 1.0f;
}

static float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

void ff_swarm_init(ff_swarm_t *sw, uint32_t seed)
{
    if (sw == NULL) return;
    /* Zero the WHOLE struct first, padding bytes included — `bool
     * is_accent_green` leaves 3 bytes of compiler-inserted padding
     * before the next float field that no individual field assignment
     * below ever touches. Two independently-`ff_swarm_init`ed structs
     * are otherwise bit-identical in every field they set but can still
     * carry different garbage in that padding (whatever was already on
     * the stack/heap at each one's address) — exactly the failure
     * `same_seed_same_stimulus_is_bit_reproducible`'s raw
     * `TEST_ASSERT_EQUAL_MEMORY` catches, since it compares padding too. */
    memset(sw, 0, sizeof(*sw));
    uint32_t state = (seed == 0u) ? FF_SWARM_DEFAULT_SEED : seed;
    float const slot_deg = 360.0f / (float)FF_SWARM_PARTICLE_COUNT;
    float const r_span = FF_SWARM_R_MAX_PX - FF_SWARM_R_MIN_PX;
    float const rad_to_deg = 180.0f / (float)M_PI;

    for (uint32_t i = 0; i < FF_SWARM_PARTICLE_COUNT; i++) {
        ff_swarm_particle_t *p = &sw->particles[i];

        /* Initial angle: one evenly-spaced slot per particle plus
         * jitter within that slot (never a purely random draw for 60
         * points — risks visibly uneven clumping/gaps on a small round
         * display at t==0; even slots + jitter reads as an organic
         * swarm without that risk, and the continuous per-particle
         * angular drift below means they don't stay evenly spaced for
         * long anyway — this only shapes the FIRST frame). */
        float const jitter_deg = (rand01(&state) - 0.5f) * slot_deg;
        p->theta_deg = fmodf((float)i * slot_deg + jitter_deg + 360.0f, 360.0f);

        /* Initial radius: uniform across the full drift range. */
        p->wander_r_px = FF_SWARM_R_MIN_PX + rand01(&state) * r_span;
        p->r_px = p->wander_r_px;

        /* Fixed drift velocities — "each with its own angular drift
         * (+/-0.2 rad/s) and radial drift (+/-12 px/s)". */
        p->angular_speed_deg_s = rand_signed(&state) * FF_SWARM_ANGULAR_DRIFT_MAX_RAD_S * rad_to_deg;
        p->radial_speed_px_s = rand_signed(&state) * FF_SWARM_RADIAL_DRIFT_MAX_PX_S;

        p->twinkle_phase = rand01(&state);
        p->is_accent_green = ((i % FF_SWARM_GREEN_EVERY_N) == (FF_SWARM_GREEN_EVERY_N - 1u));

        /* No beat has ever been seen yet — glow starts at the idle
         * floor alone (envelope 0), before this firefly's own twinkle
         * is even known; the very first ff_swarm_step call establishes
         * the real value. */
        p->glow = FF_SWARM_GLOW_IDLE_BASE;
    }

    sw->rng_state = state;
    sw->t_s = 0.0f;
    sw->time_since_beat_s = 0.0f;
    sw->beat_interval_s = 0.0f;
    sw->beat_index = 0u;
    sw->drop_active = false;
    sw->envelope = 0.0f;
}

void ff_swarm_step(ff_swarm_t *sw, float loudness, bool beat_now, float dt_s)
{
    if (sw == NULL || dt_s <= 0.0f) return;
    float const l = clamp01(loudness);

    sw->t_s += dt_s;
    sw->time_since_beat_s += dt_s;

    if (beat_now) {
        /* The gap since the PREVIOUS beat becomes the interval this
         * envelope decays over for the period that follows — the
         * "assume the next interval looks like the last one" posture
         * FF_SWARM_DEFAULT_BEAT_INTERVAL_S's own doc comment describes.
         * Skipped on the very first beat ever seen (time_since_beat_s
         * is time-since-init, not a real gap). */
        if (sw->beat_index > 0u) {
            sw->beat_interval_s = sw->time_since_beat_s;
        }
        sw->time_since_beat_s = 0.0f;
        sw->beat_index++;
        sw->drop_active = ((sw->beat_index % FF_SWARM_DROP_EVERY_N_BEATS) == 0u);
    }

    /* Shared envelope — identical for every firefly this step, per
     * ff_swarm_step's own doc comment ("a shared pulse, not a
     * staggered per-particle one"). Stays at 0 until the first beat
     * has ever been seen (no fabricated pulse before any real beat). */
    float envelope = 0.0f;
    if (sw->beat_index > 0u) {
        float const interval = (sw->beat_interval_s > 0.0f) ? sw->beat_interval_s : FF_SWARM_DEFAULT_BEAT_INTERVAL_S;
        float const phase = clamp01(sw->time_since_beat_s / interval);
        envelope = expf(-phase * FF_SWARM_ENVELOPE_DECAY_RATE);
    }
    sw->envelope = envelope;

    float const pull_strength = clamp01(envelope * (FF_SWARM_PULL_LOUDNESS_BASE + l));
    float const drop_term = sw->drop_active ? 1.0f : 0.0f;
    float const shared_glow = clamp01(FF_SWARM_GLOW_IDLE_BASE
                                       + FF_SWARM_GLOW_LOUDNESS_GAIN * envelope * l
                                       + FF_SWARM_GLOW_DROP_GAIN * drop_term * envelope);

    for (uint32_t i = 0; i < FF_SWARM_PARTICLE_COUNT; i++) {
        ff_swarm_particle_t *p = &sw->particles[i];

        /* Continuous drift — never stops, even at loudness 0 with no
         * beat ("continuously wandering; never static even in
         * silence"). */
        p->theta_deg = fmodf(p->theta_deg + p->angular_speed_deg_s * dt_s + 360.0f, 360.0f);

        p->wander_r_px += p->radial_speed_px_s * dt_s;
        if (p->wander_r_px > FF_SWARM_R_MAX_PX) {
            p->wander_r_px = FF_SWARM_R_MAX_PX - (p->wander_r_px - FF_SWARM_R_MAX_PX);
            p->radial_speed_px_s = -p->radial_speed_px_s;
        } else if (p->wander_r_px < FF_SWARM_R_MIN_PX) {
            p->wander_r_px = FF_SWARM_R_MIN_PX + (FF_SWARM_R_MIN_PX - p->wander_r_px);
            p->radial_speed_px_s = -p->radial_speed_px_s;
        }

        /* Rendered radius: the free drift trajectory, lerped toward the
         * shared pull target by the shared pull strength — see
         * ff_swarm_step's own doc comment, step 2. */
        p->r_px = p->wander_r_px + (FF_SWARM_PULL_TARGET_R_PX - p->wander_r_px) * pull_strength;

        float const twinkle = FF_SWARM_TWINKLE_FLOOR + FF_SWARM_TWINKLE_GAIN *
            fmaxf(0.0f, sinf((sw->t_s + p->twinkle_phase * FF_SWARM_TWINKLE_PHASE_GAIN) * FF_SWARM_TWINKLE_RATE));
        p->glow = clamp01(shared_glow * twinkle);
    }
}
