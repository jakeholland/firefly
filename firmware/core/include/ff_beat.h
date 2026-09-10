/**
 * ff_beat.h — S31 Music/Swarm: pure, host-testable BEAT + LOUDNESS
 * detection shared by the esp32s3 device's Music face wiring
 * (`firmware/app/ff_shell.c`, fed from `ff_mic_level()` /
 * `ff_compass_last_accel_board()`) and this repo's Unity tests. Zero
 * I/O, zero hardware knowledge — the `firmware/core/` placement rule
 * (CLAUDE.md: "All logic goes in firmware/core/ ... no I/O") applied to
 * "is there a beat right now, and how loud is it", the same split
 * `ff_miclevel.h` already draws between the pure RMS/envelope math and
 * `ff_mic.h`'s device HAL.
 *
 * Spec: docs/specs/S31-music-swarm.md.
 *
 * ## Two independent sources, one output shape
 * `ff_beat_update` takes a caller-supplied `ff_beat_sample_t` tagged
 * with WHICH source it came from (`FF_BEAT_SOURCE_MIC` /
 * `FF_BEAT_SOURCE_IMU` / `FF_BEAT_SOURCE_NONE`) — the caller (the
 * shell) decides source selection (mic present -> MIC, else IMU present
 * -> IMU, else NONE — S31's own "honesty" rule: the source actually used
 * is always the one shown on glass, never invented). This module never
 * blends or averages the two; a single `ff_beat_t` instance tracks
 * whichever source is currently feeding it, and `ff_beat_reset` clears
 * all of it (attack/release state, refractory, floor/ceiling) so a
 * source switch (or a Music-face re-entry) never carries stale state
 * from a previous session/source.
 *
 * ## Loudness: auto-ranging floor/ceiling
 * `env_dbfs` (MIC) is DIRECTLY dBFS (`ff_miclevel_to_dbfs`'s
 * convention). For IMU, the caller hands `accel_mag_g` (the dynamic —
 * gravity-removed — component of the vertical-axis accelerometer
 * reading, in g); this module converts it to the SAME pseudo-dBFS scale
 * via `20*log10(accel_mag_g / FF_BEAT_IMU_REF_G)` (floored, never -inf)
 * so exactly one auto-ranging/mapping code path serves both sources —
 * see `ff_beat_level_dbfs` below.
 *
 * A quiet tent and a loud stage both need to use the full [0,1]
 * loudness range, so the floor and ceiling this module maps against are
 * not fixed constants — they SLOWLY track what this puck has actually
 * heard/felt:
 *   - `floor_dbfs` chases a newly QUIETER level fast (attack
 *     `FF_BEAT_FLOOR_ATTACK_MS`, ~1s) — so a sudden hush is reflected
 *     quickly — and creeps back UP slowly when the level stays above it
 *     (release `FF_BEAT_FLOOR_RELEASE_MS`, ~20s) — so a floor set during
 *     a loud stretch does not stay pinned low forever once things quiet
 *     back down.
 *   - `ceil_dbfs` mirrors this: chases a newly LOUDER level fast
 *     (`FF_BEAT_CEIL_ATTACK_MS`, ~1s — a stage's loud level is captured
 *     quickly) and decays back DOWN slowly when nothing that loud
 *     recurs (`FF_BEAT_CEIL_RELEASE_MS`, ~20s — one clap does not
 *     permanently peg the ceiling).
 *   - the two are clamped `FF_BEAT_RANGE_FLOOR_DB` (12 dB) apart at
 *     minimum, so a dead-silent room (no signal variation at all) never
 *     divides by (near) zero or turns ordinary noise-floor jitter into
 *     wild loudness swings.
 * `loudness = clamp01((level_dbfs - floor_dbfs) / (ceil_dbfs -
 * floor_dbfs))`.
 *
 * ## Beat/onset detection
 * MIC path: a classic fast-envelope-vs-slow-average onset detector —
 * `fast_env` (attack `FF_BEAT_FAST_ATTACK_MS` ~5ms, release
 * `FF_BEAT_FAST_RELEASE_MS` ~50ms) tracks transients; `slow_env`
 * (attack `FF_BEAT_SLOW_ATTACK_MS` ~100ms, release
 * `FF_BEAT_SLOW_RELEASE_MS` ~400ms) tracks the recent average. A beat
 * fires when `fast_env - slow_env >= FF_BEAT_ONSET_THRESHOLD_DB` (6 dB)
 * AND the refractory window (`FF_BEAT_REFRACTORY_MS`, 250ms) has
 * elapsed since the last beat.
 *
 * IMU path: vertical-axis bounce PEAK-picking on `accel_mag_g` directly
 * (a local maximum above `FF_BEAT_IMU_PEAK_THRESHOLD_G`), gated by the
 * SAME `FF_BEAT_REFRACTORY_MS` window — see `ff_beat_update`'s own
 * implementation comment for why a peak detector (not the fast/slow
 * dB-domain detector) fits a bounce signal better: a footstep/dance
 * bounce is a single, roughly-sinusoidal excursion, not a broadband
 * transient riding on a steady background the way a clap/kick riding on
 * ambient room noise is.
 *
 * `beat_count` is a monotonic counter (never reset except by
 * `ff_beat_reset`) — callers detect "a new beat happened since I last
 * looked" by diffing this value rather than sampling a transient
 * per-tick edge flag, which a caller polling slower than this module is
 * updated (the Music face's own 15-30fps redraw vs. this module's
 * ~50Hz nominal input rate) could otherwise miss entirely. `bpm_estimate`
 * is `60000 / (now_ms - previous_beat_ms)` at the moment of the SECOND
 * and every later beat (0 before two beats have ever been seen —
 * honestly "no estimate yet", never a fabricated tempo).
 */
