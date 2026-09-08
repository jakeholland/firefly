/**
 * ff_swarm.c — see ff_swarm.h.
 */
#include "ff_swarm.h"

#include <math.h>
#include <stddef.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* xorshift32 — a small, fast, fully-deterministic PRNG (Marsaglia,
 * "Xorshift RNGs", 2003) used ONLY to derive each particle's fixed
 * "personality" at init time (home radius/angle, wander speed/phase) —
 * never touched again by ff_swarm_step, which is otherwise a pure
 * function of its own arguments. Never seeded with 0 (see
 * ff_swarm_init's own doc comment: x^=x<<13 etc. all fix 0 at 0
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

static float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* Same one-pole-toward-target shape ff_beat.c's own helper uses
 * (attack when rising toward target, release when falling) — kept as
 * an independent copy rather than a shared header: both are tiny,
 * file-local, and a shared helper header would couple two otherwise
 * unrelated core modules for one three-line function. */
static float one_pole_toward(float current, float target, float dt_ms, float tau_ms)
{
    float alpha = (tau_ms > 0.0f) ? (dt_ms / tau_ms) : 1.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    if (alpha < 0.0f) alpha = 0.0f;
    return current + alpha * (target - current);
}

void ff_swarm_init(ff_swarm_t *sw, uint32_t seed)
{
    if (sw == NULL) return;
    uint32_t state = (seed == 0u) ? FF_SWARM_DEFAULT_SEED : seed;
    float const slot_deg = 360.0f / (float)FF_SWARM_PARTICLE_COUNT;

    for (uint32_t i = 0; i < FF_SWARM_PARTICLE_COUNT; i++) {
        ff_swarm_particle_t *p = &sw->particles[i];

        /* Home angle: one evenly-spaced slot per particle plus jitter
         * within that slot (never fully random placement — a purely
         * random draw for 60 points risks visibly uneven clumping/gaps
         * on a small round display; even slots + jitter reads as an
         * organic swarm without that risk). */
        float const jitter_deg = (rand01(&state) - 0.5f) * slot_deg;
        p->base_theta_deg = fmodf((float)i * slot_deg + jitter_deg + 360.0f, 360.0f);
        /* Home radius: spread across the middle-to-outer glass, never
         * dead center (the clock/chrome sits there) and never past the
         * very edge (leaves the glow visible under the bezel). */
        p->base_r = 0.30f + 0.65f * rand01(&state);

        p->phase = rand01(&state) * 2.0f * (float)M_PI;
        p->wander_speed = 0.15f + rand01(&state) * 0.35f; /* rad/s */
        p->theta_amp_deg = 4.0f + rand01(&state) * 10.0f;
        p->r_amp = 0.04f + rand01(&state) * 0.10f;

        p->r = p->base_r;
        p->theta_deg = p->base_theta_deg;
        p->lean = 0.0f;
        p->flare = 0.0f;
        p->glow = FF_SWARM_IDLE_GLOW;
    }
    sw->rng_state = state;
}

void ff_swarm_step(ff_swarm_t *sw, float loudness, bool beat_now, float dt_s)
{
    if (sw == NULL || dt_s <= 0.0f) return;
    float const l = clamp01(loudness);
    float const dt_ms = dt_s * 1000.0f;

    for (uint32_t i = 0; i < FF_SWARM_PARTICLE_COUNT; i++) {
        ff_swarm_particle_t *p = &sw->particles[i];

        /* Decay any existing lean/flare toward 0 first, THEN retrigger
         * on a beat (rather than a bare assignment) — a beat landing
         * before the previous one's lean has fully settled boosts it
         * further, which reads as the swarm keeping pace with a fast
         * tempo rather than resetting each time. */
        p->lean = one_pole_toward(p->lean, 0.0f, dt_ms, (float)FF_SWARM_LEAN_DECAY_MS);
        p->flare = one_pole_toward(p->flare, 0.0f, dt_ms, (float)FF_SWARM_FLARE_DECAY_MS);
        if (beat_now) {
            p->lean = clamp01(p->lean + l);
            p->flare = clamp01(p->flare + l);
        }

        /* Wander: a slow per-particle phase accumulator drives a small
         * sinusoidal drift around the particle's own home position —
         * organic, bounded, and fully deterministic. */
        p->phase += p->wander_speed * dt_s;
        if (p->phase > 2.0f * (float)M_PI) {
            p->phase = fmodf(p->phase, 2.0f * (float)M_PI);
        }
        float const wander_theta = p->theta_amp_deg * sinf(p->phase);
        float const wander_r = p->r_amp * sinf(p->phase * 0.7f + 1.0f);

        /* Beat-driven pull toward centre, proportional to `lean` (which
         * itself decays over FF_SWARM_LEAN_DECAY_MS after each beat) —
         * "flare and lean toward the centre, then wander off again". */
        float target_r = (p->base_r + wander_r) * (1.0f - FF_SWARM_MAX_PULL * p->lean);
        if (target_r < 0.02f) target_r = 0.02f; /* never collapse onto the centre clock chip */
        float const target_theta = p->base_theta_deg + wander_theta;

        p->r = one_pole_toward(p->r, target_r, dt_ms, (float)FF_SWARM_EASE_MS);

        float dtheta = target_theta - p->theta_deg;
        while (dtheta > 180.0f) dtheta -= 360.0f;
        while (dtheta < -180.0f) dtheta += 360.0f;
        float const theta_alpha = clamp01(dt_ms / (float)FF_SWARM_EASE_MS);
        p->theta_deg = fmodf(p->theta_deg + theta_alpha * dtheta + 360.0f, 360.0f);

        /* Glow: an idle floor (never fully dark), a loudness-driven
         * ambient term, and the transient flare flash on top — "Loudness
         * sets how bright the swarm glows... silence lets it fade back
         * toward the ordinary idle look" (the concept sheet). */
        p->glow = clamp01(FF_SWARM_IDLE_GLOW + l * 0.5f + p->flare * 0.6f);
    }
}
