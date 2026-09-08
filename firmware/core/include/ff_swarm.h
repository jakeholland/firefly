/**
 * ff_swarm.h — S31 Music/Swarm: pure, host-testable, DETERMINISTIC
 * 60-firefly particle simulation for the Music face's "Swarm" concept
 * (docs/specs/S31-music-swarm.md). Zero I/O, zero LVGL — the
 * `firmware/core/` placement rule applied to "where is each firefly and
 * how bright is it", exactly the split `ff_radar_layout`/`radar_layout.c`
 * already draw between core geometry and `scr_radar.c`'s LVGL drawing.
 *
 * ## S31 polish (owner feedback on PR #245, 2026-09-08)
 * Jake, on the field puck: "swarm works, clap flares it and quiet calms
 * it. Would love for it to be more dynamic like the design originally."
 * This motion model replaces PR #245's "wander around a fixed home
 * position" particle with the coordinator's ORIGINAL concept-mockup
 * behaviour: every firefly continuously drifts on its own fixed angular
 * (rad/s) and radial (px/s) velocity — never settling, never static even
 * in total silence — bouncing off the swarm's inner/outer radius bounds
 * forever. A beat no longer "leans" a home position inward; instead it
 * LERPS the rendered radius toward a shared pull target
 * (`FF_SWARM_PULL_TARGET_R_PX`) by a strength that is proportional to
 * the shared beat envelope (see below) and the current loudness, so a
 * strong beat visibly snaps the whole swarm inward and then the pull
 * relaxes back out over the following beat interval as the envelope
 * decays — "then wander off again", now literally riding the same
 * continuous drift trajectory the firefly was already on, not a
 * separate "home" it returns to. Per-firefly TWINKLE (a slow, phase-
 * offset brightness oscillation) is new — it is what keeps a firefly
 * visibly alive even when the shared envelope is fully decayed to
 * near-zero between beats.
 *
 * ## Ownership — NOT part of ff_app_state_t
 * Unlike every other per-face view struct in `ff_app_state.h`, an
 * `ff_swarm_t` instance is NOT projected into the shell's render key:
 * it changes every frame (drift, beat pulls, twinkle) and the render
 * key drives a full `lv_obj_clean` + rebuild on every dirty tick (S16's
 * own rule) — putting 60 particles' continuous motion in there would
 * either force a full-screen rebuild every frame (an S16 churn-budget
 * violation and, per the Map face's own measured cost, a real stall
 * risk) or need the same field-by-field masking every other "must not
 * churn" field gets, at 60x the surface area. Instead `scr_music.c`
 * owns one `ff_swarm_t` instance itself (a file-static, the same
 * "screen owns its own pool" convention `scr_map.c`'s draw-op pool
 * already establishes) and steps/reads it from its own per-frame LVGL
 * timer — see that file's top comment for the full mechanism.
 *
 * ## Determinism
 * `ff_swarm_init(seed)` seeds a small xorshift32 PRNG and derives every
 * particle's fixed "personality" (initial radius/angle, angular/radial
 * drift velocity, twinkle phase, accent color) from it ONCE — nothing
 * here calls a platform RNG or reads wall-clock time. `ff_swarm_step`
 * is a pure function of (current state, loudness, beat_now, dt_s): the
 * SAME seed fed the SAME sequence of (loudness, beat_now, dt_s) calls
 * always produces the SAME particle positions, which is what makes the
 * Music golden fixtures (`music_swarm_*.json`) reproducible
 * pixel-for-pixel.
 *
 * ## Scale: the coordinator's mockup canvas was 600px, the puck is 412px
 * Every absolute-pixel constant below (`FF_SWARM_R_MIN_PX`, `..._MAX_PX`,
 * `..._PULL_TARGET_R_PX`, `..._RADIAL_DRIFT_MAX_PX_S`) is the
 * coordinator's own concept-mockup number — a canvas animation authored
 * at 600px — scaled by `FF_SWARM_MOCK_TO_PUCK_SCALE` (412/600) onto this
 * puck's actual 412px glass (`FF_THEME_PUCK_PX`). 268px (the mockup's
 * own outer radius) would run almost 30px past the visible glass edge
 * (`FF_THEME_GLASS_R` == 200) if used unscaled; the scaled value (~184px)
 * lands comfortably inside it. Angular quantities (drift rad/s, the
 * twinkle formula) need no such scaling — an angle means the same thing
 * on any canvas size.
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

/** The coordinator's concept mockup was authored on a 600px canvas;
 *  this puck's glass is 412px (`FF_THEME_PUCK_PX`) — see this header's
 *  own "Scale" comment above. Applied to every absolute-pixel constant
 *  below. */
#define FF_SWARM_MOCK_TO_PUCK_SCALE (412.0f / 600.0f)