#ifndef FF_BEAT_H
#define FF_BEAT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Which input is currently feeding the detector. `FF_BEAT_SOURCE_NONE`
 *  is the zero value — the least-claiming default, this codebase's
 *  standing enum convention (see e.g. `ff_app_face_t`'s own top
 *  comment) — and `ff_beat_update`'s honest answer when the caller
 *  itself has neither a mic nor an IMU to offer. */
typedef enum {
    FF_BEAT_SOURCE_NONE = 0,
    FF_BEAT_SOURCE_MIC,
    FF_BEAT_SOURCE_IMU,
} ff_beat_source_t;

/** One input sample. Only the fields matching `source` are read —
 *  see this header's top comment. */
typedef struct {
    ff_beat_source_t source;
    float rms_dbfs;      /* FF_BEAT_SOURCE_MIC only — informational, not used by the detector itself */
    float env_dbfs;      /* FF_BEAT_SOURCE_MIC only — the envelope-followed level the loudness/onset math runs on */
    float accel_mag_g;   /* FF_BEAT_SOURCE_IMU only — dynamic (gravity-removed) vertical-axis magnitude, in g */
} ff_beat_sample_t;

/* -------------------------------------------------------------------
 * Time constants — see this header's top comment, "Loudness"/"Beat
 * detection" sections, for the reasoning behind each. All flagged per
 * AGENTS.md's "note the interpretation" rule: no existing spec pinned
 * any of these before S31 (which this same change introduces).
 * ------------------------------------------------------------------- */
#define FF_BEAT_FLOOR_ATTACK_MS   1000u
#define FF_BEAT_FLOOR_RELEASE_MS  20000u
#define FF_BEAT_CEIL_ATTACK_MS    1000u
#define FF_BEAT_CEIL_RELEASE_MS   20000u
#define FF_BEAT_RANGE_FLOOR_DB    12.0f /* minimum ceil_dbfs - floor_dbfs spread */

#define FF_BEAT_FAST_ATTACK_MS    5u
#define FF_BEAT_FAST_RELEASE_MS   50u
#define FF_BEAT_SLOW_ATTACK_MS    100u
#define FF_BEAT_SLOW_RELEASE_MS   400u
#define FF_BEAT_ONSET_THRESHOLD_DB 6.0f
#define FF_BEAT_REFRACTORY_MS     250u

/** Reference g for the IMU's pseudo-dBFS conversion (see this header's
 *  top comment) — a bounce this size (an ordinary steady bob) reads as
 *  0 "dBFS" on the shared loudness scale; a harder stomp reads louder. */
#define FF_BEAT_IMU_REF_G          0.5f
#define FF_BEAT_IMU_PEAK_THRESHOLD_G 0.12f

/** Never returned below this by `ff_beat_level_dbfs` — mirrors
 *  `FF_MICLEVEL_FLOOR_DBFS` (ff_miclevel.h)'s own "a real, comparable
 *  number, never -inf" contract, independently so this header does not
 *  need to include ff_miclevel.h for one constant (see ff_theme.h's own
 *  "dependency-light header" precedent for the same tradeoff). */
#define FF_BEAT_FLOOR_DBFS (-120.0f)

/** The QUIET/LOUD word threshold — S31's own binary chrome word, shared
 *  verbatim by `ff_shell.c`'s render-key bucketing and `scr_music.c`'s
 *  own word rendering so the two can never disagree (the exact drift
 *  S16's 2026-09-08 render-key-churn fix names as the failure mode to
 *  avoid — see docs/specs/S16-app-shell.md's Amendments). */
#define FF_BEAT_LOUD_THRESHOLD 0.5f

