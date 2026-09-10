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
 * Spec: docs/specs/S31-music-swarm.md, including its 2026-09-09
 * amendment (fix/s31-beat-real-audio) this header's own "Beat/onset
 * detection" section below describes.
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
 * all of it (attack/release state, refractory, floor/ceiling, band
 * onset trackers) so a source switch (or a Music-face re-entry) never
 * carries stale state from a previous session/source.
 *
 * ## Loudness: auto-ranging floor/ceiling (UNCHANGED by the 2026-09-09
 * amendment — see that section below for what DID change)
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
 * floor_dbfs))`. This is driven by the BROADBAND `env_dbfs`, exactly as
 * before the 2026-09-09 amendment — that amendment is scoped to the
 * ONSET (beat) detector only, per its own root-cause finding (see
 * below): loudness auto-ranging was never the broken part.
 *
 * ## Beat/onset detection — 2026-09-09 amendment (fix/s31-beat-real-
 * audio, docs/specs/S31-music-swarm.md's dated amendment)
 *
 * ROOT CAUSE this amendment fixes: on-device evidence (Jake's puck,
 * main a853bb5 + coordinator hotfix, real music from a speaker) showed
 * `music source=mic loudness=0.82 bpm=0.0` for an entire 15s set — the
 * OLD MIC detector (a fast-envelope-vs-slow-average crossing on the
 * BROADBAND envelope, tuned against a click-train bench test) never
 * fires on real, dynamic-range-COMPRESSED music: a mastered track sits
 * near the limiter's ceiling throughout, so a kick drum's actual energy
 * punch barely moves the one broadband RMS number the old detector
 * watched, even though the loudness reading (which samples the SAME
 * broadband envelope, just mapped through the floor/ceiling above) was
 * honestly near its own ceiling. A click-train bench test never catches
 * this because a click IS a genuine broadband transient riding on
 * near-silence — exactly the one case the old detector was tuned for.
 *
 * THE FIX: MIC onset detection now runs on BAND-LIMITED energy
 * (`ff_bandenergy.h`, this same amendment) — a LOW band (~60-200Hz,
 * where a kick/bass note's fundamental lives) AND a MID band
 * (~200-2000Hz, where a snare/clap/other percussive-or-melodic
 * transient lives) — rather than the one already-compressed broadband
 * number. Each band's OWN per-frame ENERGY RISE (a half-wave-rectified
 * frame-to-frame delta in dB — "spectral flux" collapsed to two bands,
 * per the deliverable's own "spectral-flux or band-limited energy
 * onset" framing) is compared against an ADAPTIVE threshold: the
 * MEDIAN of that band's own flux over the last ~1.5s
 * (`FF_BEAT_MUSIC_FLUX_WINDOW_N` samples at the ~50Hz nominal input
 * rate) plus a fixed margin (`FF_BEAT_MUSIC_FLUX_MARGIN_DB`), floored
 * at an absolute minimum (`FF_BEAT_MUSIC_FLUX_MIN_DB`) so a truly flat/
 * silent signal (median flux ~0) cannot trip on ordinary numerical
 * jitter. A beat fires when EITHER band's flux clears its own threshold
 * AND the shared refractory window (`FF_BEAT_REFRACTORY_MS`, 250ms,
 * UNCHANGED) has elapsed — "either", not "both", because different
 * genres/mixes push the audible onset into different bands (a
 * four-on-the-floor kick into LOW; a claps/snare-forward mix more into
 * MID) and requiring both would miss real beats a working ear calls
 * obvious.
 *
 * WHY FLUX (a frame-to-frame DELTA), NOT a level-vs-median comparison:
 * a slow swell (loudness rising/falling over several SECONDS — this
 * module's own "no beats" acceptance case) still produces a tiny
 * per-frame delta (a few tenths of a dB at 20ms/frame even at a fast
 * swell rate) — comfortably under the flux margin — while a genuine
 * attack (tens of dB within one or two 20ms frames) clears it easily.
 * Comparing the current LEVEL against a slow-moving statistic instead
 * (an earlier draft of this fix) does not have this property: a smooth
 * multi-second swell can sit several dB above its own trailing median
 * for a sustained stretch purely from the median's own lag, producing
 * exactly the false positives this module's own acceptance tests exist
 * to catch.
 *
 * `beat_count` is a monotonic counter (never reset except by
 * `ff_beat_reset`) — callers detect "a new beat happened since I last
 * looked" by diffing this value rather than sampling a transient
 * per-tick edge flag, which a caller polling slower than this module is
 * updated (the Music face's own 15-30fps redraw vs. this module's
 * ~50Hz nominal input rate) could otherwise miss entirely.
 *
 * `bpm_estimate` (2026-09-09 amendment: now a MEDIAN-smoothed,
 * OCTAVE-FOLDED estimate, not a raw single-interval one) is derived
 * from the last `FF_BEAT_BPM_HISTORY_N` inter-onset intervals: their
 * MEDIAN (resistant to one missed/doubled beat the way a mean is not)
 * converts to a raw BPM, then that raw value is folded by repeated
 * doubling/halving into `[FF_BEAT_BPM_MIN, FF_BEAT_BPM_MAX]`
 * (70-180 BPM) — a real onset detector inevitably sometimes catches a
 * half-note or double-time subdivision instead of the actual beat, and
 * folding into one canonical octave is the standard, honest way to
 * report "the tempo", not a fabricated exact multiple. Still 0 before a
 * second beat has ever been seen (honestly "no estimate yet") and still
 * shared machinery for BOTH sources — MIC's band-onset and IMU's
 * peak-pick each just call the same `beat_fire`, per this header's
 * "IMU path" section below.
 *
 * IMU path (UNCHANGED by this amendment — the deliverable's own "keep
 * the IMU path" instruction): vertical-axis bounce PEAK-picking on
 * `accel_mag_g` directly (a local maximum above
 * `FF_BEAT_IMU_PEAK_THRESHOLD_G`), gated by the SAME
 * `FF_BEAT_REFRACTORY_MS` window — see `ff_beat_update`'s own
 * implementation comment for why a peak detector (not a band/flux
 * detector) fits a bounce signal better: a footstep/dance bounce is a
 * single, roughly-sinusoidal excursion on ONE channel already, not a
 * multi-band audio mixture needing frequency separation at all.
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
 *  see this header's top comment. `low_band_dbfs`/`mid_band_dbfs`
 *  (2026-09-09 amendment) are the `ff_bandenergy.h` band energies for
 *  THIS frame, MIC only — the caller (`ff_shell_set_beat_input`) is
 *  expected to compute them from the same raw frame `env_dbfs` was
 *  derived from (`ff_mic_level_t`'s own new fields on the esp32s3
 *  target). */
typedef struct {
    ff_beat_source_t source;
    float rms_dbfs;      /* FF_BEAT_SOURCE_MIC only — informational, not used by the detector itself */
    float env_dbfs;      /* FF_BEAT_SOURCE_MIC only — the envelope-followed BROADBAND level the loudness math runs on */
    float low_band_dbfs; /* FF_BEAT_SOURCE_MIC only — ~60-200Hz band energy this frame (ff_bandenergy.h) */
    float mid_band_dbfs; /* FF_BEAT_SOURCE_MIC only — ~200-2000Hz band energy this frame (ff_bandenergy.h) */
    float accel_mag_g;   /* FF_BEAT_SOURCE_IMU only — dynamic (gravity-removed) vertical-axis magnitude, in g */
} ff_beat_sample_t;

/* -------------------------------------------------------------------
 * Time constants — see this header's top comment, "Loudness"/"Beat
 * detection" sections, for the reasoning behind each. All flagged per
 * AGENTS.md's "note the interpretation" rule: no existing spec pinned
 * any of these before S31 (which this same change introduces), or
 * before this file's own 2026-09-09 amendment for the onset-detection
 * constants specifically.
 * ------------------------------------------------------------------- */
#define FF_BEAT_FLOOR_ATTACK_MS   1000u
#define FF_BEAT_FLOOR_RELEASE_MS  20000u
#define FF_BEAT_CEIL_ATTACK_MS    1000u
#define FF_BEAT_CEIL_RELEASE_MS   20000u
#define FF_BEAT_RANGE_FLOOR_DB    12.0f /* minimum ceil_dbfs - floor_dbfs spread */

#define FF_BEAT_REFRACTORY_MS     250u

/** MIC onset detection (2026-09-09 amendment) — see this header's top
 *  comment, "Beat/onset detection", for the full design. Each band's
 *  own rolling window of past FLUX values (half-wave-rectified
 *  frame-to-frame dB deltas) this many samples deep, at the ~50Hz
 *  nominal input rate -> ~1.5s of history. */
#define FF_BEAT_MUSIC_FLUX_WINDOW_N 75u
/** A band's flux must clear its own rolling MEDIAN flux by at least
 *  this many dB to count as an onset. */
#define FF_BEAT_MUSIC_FLUX_MARGIN_DB 6.0f
/** ...or this ABSOLUTE floor, whichever is higher — guards a flat/
 *  silent signal (median flux ~0) from tripping on ordinary numerical
 *  jitter, where "0 + margin" would otherwise be a near-zero bar. */
#define FF_BEAT_MUSIC_FLUX_MIN_DB 3.0f

/** BPM estimator (2026-09-09 amendment) — see this header's top
 *  comment, "Beat/onset detection", `bpm_estimate`'s own paragraph.
 *  Inter-onset intervals kept for the median smoothing (odd, so the
 *  median is a real element, never an average of two). Folding range:
 *  a real onset detector's raw interval inevitably sometimes reads a
 *  half-note/double-time subdivision instead of the true beat; both
 *  bounds are ordinary dance-music tempo territory, per the
 *  deliverable's own "octave folding into 70-180 BPM" instruction. */
#define FF_BEAT_BPM_HISTORY_N 5u
#define FF_BEAT_BPM_MIN 70.0f
#define FF_BEAT_BPM_MAX 180.0f

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

/** ff_beat_band_tracker_t — one band's own onset-flux state (2026-09-09
 *  amendment): the previous frame's level (to compute this frame's
 *  half-wave-rectified flux) plus a rolling window of past flux values
 *  this band's own adaptive median threshold is computed over. Two live
 *  inside `ff_beat_t` below (LOW, MID) — kept as their own named type
 *  rather than two anonymous copies of the same five fields, so the
 *  update logic (`ff_beat.c`) is one function called twice, not two
 *  hand-copied blocks that could quietly drift apart. */
typedef struct {
    float prev_level_db;
    bool  have_prev; /* false until this band's first sample — the very first frame reports 0 flux, never a
                         fabricated jump from an assumed prior level */
    float flux_history_db[FF_BEAT_MUSIC_FLUX_WINDOW_N];
    uint8_t next;    /* ring write cursor into flux_history_db */
    bool  primed;    /* false until the first sample warm-seeds the WHOLE ring at that sample's own flux (0) —
                         mirrors ranging_init's own "seed from the first sample" discipline below, so the
                         adaptive threshold does not spend its first ~1.5s biased by stale/assumed zeros */
} ff_beat_band_tracker_t;

typedef struct {
    /* auto-ranging floor/ceiling (loudness mapping) */
    float floor_dbfs;
    float ceil_dbfs;
    bool  ranging_init; /* false until the first sample seeds floor/ceil, so a fresh reset never maps against 0/0 */

    /* MIC onset detector (2026-09-09 amendment) — one tracker per band;
     * see ff_beat_band_tracker_t's own doc comment just above. */
    ff_beat_band_tracker_t mic_low_track;
    ff_beat_band_tracker_t mic_mid_track;

    /* IMU onset (peak-pick) detector — UNCHANGED by this amendment. */
    float imu_prev_mag_g;
    bool  imu_rising;
    bool  imu_have_prev; /* false until the first IMU sample, so the very first sample never reads as a "falling edge" */

    uint32_t refractory_until_ms; /* shared by both onset paths — a beat from either never fires inside the other's cooldown */

    /* BPM estimator (2026-09-09 amendment) — median-smoothed, octave-
     * folded; see this header's top comment, `bpm_estimate`'s paragraph. */
    float ioi_history_ms[FF_BEAT_BPM_HISTORY_N]; /* ring of recent inter-onset intervals */
    uint8_t ioi_next;                            /* ring write cursor */
    uint8_t ioi_count;                           /* valid entries so far, saturates at FF_BEAT_BPM_HISTORY_N */

    /* output */
    ff_beat_source_t source;
    float    loudness;    /* [0,1] */
    uint32_t beat_count;   /* monotonic; see this header's top comment on why callers diff this instead of an edge flag */
    uint32_t last_beat_ms; /* meaningful only once beat_count > 0 */
    float    bpm_estimate; /* 0 until a second beat has been seen */
} ff_beat_t;

/** ff_beat_reset — zero every field to the least-claiming state
 *  (source NONE, loudness 0, no beats yet, no band-tracker history).
 *  Call once when a Music session starts (or on every face re-entry —
 *  see ff_shell.c's own face-transition-cleanup block) so a previous
 *  session's floor/ceiling/refractory/band-flux state never leaks into
 *  a fresh one — mirrors `ff_miclevel_dc_reset`'s / `ff_batt_filter_t`'s
 *  own reset-on-restart discipline. */
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
 *  verbatim (the BROADBAND level loudness auto-ranging runs on, per
 *  this header's own "UNCHANGED by the 2026-09-09 amendment" note); for
 *  `FF_BEAT_SOURCE_IMU`, `20*log10(accel_mag_g / FF_BEAT_IMU_REF_G)`,
 *  floored at `FF_BEAT_FLOOR_DBFS`; for `FF_BEAT_SOURCE_NONE`,
 *  `FF_BEAT_FLOOR_DBFS`. */
float ff_beat_level_dbfs(ff_beat_sample_t const *sample);

#ifdef __cplusplus
}
#endif

#endif /* FF_BEAT_H */