/** Firefly radius bounds, px from centre, on the puck's own 412px
 *  canvas — the mockup's own 60-268px range, scaled. Each firefly
 *  drifts continuously and REFLECTS off these bounds (never clamps to
 *  a hard stop, never a fixed "home") so the swarm is never static,
 *  even in total silence. */
#define FF_SWARM_R_MIN_PX (60.0f  * FF_SWARM_MOCK_TO_PUCK_SCALE)  /* ~41.2px */
#define FF_SWARM_R_MAX_PX (268.0f * FF_SWARM_MOCK_TO_PUCK_SCALE)  /* ~184.1px */

/** Beat-driven pull target radius (px) — every firefly's rendered
 *  radius is lerped toward this on a beat, by a strength proportional
 *  to the shared envelope and loudness (see `ff_swarm_step`'s own doc
 *  comment) — "pulled toward radius ~120px... then drift back out". */
#define FF_SWARM_PULL_TARGET_R_PX (120.0f * FF_SWARM_MOCK_TO_PUCK_SCALE) /* ~82.4px */

/** `strength = envelope * (FF_SWARM_PULL_LOUDNESS_BASE + loudness)`,
 *  clamped to [0,1] — a beat pulls inward even at loudness 0 (the base
 *  term), more so the louder it is. Interpretation call, flagged per
 *  AGENTS.md — the task's own wording ("strength proportional to
 *  envelope*(0.6+loudness)") pins the shape, not a separate spec. */
#define FF_SWARM_PULL_LOUDNESS_BASE 0.6f

/** Each firefly's fixed (per-init) angular/radial drift velocity is
 *  drawn uniformly from [-MAX, +MAX] on these two ranges — "each with
 *  its own angular drift (+/-0.2 rad/s) and radial drift (+/-12 px/s),
 *  continuously wandering". Angular needs no mock-scale conversion (an
 *  angle means the same thing on any canvas size); radial does. */
#define FF_SWARM_ANGULAR_DRIFT_MAX_RAD_S 0.2f
#define FF_SWARM_RADIAL_DRIFT_MAX_PX_S (12.0f * FF_SWARM_MOCK_TO_PUCK_SCALE) /* ~8.24px/s */

/** Shared beat-envelope decay rate — `envelope = exp(-phase*RATE)`
 *  where `phase` is how far (as a fraction of the beat interval) the
 *  sim has advanced since the last beat, clamped to [0,1]. The task's
 *  own stated number. */
#define FF_SWARM_ENVELOPE_DECAY_RATE 5.5f

/** Assumed beat interval (seconds, ~120bpm) used to normalize `phase`
 *  until a real interval has been MEASURED from two actual beats (the
 *  same "never fabricate a tempo before you've seen one" posture
 *  `ff_beat_t.bpm_estimate` already carries). Interpretation call,
 *  flagged per AGENTS.md — no spec pins an exact default. */
#define FF_SWARM_DEFAULT_BEAT_INTERVAL_S 0.5f

/** Every Nth beat gets a stronger "drop" accent on top of the ordinary
 *  envelope-driven glow — the task's own "a stronger accent every 8th
 *  beat". */
#define FF_SWARM_DROP_EVERY_N_BEATS 8u

/** glow = clamp01(GLOW_IDLE_BASE + GLOW_LOUDNESS_GAIN*envelope*loudness
 *              + GLOW_DROP_GAIN*drop*envelope) * twinkle
 *  — the task's own formula, verbatim. `drop` is 1.0 for every step
 *  during the beat interval that immediately follows an 8th beat, 0.0
 *  otherwise (see `ff_swarm_step`'s own doc comment). */
#define FF_SWARM_GLOW_IDLE_BASE     0.25f
#define FF_SWARM_GLOW_LOUDNESS_GAIN 0.75f
#define FF_SWARM_GLOW_DROP_GAIN     0.5f

/** twinkle = TWINKLE_FLOOR + TWINKLE_GAIN * max(0, sin((t +
 *  twinkle_phase*TWINKLE_PHASE_GAIN) * TWINKLE_RATE)) — the task's own
 *  formula, verbatim. `t` is the swarm's own accumulated simulated
 *  time (seconds since `ff_swarm_init`); `twinkle_phase` is a fixed
 *  per-firefly random offset in [0,1) drawn at init, so every firefly
 *  blinks at a visibly different moment despite sharing the same
 *  3 rad/s rate. */
#define FF_SWARM_TWINKLE_FLOOR      0.35f
#define FF_SWARM_TWINKLE_GAIN       0.65f
#define FF_SWARM_TWINKLE_RATE       3.0f
#define FF_SWARM_TWINKLE_PHASE_GAIN 10.0f

/** Every Nth firefly (0-indexed, so index N-1, 2N-1, ...) is rendered
 *  as the live-green accent color instead of amber — "every 7th one
 *  live-green". A domain fact (which particles are the accent color),
 *  not a rendering detail, so it lives here, not in `scr_music.c` —
 *  same "core decides WHAT, screen decides HOW to draw it" split this
 *  whole module already follows. */
