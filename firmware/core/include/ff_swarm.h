/**
 * ff_swarm.h — S31 Music/Swarm: pure, host-testable, DETERMINISTIC
 * 60-firefly particle simulation for the Music face's "Swarm" concept
 * (docs/specs/S31-music-swarm.md). Zero I/O, zero LVGL — the
 * `firmware/core/` placement rule applied to "where is each firefly and
 * how bright is it", exactly the split `ff_radar_layout`/`radar_layout.c`
 * already draw between core geometry and `scr_radar.c`'s LVGL drawing.
 *
 * ## Ownership — NOT part of ff_app_state_t
 * Unlike every other per-face view struct in `ff_app_state.h`, an
 * `ff_swarm_t` instance is NOT projected into the shell's render key:
 * it changes every frame (drift, beat pulls, flare decay) and the
 * render key drives a full `lv_obj_clean` + rebuild on every dirty tick
 * (S16's own rule) — putting 60 particles' continuous motion in there
 * would either force a full-screen rebuild every frame (an S16 churn-
 * budget violation and, per the Map face's own measured cost, a real
 * stall risk) or need the same field-by-field masking every other
 * "must not churn" field gets, at 60x the surface area. Instead
 * `scr_music.c` owns one `ff_swarm_t` instance itself (a file-static,
 * the same "screen owns its own pool" convention `scr_map.c`'s draw-op
 * pool already establishes) and steps/reads it from its own per-frame
 * LVGL timer — see that file's top comment for the full mechanism.
 *
 * ## Determinism
 * `ff_swarm_init(seed)` seeds a small xorshift32 PRNG and derives every
 * particle's fixed "personality" (home radius, home angle, wander
 * speed/phase) from it ONCE — nothing here calls a platform RNG or
 * reads wall-clock time. `ff_swarm_step` is a pure function of
 * (current state, loudness, beat_now, dt_s): the SAME seed fed the SAME
 * sequence of (loudness, beat_now, dt_s) calls always produces the SAME
 * particle positions, which is what makes the Music golden fixtures
 * (`music_swarm_*.json`) reproducible pixel-for-pixel.
 */
#ifndef FF_SWARM_H
#define FF_SWARM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FF_SWARM_PARTICLE_COUNT 60u

/** Default seed used whenever nothing more specific is asked for (the
 *  live shell's own default, and every fixture/golden that doesn't
 *  override it via `music.seed` — see tests/fixtures/README.md). Not
 *  0: xorshift32 is fixed at exactly 0 forever if seeded with 0, so 0
 *  is remapped to this value by `ff_swarm_init` regardless of caller —
 *  see that function's own doc comment. */
#define FF_SWARM_DEFAULT_SEED 0x51E9FF17u

/** Beat-driven "lean toward centre" — how much of a particle's home
 *  radius a full-intensity (loudness 1.0), fully-leaned-in beat pulls
 *  it toward the centre. 0.6 reads clearly as "leaning in" on glass
 *  without particles ever stacking on the exact centre (where the wall
 *  clock chip sits) even at max loudness. Interpretation call, flagged
 *  per AGENTS.md — no spec pins this exact fraction. */
#define FF_SWARM_MAX_PULL 0.6f

/** Per-beat lean/flare decay — see docs/specs/S31-music-swarm.md,
 *  "glow radius = f(loudness, beat flare with ~150ms decay)". Lean
 *  (the radial pull) decays slower than the glow flash itself so the
 *  "lean toward the centre, then wander off again" motion reads as a
 *  deliberate settle, not a snap. */
#define FF_SWARM_FLARE_DECAY_MS 150u
#define FF_SWARM_LEAN_DECAY_MS  450u

/** How quickly a particle's rendered radius eases toward its current
 *  target (home radius adjusted by lean) — a short critically-damped-
 *  feeling ease so beat pulls read as motion, not a teleport. */
#define FF_SWARM_EASE_MS 120u

/** Idle glow floor — "silence lets it fade back toward the ordinary
 *  idle look" (the concept sheet): fireflies never go fully dark at
 *  loudness 0, they settle to a dim, ordinary glow. */
#define FF_SWARM_IDLE_GLOW 0.15f

typedef struct {
    float base_r;       /* home radius, normalized [~0.25, 1.0] of the glass radius — fixed at init, never mutated after */
    float base_theta_deg; /* home angle, fixed at init */
    float phase;          /* wander phase accumulator, radians, advances at wander_speed */
    float wander_speed;   /* rad/s, fixed per-particle at init (small, deterministic spread) */
    float theta_amp_deg;  /* wander angular amplitude, fixed at init */
    float r_amp;           /* wander radial amplitude (fraction of base_r), fixed at init */

    float r;        /* CURRENT rendered radius, normalized [0,1] — eases toward its target each step */
    float theta_deg; /* CURRENT rendered angle [0,360) */
    float lean;      /* [0,1] transient "pulled toward centre" state, decays over FF_SWARM_LEAN_DECAY_MS */
    float flare;      /* [0,1] transient glow-flash state, decays over FF_SWARM_FLARE_DECAY_MS */
    float glow;        /* [0,1] the rendered brightness this step: idle floor + loudness + flare */
} ff_swarm_particle_t;

typedef struct {
    ff_swarm_particle_t particles[FF_SWARM_PARTICLE_COUNT];
    uint32_t rng_state; /* kept only for reproducible re-derivation if ever needed; init() alone fully seeds particles */
} ff_swarm_t;

/** ff_swarm_init — (re)seed every particle's fixed "personality" and
 *  reset its live state (r == base_r, theta == base_theta, no lean/
 *  flare). `seed == 0` is remapped to `FF_SWARM_DEFAULT_SEED` (see that
 *  constant's own doc comment) — `sw == NULL` is a safe no-op. */
void ff_swarm_init(ff_swarm_t *sw, uint32_t seed);

/** ff_swarm_step — advance every particle by `dt_s` seconds of
 *  simulated time. `loudness` ([0,1], clamped) sets the ambient glow
 *  and the intensity a beat leans/flares with; `beat_now` — true on
 *  exactly the step a new beat should register — triggers the
 *  lean-toward-centre + glow flare (scaled by `loudness`) on every
 *  particle at once ("it is the brand, moving" — a shared pulse, not a
 *  staggered per-particle one; see docs/specs/S31-music-swarm.md).
 *  `sw == NULL` or a non-positive `dt_s` is a safe no-op. */
void ff_swarm_step(ff_swarm_t *sw, float loudness, bool beat_now, float dt_s);

#ifdef __cplusplus
}
#endif

#endif /* FF_SWARM_H */