/* FF_BEAT_KEEPAWAKE_LOUDNESS — REMOVED, fix/s31-music-idle-drain,
 * 2026-09-09 amendment (docs/specs/S31-music-swarm.md). This constant
 * used to gate `ff_shell_keep_awake`'s Music branch: "the face keeps the
 * puck awake only while loudness is above this floor". Bench evidence
 * (Jake's puck, main 51c5d16, left on Music overnight on USB) showed why
 * that never actually released the hold: `loudness` is computed against
 * `ff_beat_t`'s own AUTO-RANGING floor/ceiling (this header's "Loudness"
 * section above) — the floor chases whatever the room's ambient level
 * IS, with only a ~20s release time constant, so ordinary night-quiet
 * background noise sits jittering just above that self-tracking floor
 * indefinitely, never settling all the way down to 0. The mic ran and
 * the screen sat at 90% for 6.6 hours straight with nobody in the room.
 * The fix removes the level-based keep-awake ENTIRELY: Music now obeys
 * the exact same S26 DIM-at-15s/OFF-at-30s-since-last-INPUT policy as
 * every other face (ff_shell_keep_awake, app/ff_shell.c) — sound is
 * never an input. See that function's own comment for the removal, and
 * `ff_shell_music_wants_mic`'s doc comment (app/include/ff_shell.h) for
 * the mic-specific half of this same fix. */

typedef struct {
    /* auto-ranging floor/ceiling (loudness mapping) */
    float floor_dbfs;
    float ceil_dbfs;
    bool  ranging_init; /* false until the first sample seeds floor/ceil, so a fresh reset never maps against 0/0 */

    /* MIC onset detector */
    float fast_env_dbfs;
    float slow_env_dbfs;

    /* IMU onset (peak-pick) detector */
    float imu_prev_mag_g;
    bool  imu_rising;
    bool  imu_have_prev; /* false until the first IMU sample, so the very first sample never reads as a "falling edge" */

    uint32_t refractory_until_ms; /* shared by both onset paths — a beat from either never fires inside the other's cooldown */

    /* output */
    ff_beat_source_t source;
    float    loudness;    /* [0,1] */
    uint32_t beat_count;   /* monotonic; see this header's top comment on why callers diff this instead of an edge flag */
    uint32_t last_beat_ms; /* meaningful only once beat_count > 0 */
    float    bpm_estimate; /* 0 until a second beat has been seen */
} ff_beat_t;

/** ff_beat_reset — zero every field to the least-claiming state
 *  (source NONE, loudness 0, no beats yet). Call once when a Music
 *  session starts (or on every face re-entry — see ff_shell.c's own
 *  face-transition-cleanup block) so a previous session's floor/
 *  ceiling/refractory state never leaks into a fresh one — mirrors
 *  `ff_miclevel_dc_reset`'s / `ff_batt_filter_t`'s own reset-on-restart
 *  discipline. */
void ff_beat_reset(ff_beat_t *b);

/** ff_beat_update — advance the detector by one sample. `dt_ms` is the
 *  caller's own elapsed-time-since-last-update (explicit, not assumed —
 *  same convention as `ff_miclevel_envelope_update`'s `dt_ms` and
 *  `ff_batt_filter_push`'s `now_ms`), so a caller polling faster or
 *  slower than the mic's own nominal 50Hz frame rate still integrates
 *  correctly. `now_ms` is used only for the refractory window and the
 *  BPM estimate's inter-beat interval. `b == NULL` or `sample == NULL`
 *  is a safe no-op. `sample->source == FF_BEAT_SOURCE_NONE` decays
 *  loudness toward 0 (via the same release-rate floor/ceiling
 *  machinery, so it does not snap to 0 the instant a source is lost)
 *  and never fires a beat. */
void ff_beat_update(ff_beat_t *b, ff_beat_sample_t const *sample, uint32_t dt_ms, uint32_t now_ms);

/** ff_beat_level_dbfs — the shared MIC/IMU-to-pseudo-dBFS conversion
 *  this header's top comment describes, exposed directly so a caller
 *  (or a test) can reason about the mapping independent of the
 *  stateful detector. For `FF_BEAT_SOURCE_MIC`, this is `env_dbfs`
 *  verbatim; for `FF_BEAT_SOURCE_IMU`, `20*log10(accel_mag_g /
 *  FF_BEAT_IMU_REF_G)`, floored at `FF_BEAT_FLOOR_DBFS`; for
 *  `FF_BEAT_SOURCE_NONE`, `FF_BEAT_FLOOR_DBFS`. */
float ff_beat_level_dbfs(ff_beat_sample_t const *sample);

#ifdef __cplusplus
}
#endif

#endif /* FF_BEAT_H */