#define FF_SWARM_GREEN_EVERY_N 7u

typedef struct {
    float r_px;        /* CURRENT rendered radius, px from centre — the free
                         * drift trajectory lerped toward the pull target on
                         * a beat (see ff_swarm_step's own doc comment) */
    float theta_deg;     /* CURRENT rendered angle, degrees, 0 == top, clockwise */
    bool  is_accent_green; /* true for every FF_SWARM_GREEN_EVERY_Nth firefly (fixed at init) */
    float glow;            /* [0,1] the rendered brightness this step: envelope/loudness/drop, times this firefly's own twinkle */

    /* --- fixed "personality", set once at init, read-only after --- */
    float wander_r_px;         /* the free (beat-independent) drift radius — reflects off [R_MIN_PX, R_MAX_PX] forever */
    float radial_speed_px_s;   /* fixed drift velocity for wander_r_px; sign flips on each bounce */
    float angular_speed_deg_s; /* fixed drift velocity for theta_deg, +/- FF_SWARM_ANGULAR_DRIFT_MAX_RAD_S in deg/s */
    float twinkle_phase;       /* fixed per-firefly twinkle phase offset, [0,1) */
} ff_swarm_particle_t;

typedef struct {
    ff_swarm_particle_t particles[FF_SWARM_PARTICLE_COUNT];
    uint32_t rng_state; /* kept only for reproducible re-derivation if ever needed; init() alone fully seeds particles */

    float t_s;              /* accumulated simulated time (seconds) since ff_swarm_init — drives twinkle */
    float time_since_beat_s; /* seconds since the most recent beat (0 at init: no beat has ever been seen) */
    float beat_interval_s;   /* the most recently MEASURED gap between two beats; 0 until a second beat has ever been seen (FF_SWARM_DEFAULT_BEAT_INTERVAL_S is used as the phase denominator until then) */
    uint32_t beat_index;      /* count of beats seen since init, 0 until the first */
    bool drop_active;         /* true for every step in the interval immediately following an FF_SWARM_DROP_EVERY_N_BEATS-th beat */
    float envelope;            /* [0,1] this step's shared beat envelope, exp(-phase*FF_SWARM_ENVELOPE_DECAY_RATE); 0 until the first beat has ever been seen */
} ff_swarm_t;

/** ff_swarm_init — (re)seed every particle's fixed "personality" and
 *  reset all live/shared state (r_px == wander_r_px == a random point
 *  in [R_MIN_PX, R_MAX_PX], no beat ever seen). `seed == 0` is remapped
 *  to `FF_SWARM_DEFAULT_SEED` (see that constant's own doc comment) —
 *  `sw == NULL` is a safe no-op. */
void ff_swarm_init(ff_swarm_t *sw, uint32_t seed);

/** ff_swarm_step — advance every particle by `dt_s` seconds of
 *  simulated time. `loudness` ([0,1], clamped) sets the loudness term
 *  of the shared glow and the beat-pull strength; `beat_now` — true on
 *  exactly the step a new beat should register — resets the shared
 *  envelope to 1.0 and, every `FF_SWARM_DROP_EVERY_N_BEATS`th beat,
 *  arms `drop_active` for the interval that follows ("it is the brand,
 *  moving" — a shared pulse, not a staggered per-particle one; see
 *  docs/specs/S31-music-swarm.md). Every firefly then:
 *   1. Advances its own fixed angular/radial drift velocity
 *      (`theta_deg`, `wander_r_px`), reflecting `wander_r_px` off
 *      [`FF_SWARM_R_MIN_PX`, `FF_SWARM_R_MAX_PX`] — this NEVER stops,
 *      even at loudness 0 with no beat, so the swarm is never static.
 *   2. Sets its rendered `r_px` by lerping `wander_r_px` toward
 *      `FF_SWARM_PULL_TARGET_R_PX` by the shared pull strength
 *      (`envelope * (FF_SWARM_PULL_LOUDNESS_BASE + loudness)`, clamped
 *      to [0,1]) — a fresh beat snaps this toward the pull target; as
 *      `envelope` decays over the following beat interval, `r_px`
 *      relaxes back toward wherever `wander_r_px` has drifted to by
 *      then ("then wander off again").
 *   3. Computes its own twinkle (see `FF_SWARM_TWINKLE_*`'s own doc
 *      comment) and sets `glow = clamp01(shared_glow) * twinkle`,
 *      itself always in [0,1] (`FF_SWARM_TWINKLE_FLOOR..+GAIN` sums to
 *      <= 1.0).
 *  `sw == NULL` or a non-positive `dt_s` is a safe no-op. */
void ff_swarm_step(ff_swarm_t *sw, float loudness, bool beat_now, float dt_s);

#ifdef __cplusplus
}
#endif

#endif /* FF_SWARM_H */
