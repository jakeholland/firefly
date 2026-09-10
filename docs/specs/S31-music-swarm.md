# S31 — Music/Swarm: the fifth launcher app

Status: draft (2026-09-08, amended 2026-09-09 — canvas renderer, see
"2026-09-09 amendment" below "Frame budget / renderer choice"). Builds
on [S30](S30-audio-input.md) (mic
bring-up — `ff_mic`, `ff_miclevel.h`), [S26](S26-device-lifecycle.md)
(the launcher's N-agnostic satellite layout, keep-awake), [S16](S16-app-shell.md)
(the render-key churn-budget discipline), [S28](S28-gestures.md)
(BACK/HOME rim gestures), and [S15](S15-esp32s3-target.md) (`ff_compass`'s
onboard QMI8658 IMU).

## Why

The puck has a microphone (S30) and an IMU (S15) and, until now, no
face has ever used either for anything a wearer sees. "Swarm" is the
first: sixty fireflies drift on the round glass, flare and lean toward
the centre on every beat, and fade back to a calm idle glow in silence.
It is the brand, moving — not a generic visualizer, not a spectrum
analyzer, and not a claim about what anyone else's puck hears. The
concept sheet (owner-approved, via the coordinator):

> Sixty fireflies drift on the round glass. Each beat they flare and
> lean toward the centre, then wander off again. Loudness sets how
> bright the swarm glows; silence lets it fade back toward the ordinary
> idle look. Source is the mic envelope; if the mic is absent the
> wearer's bounce from the IMU becomes the beat. It is the brand,
> moving. Honesty: it reacts to what THIS puck hears or feels and never
> claims the crew hears the same thing; the source is always shown (a
> small MIC or IMU chip), and with neither source it says so and stays
> calm.

## S31 polish (owner feedback on PR #245, 2026-09-08)

Jake, trying Swarm on the field puck: "works, clap flares it and quiet
calms it. Would love it to be more dynamic like the design originally.
Not sure we need the text QUIET and LOUD or the MIC chip." Three
changes from PR #245, all in `firmware/core/ff_swarm.h`/`.c` and
`firmware/app/screens/scr_music.c`/`.h` unless noted:

1. **Motion model replaced**, not tuned. PR #245's particles wandered
   around a fixed per-particle "home" position and a beat LEANED that
   home inward. The original concept-mockup behaviour this polish
   restores is different in kind: every firefly drifts CONTINUOUSLY on
   its own fixed angular (±0.2 rad/s) and radial (±12 px/s, mockup
   units) velocity forever — reflecting off the swarm's own
   inner/outer radius bounds rather than orbiting a home point — so the
   swarm is never static even in total silence. A beat no longer leans
   a home position; it LERPS every firefly's rendered radius toward a
   shared pull target (~120px, mockup units) by a strength proportional
   to a shared beat envelope (`exp(-phase*5.5)` across the beat
   interval) and loudness, so the whole swarm visibly snaps inward
   together on a beat and eases back out onto each firefly's own
   drift trajectory as the envelope decays — literally "then wander off
   again," not a return to a fixed point. A new per-firefly TWINKLE
   term (`0.35 + 0.65*max(0, sin(...))`, phase-offset per firefly) keeps
   each firefly visibly alive between beats, when the shared envelope
   has decayed to near zero. See `ff_swarm.h`'s own top comment for the
   full writeup and every constant's provenance.
2. **QUIET/LOUD word removed entirely** from `scr_music.c`'s chrome —
   Jake's own call, quoted above. `shell_render_key` (`ff_shell.c`)
   drops `music.loudness`'s previous QUIET/LOUD-threshold bucketing and
   now zeros it unconditionally, joining `beat_count`/`bpm_estimate` —
   nothing the render key drives reads it any more (see "Render key"
   below, updated).
3. **Source chip shown ONLY when the source is NOT the mic** (IMU or
   NO SOURCE) — a deliberate NARROWING of this spec's original "the
   source is always shown" line above, not a reversal of the honesty
   rule it serves. **Recorded here as the owner's call**: an UNUSUAL
   source is always labelled (IMU renders amber, NO SOURCE renders
   muted — never an alarm color, unchanged from the original honesty
   posture); it is only the NORMAL, expected case — the mic running,
   which is what "Swarm" is built around — that now goes unlabelled.
   With the mic running, the glass shows nothing but the clock and the
   swarm: no MIC chip. This is the same "only flag what's unusual"
   posture already applied elsewhere in this codebase (a face with a
   live peer draws no special banner; a STALE or LOST one does).

A fourth change was forced by the first, not requested by the owner:
the original three-object-per-firefly rendering plan (a core dot plus
TWO halo rings, 180 objects) was measured to CRASH — this target's
LVGL heap is a fixed 64KB arena (`sdkconfig.defaults`'s
`CONFIG_LV_MEM_SIZE_KILOBYTES=64`, mirrored by the sim build's own LVGL
default), and 180 small objects plus the rest of the screen's chrome
does not fit in it. Shipped as ONE halo ring per firefly instead (120
objects total) — see `scr_music.c`'s own top comment, "Object count is
bounded by the LVGL heap," for the measured numbers and the fallback
this file would reach for (a canvas) if a future change ever needs a
third object per firefly again. **SUPERSEDED 2026-09-09**: that 120-
object renderer measured at ~7fps in the field — see this doc's own
"2026-09-09 amendment" section (below "Frame budget / renderer choice")
for the bench evidence and the canvas replacement.

**Goldens regenerated, deliberately** (the motion model, twinkle, and
chip-visibility changes all move committed pixels): `music_swarm_quiet.
png`, `music_swarm_loud.png`, `music_swarm_imu.png`,
`music_swarm_nosource.png`. Verified determinism-clean (byte-identical
across two renders each, and across the clang and gcc-14 sim builds) —
every other committed golden is untouched (confirmed via `git status`
after `tests/run_goldens.sh --update-golden`).

## Data contract

### Core: `ff_beat.h`/`.c` (`firmware/core`)

Pure, host-testable, zero I/O. Input: one `ff_beat_sample_t` per call to
`ff_beat_update(ff_beat_t *b, ff_beat_sample_t const *sample, uint32_t
dt_ms, uint32_t now_ms)` — `dt_ms` explicit (not an assumed tick rate,
same convention `ff_miclevel_envelope_update`/`ff_batt_filter_push`
already use), tagged with `source` (`FF_BEAT_SOURCE_MIC` /
`FF_BEAT_SOURCE_IMU` / `FF_BEAT_SOURCE_NONE`) so the caller — not this
module — decides which sensor is authoritative (S30's mic `present`
fact wins whenever true; IMU is the fallback).

Output, read off `ff_beat_t` after each call:
- `loudness` ∈ [0,1] — see "Loudness mapping" below.
- `beat_count` — a monotonic counter, not a per-tick edge flag. A
  caller polling slower than the input arrives (the Music face's own
  15-30fps redraw vs. the mic's ~50Hz nominal frame rate) diffs this
  value to detect "a new beat happened since I last looked" without
  risking a missed edge.
- `bpm_estimate` — `60000 / (interval since the previous beat)`, 0
  until a second beat has ever been seen (never a fabricated tempo).
- `source` — echoes the input's source verbatim; the ONE fact the
  Music face's chip renders.

### Loudness mapping — auto-ranging floor/ceiling

A quiet tent and a loud stage must both use the full [0,1] range, so
the floor/ceiling this maps against are not fixed constants:

| | Attack (chases a bigger swing) | Release (relaxes back) |
|---|---|---|
| `floor_dbfs` | 1 s, toward a QUIETER level | 20 s, drifting back up when the level stays above it |
| `ceil_dbfs` | 1 s, toward a LOUDER level | 20 s, decaying back down when nothing that loud recurs |

`loudness = clamp01((level_dbfs - floor_dbfs) / (ceil_dbfs -
floor_dbfs))`, with `ceil_dbfs - floor_dbfs` clamped to a 12 dB
minimum (widened from the floor, never lowering it — the floor is the
honestly-observed quiet baseline) so a dead-silent room with no real
dynamic range yet never divides by ~0.

`level_dbfs` is `env_dbfs` verbatim for MIC; for IMU, the caller hands
the DYNAMIC (gravity-removed) vertical-axis magnitude in g
(`ff_shell_set_beat_input`'s own `accel_z_g - 1.0f`), converted via
`20*log10(accel_mag_g / FF_BEAT_IMU_REF_G)` (`FF_BEAT_IMU_REF_G` =
0.5g) — one shared mapping/floor/ceiling code path for both sources.

### Beat/onset detection

- **MIC**: classic fast-envelope-vs-slow-average onset detector.
  `fast_env` (5 ms attack / 50 ms release) tracks transients;
  `slow_env` (100 ms attack / 400 ms release) tracks the recent
  average. A beat fires when `fast_env - slow_env >=
  FF_BEAT_ONSET_THRESHOLD_DB` (6 dB) and the refractory window has
  elapsed.
- **IMU**: vertical-axis bounce PEAK-picking directly on
  `accel_mag_g` (a local maximum above `FF_BEAT_IMU_PEAK_THRESHOLD_G`
  = 0.12g) — a footstep/dance bounce is one roughly-sinusoidal
  excursion, not a broadband transient riding on a steady background,
  so a peak detector fits it better than the dB-domain fast/slow
  detector the MIC path uses.
- **Refractory**: `FF_BEAT_REFRACTORY_MS` = 250 ms, shared by both
  paths — a beat from either never fires inside the other's cooldown.

All constants are `#define`s in `ff_beat.h`, flagged per AGENTS.md as
interpretation calls (no existing spec pinned any of these before this
change).

## 2026-09-09 amendment (fix/s31-beat-real-audio): the MIC detector never
## fires on real music — replaced with band-limited onset-flux

### On-device evidence

Jake's puck, main `a853bb5` + coordinator hotfix, real music playing
from a speaker (no subwoofer), 15s Music session:

```
music source=mic loudness=0.82 bpm=0.0 frame_ms=n/a canvas_us=n/a
```

for the ENTIRE set — `loudness` honestly tracked the mix (0.82, LOUD),
but `bpm_estimate` stayed 0.0 throughout: the onset detector above
never fired a single beat. Mic level during the set: RMS -55 dBFS, peak
-45 dBFS, envelope -54 dBFS (`mic watch` reading, mic channel
independent of the beat detector's own auto-ranging).

Also observed: the `music` console line's `frame_ms`/`canvas_us`
fragment never populated (`n/a` throughout) — a separate bug, fixed
below under "Frame stats hook", unrelated to the detector itself.

**Root cause**: the fast/slow envelope detector above was tuned
against (and only ever tested against) a click train — a genuine
broadband transient riding on near-silence. Real, mastered music is
dynamic-range-COMPRESSED: the track sits near a limiter's ceiling
throughout, so a kick drum's actual energy punch barely moves the ONE
broadband RMS/envelope number the old detector watched, even though
that same number (correctly) reads as loud on the auto-ranged
loudness scale. A click-train bench test structurally cannot catch
this failure mode.

### The fix — band-limited onset-flux, not a broadband threshold

New module `ff_bandenergy.h`/`.c` (`firmware/core`) computes a LOW
(~60-200Hz — a kick/bass note's fundamental) and a MID (~200-2000Hz — a
snare/clap/other percussive-or-melodic transient) band RMS dBFS per
20ms frame, via two cascaded one-pole (RC) lowpass filters differenced
at each band's edges (`LP(f_hi) - LP(f_lo)`) — unconditionally stable
(real poles only), not a resonant/biquad bandpass, a deliberate
"stable over surgical" tradeoff for a detector that only needs "energy
rose sharply in roughly this range", not a precise spectral picture.

`ff_beat.c`'s MIC onset path now runs on these two bands instead of the
broadband envelope:

- **Flux, not level**: each band's per-frame FLUX (a half-wave-
  rectified frame-to-frame delta in dB — "spectral flux" collapsed to
  two bands) is the onset feature, not the raw level compared to a
  slow-moving statistic. This is what correctly ignores a slow swell
  (a multi-second rise still produces a tiny per-frame delta — a few
  tenths of a dB at 20ms/frame even at a fast swell rate) while still
  catching a real attack (tens of dB within one or two frames). An
  earlier draft compared LEVEL against a trailing median directly and
  reintroduced false positives on a slow swell purely from the
  median's own lag — flux avoids that class of bug entirely.
- **Adaptive threshold**: a band's flux must clear the MEDIAN of its
  own last ~1.5s of flux history (`FF_BEAT_MUSIC_FLUX_WINDOW_N` = 75
  samples at the ~50Hz nominal rate) plus a fixed margin
  (`FF_BEAT_MUSIC_FLUX_MARGIN_DB` = 6dB), floored at an absolute
  minimum (`FF_BEAT_MUSIC_FLUX_MIN_DB` = 3dB) so a flat/silent signal
  (median flux ~0) cannot trip on numerical jitter. A MEDIAN, not a
  mean, because it is not dragged toward the very transients it exists
  to detect against.
- **Either band fires it**: a beat fires when LOW's flux OR MID's flux
  clears its own threshold (never "both required") — different
  genres/mixes push the audible onset into different bands (a
  four-on-the-floor kick into LOW; a claps/snare-forward mix more into
  MID), and requiring both would miss real beats.
- **Refractory unchanged**: still the shared `FF_BEAT_REFRACTORY_MS`
  (250ms).
- **IMU path unchanged**: still the peak-picker described above — a
  bounce is already a single-channel excursion, not a multi-band audio
  mixture needing frequency separation.
- **Loudness auto-ranging unchanged**: still driven by the broadband
  envelope exactly as before — the loudness reading was never the
  broken part; only the onset detector was.

**BPM estimator**: also amended — `bpm_estimate` is now the MEDIAN of
the last `FF_BEAT_BPM_HISTORY_N` (5) inter-onset intervals (resistant
to one missed/doubled beat), converted to BPM and OCTAVE-FOLDED (
repeated doubling/halving) into `[FF_BEAT_BPM_MIN, FF_BEAT_BPM_MAX]` =
`[70, 180]` — a real onset detector inevitably sometimes catches a
half-note/double-time subdivision instead of the true beat, and
folding into one canonical octave is the honest way to report "the
tempo" rather than a fabricated exact multiple.

### Test results (`firmware/core/tests/test_beat.c`)

Two new synthetic-signal tests (a 128 BPM four-on-the-floor kick with a
sustained bass line, compressed broadband envelope — only a 3-5dB
swing, mirroring a mastered track near a limiter's ceiling — and a
"phone speaker" variant with the LOW band's kick bump suppressed
entirely, `peak_db=0`, forcing detection through MID alone) both pass
against the real, unmocked `ff_beat_update`:

- beats land within **±40ms** of the synthesized kick times;
- `bpm_estimate` settles within **±3** of 128;
- the phone-speaker variant proves the LOW/MID OR-fusion actually
  matters (a "MID is ignored" bug would fail ONLY that test, not the
  LOW-band one).

All pre-existing acceptance cases still pass unmodified in behavior:
the 128 BPM click train (±30ms), silence (0 beats), a slow swell (0
beats), and the 2Hz IMU bounce (2 beats/s) — `mic_sample()`'s test
helper was updated to also populate the new `low_band_dbfs`/
`mid_band_dbfs` fields (mirroring `env_dbfs`, since a click/swell is a
genuine broadband event that shows up identically in every band for
these synthetic single-number signals), with no change to any test's
own assertions.

New module `ff_bandenergy.h`/`.c` has its own dedicated coverage
(`test_bandenergy.c`): a 100Hz tone reads louder in LOW than MID (and
vice versa for a 1000Hz tone), a 6kHz tone reads quiet in both, silence
floors both, and NULL/empty inputs are safe.

### Dump/replay workflow (bench capture -> detector validation)

The bench console's `mic dump <secs>` command (docs/specs/
S30-audio-input.md's own dated addition, same amendment) streams a raw
16kHz mono capture off a real puck. `tools/beat_replay.py` (this
repo's top-level `tools/`) decodes that capture into a WAV file, and
can also synthesize the same two test signals described above (128 BPM
kick + bass, plain and phone-speaker-high-passed) as standalone WAV
files. `firmware/core/tools/beat_sim_replay.c` is a small sim-only CLI
tool, linked against the REAL `ff_miclevel`/`ff_bandenergy`/`ff_beat`
implementations, that reads a 16-bit mono WAV, feeds it through the
full pipeline at the real 20ms/50Hz frame cadence, and prints loudness,
detected beats, and the running BPM estimate over time — the
coordinator's own tool for validating a real capture against this same
detector without flashing new firmware for every experiment.

### Core: `ff_swarm.h`/`.c` (`firmware/core`)

The 60-particle simulation, ALSO pure/host-testable/deterministic, but
deliberately **not** part of `ff_app_state_t` — see "Render key" below
for why, and `ff_swarm.h`'s own top comment ("Ownership") for the full
reasoning. `ff_swarm_init(ff_swarm_t *sw, uint32_t seed)` derives every
particle's fixed "personality" (initial radius/angle, angular/radial
drift velocity, twinkle phase, accent color) from a small xorshift32
PRNG seeded once — `seed == 0` remaps to `FF_SWARM_DEFAULT_SEED`
(xorshift32 fixes at exactly 0 forever if seeded with 0).

**Motion model (S31 polish, see that section above for the "why"):**
`ff_swarm_step(ff_swarm_t *sw, float loudness, bool beat_now, float
dt_s)` advances every particle on its own fixed angular
(±`FF_SWARM_ANGULAR_DRIFT_MAX_RAD_S` = 0.2 rad/s) and radial
(±`FF_SWARM_RADIAL_DRIFT_MAX_PX_S`, ~8.24 px/s on the puck's 412px
glass) drift velocity, reflecting off [`FF_SWARM_R_MIN_PX`,
`FF_SWARM_R_MAX_PX`] (~41-184px, the mockup's own 60-268px scaled by
412/600) forever — this NEVER stops, even at loudness 0 with no beat.
A beat resets a SHARED envelope to 1.0
(`envelope = exp(-phase*FF_SWARM_ENVELOPE_DECAY_RATE)`, `phase` the
fraction of the beat interval elapsed since, decay rate 5.5) and, every
`FF_SWARM_DROP_EVERY_N_BEATS`th (8th) beat, arms a stronger `drop`
accent for the interval that follows. Every firefly's RENDERED radius
is then `wander_r_px` lerped toward `FF_SWARM_PULL_TARGET_R_PX` (~82px,
the mockup's own 120px scaled) by `envelope * (FF_SWARM_PULL_LOUDNESS_
BASE + loudness)` clamped to [0,1] — a beat snaps the whole swarm
toward that shared target together, then it relaxes back onto each
firefly's own drift trajectory as the envelope decays. Glow =
`clamp01(FF_SWARM_GLOW_IDLE_BASE + FF_SWARM_GLOW_LOUDNESS_GAIN*envelope
*loudness + FF_SWARM_GLOW_DROP_GAIN*drop*envelope) * twinkle`, where
`twinkle = FF_SWARM_TWINKLE_FLOOR + FF_SWARM_TWINKLE_GAIN * max(0,
sin((t + twinkle_phase*FF_SWARM_TWINKLE_PHASE_GAIN) *
FF_SWARM_TWINKLE_RATE))` — a slow, per-firefly phase-offset brightness
oscillation that keeps a firefly visibly alive even when the shared
envelope has fully decayed between beats. See `ff_swarm.h`'s own doc
comments for every constant's exact value and provenance.

## Frame budget / renderer choice

**SUPERSEDED 2026-09-09 — see the dated amendment immediately after this
section.** The reasoning below (pre-created `lv_obj_t` circles, no
canvas) was the S31-polish call and is kept for the historical record,
but the field measurement that follows proved its central premise
wrong: 120 object mutations did NOT stay "the same order of magnitude"
as 60. Read this section as "what we believed and why," not as this
spec's current renderer.

**Pre-created `lv_obj_t` circles, not `lv_canvas`.** Reasoned from the
Map face's own measured cost (`scr_map.c`'s top comment): that face's
draw-op pool exists because CREATING ~300 `lv_obj_t` synchronously
during a full-screen REBUILD stalled the touch-poll loop by ~520 ms — a
rebuild cost, not a per-frame one. Music's problem is different in
kind: particles moving continuously at up to 30fps, but the shell's
`lv_obj_clean`+rebuild happens RARELY (the particle state is
deliberately kept out of the render key — see below), so the "hundreds
of objects at once" cost Map hit essentially never recurs. What
happens every frame instead is property MUTATIONS
(`lv_obj_set_size`/`_align`/`_set_style_bg_opa`) on already-existing
objects — no create/delete, no style-tree rebuild.

**Each firefly is TWO objects (S31 polish, revised from PR #245's
original THREE-object plan), 120 total, not 180** — a small ink-white
core dot plus ONE halo ring (not two), the "2-3 concentric circles"
range this section's own original amendment allowed. The THIRD object
(a second halo ring) was tried and dropped for a reason that has
nothing to do with per-frame CPU cost: this target's LVGL heap is a
fixed 64KB arena (`sdkconfig.defaults`'s own
`CONFIG_LV_MEM_SIZE_KILOBYTES=64`, mirrored by the sim build's LVGL
default), and 180 small `lv_obj_t` — each costing roughly 230-280 bytes
once `spec_attr` (children/align bookkeeping, allocated lazily on an
object's first `lv_obj_align` call) is counted, measured directly
against this exact LVGL build — consumes essentially the whole arena
before the screen's own clock/source-chip labels get a turn;
`test_gesture_glue.c`'s `S31_back_on_music_goes_home` reproduced this
as a hard crash (`lv_realloc` returning NULL, then an unchecked NULL
write) the one time it actually built a full 180-object Music screen
end to end. 120 objects leaves comfortable headroom — see
`scr_music.c`'s own top comment, "Object count is bounded by the LVGL
heap," for the full writeup including the exact probe methodology, and
its "Per-frame cost estimate" section for why 120 objects' redraw cost
is still the same order of magnitude as the original 60-object budget
that was already comfortably inside 8ms at Radar's own measured
per-frame object count. A canvas remains the documented fallback if a
future change ever needs a third object per firefly again (it needs
one object plus one pixel buffer, not N objects — sidestepping the
LVGL-heap ceiling entirely) — not needed at 120.

**Frame cap**: 30 fps normally; 15 fps once `state->radar.batt_pct` is
a KNOWN reading (never treats "unknown" as low, same convention
`ff_radar_batt_is_low` uses everywhere else) at or below 20%. Enacted
via `lv_timer_set_period` on the face's own timer, re-checked every
firing (so a battery crossing mid-session takes effect on the next
tick, not only at the next rebuild).

## 2026-09-09 amendment: the 120-object renderer regressed to ~7fps —
## replaced with a canvas

**The bench evidence.** Jake, on the field puck (main 3ecaae7, Music
face open, mic running): "the firefly music animation got less dynamic
after the last update." `perf` console output over a 5s window:
`lvgl_refresh min_us=128 avg_us=144705 max_us=265787 n=22` — **145ms per
LVGL refresh, ~7fps, worst frames at 266ms**. The shell's own render
loop was fine in the same window (`frame avg_us` ~2200, i.e. 2.2ms) —
the cost was entirely inside LVGL's own refresh of the 120-object dot
pool this section's now-superseded reasoning shipped. Before that PR
(#245, 60 objects), the animation read as responsive; after it (#247,
120 objects — this section, above), the SAME motion model reads as
sluggish. The regression is renderer cost, not the motion model: at
7fps every drift/twinkle/beat-pull parameter looks slow regardless of
how lively the underlying numbers actually are.

**Why the "same order of magnitude as 60 objects" reasoning above was
wrong.** It modeled the per-frame cost as "N objects x (2 cheap struct
writes)" — flat, linear in N. LVGL's real per-frame cost for a redraw
this shape (many small, overlapping, individually-invalidated circles)
is dominated by things that do NOT stay flat as N grows: per-object
style/style-cache lookups, invalidation-AREA unions across overlapping
objects, and one draw-call dispatch per object. Going from 60 to 120
objects (2x) produced roughly a 15-20x frame-time regression (a rough
back-of-envelope from Radar's "comfortably inside 8ms" claim at 60
objects vs. the measured 145ms at 120), not the ~2x this section
assumed. The lesson (AGENTS.md item 6: measure, don't assume): "the
same order of magnitude" was a plausibility argument, never actually
measured against a real 120-object build's `perf lvgl_refresh` line —
this PR's whole reason to exist is that gap.

**The fix: one `lv_canvas`, not more/fewer `lv_obj_t`.** This section's
own "documented fallback" (a canvas needs one object plus one pixel
buffer, not N objects, sidestepping the LVGL-heap ceiling) turned out to
be the fix for the FRAME-TIME ceiling too, not only the heap one. The
swarm (60 fireflies) is now drawn into ONE `lv_canvas` sized to the
glass (412x412), backed by a single RGB565 pixel buffer allocated ONCE
for the process lifetime — `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`
on the esp32s3 target (PSRAM, confirmed via a dedicated device-build
check — never the fixed 64KB LVGL heap arena this section's own
120-object budget was sized against), plain `malloc` on the sim. Every
frame: clear the buffer to the theme background + bake in the faint
static ring (no longer its own `lv_obj` — an opaque canvas would just
paint over anything left under it, see `scr_music.c`'s own "Chrome"
comment), then additively composite each firefly from one of 4
pre-rendered "glow sprite" shapes (bucketed by glow quartile for SIZE,
continuously scaled for BRIGHTNESS — no visible banding) via direct,
unlocked integer pixel writes, then invalidate the canvas ONCE. See
`scr_music.c`'s own top comment, "Renderer," for the full design
(sprite generation, the additive-blend math, why RGB565 not this
build's native display depth). The clock label and source chip remain
ordinary `lv_obj_t` labels drawn on top — nothing about the "only flag
what's unusual" chrome rules above changed.

**Measured cost.** A scratch sim probe (a real `ff_ctl_loop_pump`+
`lv_timer_handler()` session driven into Music with a loud 128 BPM mic
stream, 3 simulated seconds at 30fps, reading `ff_scr_music_debug_
frame_stats()`'s real `clock_gettime`-measured canvas-draw time) measured
**~0.9-1.2ms of actual composite+clear CPU time per frame on an Apple
M2 laptop**. That number does not translate directly to the esp32s3
target (an M2 clocks ~15x higher AND is a wide superscalar core with
large caches the Xtensa LX7 has neither of), so the device estimate is
reasoned from pixel/byte counts instead, per this PR's own brief: one
412x412 RGB565 clear (169,744px, ~339KB, sequential write — PSRAM-
bandwidth-friendly) + the static ring (~1,080px, negligible) + up to 60
sprite composites (45x45 bounding box each, most pixels skipped on
zero alpha; a worst-case ALL-60-at-peak-glow frame touches up to
~72,780px of read-modify-write, ~291KB) — **up to ~0.5-0.65MB touched
per frame** in the worst case (a transient beat-drop moment), typically
much less (the swarm's own twinkle floor/pull decay means most frames
have most fireflies well under peak glow — see `ff_swarm.h`'s glow
formula). At a conservative 240MHz Xtensa LX7 + external octal PSRAM
(effective sustained bandwidth commonly cited in the 40-70MB/s range
for this kind of mixed sequential-clear/scattered-small-block-composite
traffic), that puts a TYPICAL frame comfortably inside this PR's own
<=12ms target and a WORST-CASE (rare, transient, all-60-near-peak)
frame in the ~9-16ms range — a real, bounded, honestly-reported risk,
not a guarantee, but an enormous, unambiguous improvement over the
145ms/frame this amendment replaces regardless of where in that range
the real hardware lands. Confirming the worst-case number needs a
flashed device with `perf`'s own `lvgl_refresh` line (kept working by
this PR — see "Console" below) — out of scope here (build-only, no
flash).

**Object count, revisited.** The whole "120, not 180, because the LVGL
heap is a fixed 64KB arena" story above is moot: this renderer uses
exactly one `lv_obj_t` for the canvas (plus the two chrome labels) —
three objects total, independent of firefly count. `scr_music.c`'s own
"Object count is bounded by the LVGL heap" section is superseded the
same way "Frame budget / renderer choice" above is.

## 2026-09-09 amendment: internal-RAM boot-parking regression —
## sprite table moved to PSRAM

**The bench evidence.** A puck flashed with the canvas renderer above
(main a853bb5, PR #252) never got past the boot splash. Serial log:
`E LVGL: lvgl_port_add_disp_priv(389): Not enough memory for LVGL
buffer (buf2) allocation!` -> `ff_display: lvgl_port_add_disp failed`
-> `firefly: parked: LVGL display bring-up failed`. Console dead, no
further diagnostics. The immediately preceding main (3ecaae7) booted
fine — this canvas renderer's own PR is what changed.

**Root cause.** The "Renderer" section above already documents the
canvas PIXEL BUFFER as PSRAM-only (`s_canvas_buf`, `heap_caps_malloc(...,
MALLOC_CAP_SPIRAM)`) — that part was never the problem. What that
section's own prose undersold is the SPRITE table: `static music_
sprite_t s_sprites[FF_SCR_MUSIC_GLOW_STEPS][2]` — 8 pre-rendered
45x45 RGB565+alpha sprites, `sizeof(music_sprite_t) == 6075` bytes each
— 48,608 bytes of plain `static`, i.e. ordinary INTERNAL `.dram0.bss`
(confirmed byte-for-byte against a real device build's own link map,
`firefly_esp32s3.map`: `.bss.s_sprites` at exactly `0xbde0` = 48,608
bytes). On the esp32s3 target, internal RAM is not just "the fast RAM"
— it is ALSO the ONLY RAM `esp_lvgl_port`'s own display buffers can
come from (`MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA` — the panel's QSPI
DMA engine cannot reach PSRAM). `ff_display.c`'s own `disp_cfg` asks
for TWO (`double_buffer = true`) buffers of `FF_LCD_H_RES(412) *
FF_LVGL_STRIP_LINES(40) * 2 bytes/px` = 32,960 bytes each — ~65.9KB
total — out of whatever `dram0_0_seg` (341,760 bytes total, this
target's whole internal-DRAM linker region) has left after every
`static` in the image plus every other `MALLOC_CAP_INTERNAL` consumer
(task stacks, driver buffers, IDF's own housekeeping) has taken its
share. A real before/after device build (same sdkconfig, `CONFIG_FF_
BRINGUP_STAGE_3`, both matched against this same map-file method) pins
the exact numbers:

| | `.dram0.data` | `.dram0.bss` | total static |
|---|---|---|---|
| before (a853bb5) | 15,680 | 167,968 | **183,648** |
| after (this PR)  | 15,680 | 119,360 | **135,040** |

— a difference of exactly 48,608 bytes: `s_sprites` and nothing else.
That 48.6KB was enough to starve `buf2`'s allocation of a large-enough
contiguous block on the maintainer's own bench puck.

**The fix.** `s_sprites` is now a pointer (`static music_sprite_t
(*s_sprites)[2]`), lazily allocated by `music_ensure_sprites` from
PSRAM (`heap_caps_calloc(..., MALLOC_CAP_SPIRAM)` on-device, plain
`calloc` on the sim) the first time `music_build_sprites` runs — the
identical NULL-safe, lazy, process-lifetime-cached shape `music_ensure_
canvas_buf` already established for `s_canvas_buf` one section up. A
failed allocation leaves `s_sprites == NULL`; `music_build_sprites`
then simply never sets `s_sprites_ready`, and `music_composite_particle`
(this file's own per-firefly blit) no-ops on a NULL `s_sprites` rather
than dereference it — the swarm draws zero fireflies onto an otherwise
normal cleared-plus-ring canvas rather than crash. Unlike the canvas
PIXEL buffer (one big sequential clear + scattered small blits, already
reasoned as PSRAM-bandwidth-friendly in "Measured cost" above), the
sprite table is read-only after the one-time build — PSRAM's higher
per-access latency costs nothing here since nothing re-renders the
sprites themselves per frame, only reads already-computed alpha/color
bytes out of them.

**Audited, not just this one table.** Every other `static` over 8KB in
`firmware/app` + `firmware/targets/esp32s3` was checked against a real
device build's own map file (`firefly_esp32s3.map`, byte-accurate, not
estimated): only one other candidate exists, `scr_map.c`'s `s_draw_ops`
(`map_draw_op_t s_draw_ops[FF_SCR_MAP_MAX_DRAW_OPS]`, 650 * 36 bytes =
23,400 bytes, confirmed in the map as `.bss.s_draw_ops` = `0x5b68`).
**Decision: left internal, not moved, in this PR.** Reasoning: (1) it
is not part of this regression — the 48,608 bytes this PR frees already
restores ~28.8KB of headroom past the documented CI budget (`tools/
check_dram_budget.py`) below, comfortably more than `s_draw_ops`'s own
23.4KB; (2) unlike the sprite table (read-only after one build),
`s_draw_ops` is read by LVGL's OWN draw call chain for the Map screen
every frame it is dirty, inside LVGL's draw-dispatch critical path —
a genuinely different access pattern than "read a handful of large
sequential blocks once per firefly per frame," and one this PR has not
measured; (3) this S3 board's octal PSRAM at 80MHz has ample raw
bandwidth for either pattern, so moving it later under the exact same
lazy/NULL-safe shape `music_ensure_sprites` establishes is a real,
available option if internal RAM ever gets tight again — just not a
change this urgent, narrowly-scoped regression fix should also be
making without measuring it first. Every OTHER static candidate found
(mic/audio sample buffers, the boot-edge ring, the debug-console line
buffer, radar's line/triangle pools, flare's ray-mark points) is under
2KB — no concern.

**The CI gate this regression should always have tripped.**
`tools/check_dram_budget.py` (firmware/tools/) sums `.dram0.data` +
`.dram0.bss` from the esp32s3 build's own map file and fails the build
if the total exceeds 163,840 bytes (160 KiB) — a budget derived from
the exact before/after numbers above: comfortably above this PR's own
135,040-byte total (~17.6% headroom) while sitting well below the
183,648-byte total that actually broke boot, and leaving 177,920 bytes
of `dram0_0_seg` for the heap at its own ceiling — more than double the
~65.9KB the LVGL port's own double-buffered display buffers need. See
that script's own top comment for the full derivation. Wired into
`.github/workflows/esp32.yml`'s `esp32-build` job, both matrix legs (the
"defaults" leg links no LVGL/display code at all, so it is always far
under budget — the check is a no-op there, not a special case).

**The diagnostic gate this regression should never have been silent
under.** A failed `ff_display_lvgl_start()` used to `ff_park` — an
infinite `vTaskDelay` loop with nothing but a heartbeat log, on a device
whose console had not even been installed yet. `app_main.c`'s `ff_park_
lvgl_failure` (this PR) instead logs the exact internal/DMA-RAM numbers
at the moment of failure (`heap_caps_get_free_size`/`heap_caps_get_
largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)` — the same
capability mask `esp_lvgl_port` itself allocates from) and, with
`CONFIG_FF_DEBUG_CONSOLE` on, keeps servicing the USB-Serial-JTAG bench
console (moved `dbgconsole_init()` earlier in bring-up so it is already
installed by the time this path can be reached) so the device can still
be diagnosed and reflashed over the same wire — never silently bricked
on this class of failure again.

## Render key: the particle state must NOT drive it

Per S16's rule, the render key drives a FULL `lv_obj_clean`+rebuild on
every dirty tick — 60 particles' continuous motion in there would
either force a rebuild every frame (an immediate churn-budget
violation) or need per-particle masking at 60× the surface area of
every other "must not churn" field this codebase carries. The fix has
two parts:

1. **The swarm instance itself is not in `ff_app_state_t` at all.**
   `scr_music.c` owns one `ff_swarm_t` as a file-static (the same
   "screen owns its own pool" convention `scr_map.c`'s draw-op pool
   already establishes) and steps/redraws it from its own
   `lv_timer_t`, entirely outside the shell's tick/render-key path.
2. **`ff_app_state_t.music` (`ff_app_music_t`) carries only the few
   facts the CHROME needs** — `source`, `loudness`, `beat_count`,
   `bpm_estimate`, `seed` — and `shell_render_key` masks every one of
   them to what the chrome actually renders: `beat_count`/
   `bpm_estimate` are zeroed unconditionally (nothing on glass draws
   either — the particle sim reads `beat_count` itself, by diffing it
   every frame OUTSIDE the render key, in `scr_music.c`'s own timer).
   **`loudness` (S31 polish)** now joins them, zeroed unconditionally —
   the QUIET/LOUD word it used to bucket for is gone from the chrome
   entirely (see "S31 polish" above), so nothing the render key drives
   reads `loudness` any more; the particle sim still reads the RAW
   value every frame, but straight off the live view in `scr_music.c`'s
   own timer, outside this key. `source` still earns its keep — the
   source chip's own presence and color depend on it — and stays
   gated on `active_face == FF_APP_FACE_MUSIC` (unlike the DIAGNOSTICS
   fields, `shell_project` writes `music.*` UNCONDITIONALLY every
   tick — see that projection's own comment for why — so an explicit
   gate is needed here the same way `heading_deg`'s own
   DIAGNOSTICS-bucket fix needed one).

Regression coverage: `test_shell.c`'s `S16_render_key_churn_budget_music`
(the aggregate budget, ≤ 60/min over the shared S16 churn scenario) and
`S31_music_loudness_never_dirties_render_key` (S31 polish, renamed from
`..._keys_rendered_word_bucket_only` — a dedicated pin: a settled quiet
baseline produces no further churn, and NEITHER does pushing all the
way to a sustained loud level, across 75 ticks of a continuously-
changing raw loudness float — proof `loudness` is unconditionally
zeroed, not merely bucketed).

## Power policy

> **2026-09-09 amendment — keep-awake removed.** See "Keep-awake
> (REMOVED, 2026-09-09)" below for the full writeup: the bullet just
> above the fold used to read "the face keeps the puck awake only while
> it is hearing/feeling something above the auto-ranged floor" — bench
> evidence showed that floor never actually releases in an ordinary
> room, so Music ran the mic and held the screen at full brightness for
> 6.6 hours straight, unattended, overnight. Music now gets **zero**
> special keep-awake treatment: the S26 DIM-at-15s/OFF-at-30s timers
> apply to it exactly as they do to every other face. `music_wants_mic`
> below is unchanged in SHAPE (mic runs only while Music is active,
> genuinely visible, and the screen is genuinely ACTIVE) but its THIRD
> term now comes from the idle FSM's own output, not from trying to
> feed the idle FSM an input (loudness) that structurally never goes
> quiet enough on its own.

`ff_mic_start()`/`ff_mic_stop()` (app_main.c, the esp32s3 target — the
shell never calls these directly; it has no `ff_mic.h` dependency, per
CLAUDE.md's placement rule) are driven by one boolean, re-derived every
main-loop iteration from the ONE shared, host-tested predicate
`ff_shell_music_wants_mic` (`ff_shell.h` — pulled out of app_main.c's own
inline expression by the 2026-09-09 amendment so app_main.c's device
loop and the sim's own ctl-harness regression test,
`targets/sim/tests/test_ctl_music_idle_drain.c`, can never drift apart
on what "the mic should be on" means):

```c
music_wants_mic = (active_face == FF_APP_FACE_MUSIC)
                && !flare.takeover_active
                && (idle_state == FF_IDLE_STATE_ACTIVE);
```

`idle_state` is the S26 idle FSM's own `ff_idle_tick` return for this
frame — DIM/OFF/SLEEP all withhold the mic, same as they always did, but
now because the idle FSM genuinely reached DIM/OFF/SLEEP on its own
un-overridden schedule, not because a second, separate loudness
threshold happened to agree with it.

Start/stop fire exactly on the edge (both are individually idempotent,
but `ff_mic_start()` also resets its DC-blocking/envelope filter state
on every call — calling it every frame would defeat that filter).
While `music_wants_mic` is true, `ff_shell_set_beat_input` is called
every `FF_MUSIC_INPUT_PERIOD_MS` (20 ms, matching the mic's own
50 Hz frame cadence) with `ff_mic_status()`/`ff_mic_level()` and,
whenever `mic_present` is false, `ff_compass_last_accel_board()` — the
**new small accessor this spec's own coordinator brief asked for**
(`ff_compass_last_accel_board`, mirroring `ff_compass_last_mag_board`'s
exact contract). Source selection (MIC wins whenever present, else
IMU, else NONE) is `ff_shell_set_beat_input`'s own job — see that
function's doc comment (`ff_shell.h`).

**Known limitation, not fixed by this PR**: `ff_compass_read()` (S15)
only samples the IMU as part of a combined mag+accel read, and returns
its honest -1 "unknown" sentinel — touching neither accessor — the
instant NO magnetometer is present at all. On a puck with the onboard
QMI8658 IMU healthy but no GY-273 magnetometer wired (the magnetometer
is an aftermarket add-on), `ff_compass_last_accel_board()` never
updates, so Music's IMU fallback is unavailable there today — it
honestly reports NO SOURCE instead of a fabricated bounce. Splitting
`ff_compass_read()` into independent mag-only/IMU-only sampling would
be the real fix; out of scope here (S31 is additive, not a rework of
S15's read path) — see "Questions" below.

**Keep-awake (REMOVED, 2026-09-09 amendment).** `ff_shell_keep_awake`
used to get one Music-specific branch — `active_face ==
FF_APP_FACE_MUSIC && view->music.loudness > FF_BEAT_KEEPAWAKE_LOUDNESS`
(0.05) — "the face keeps the puck awake only while it is genuinely
hearing/feeling something above the auto-ranged floor". It is gone.

**Bench evidence** (Jake's puck, main `51c5d16`, overnight on USB): the
puck was left on the Music face. Console log: `ff_mic: mic started` at
uptime 44.75s, then **no backlight change for 6.6 hours** (no DIM at
15s, no OFF at 30s), then `ff_mic: mic stopped` at uptime 23,878s. The
room was ordinary night ambient — nobody clapping, nothing loud.

**Root cause**: `loudness` (`ff_beat_t`, `ff_beat.h`) is computed
against an AUTO-RANGING floor/ceiling — "Loudness: auto-ranging
floor/ceiling" above — that CHASES whatever level the room actually is,
with only a ~20s release time constant back up. In an ordinary quiet
room, ambient noise sits jittering just above that self-tracking floor
INDEFINITELY; `loudness` never actually settles at or below
`FF_BEAT_KEEPAWAKE_LOUDNESS`, so the old branch above never released.
The floor built to answer "is the room quiet" was, by its own auto-
ranging design, structurally incapable of ever calling an ordinary room
quiet. The mic ran and the screen sat at 90% for as long as the puck sat
on Music, unattended — a real field battery-drain bug, not a bench
curiosity.

**The fix — product rule, not a threshold retune (owner's call, Jake,
2026-09-09): Music must never override the idle policy.** It now gets
**zero** special keep-awake treatment, at any loudness. The S26
DIM-at-15s/OFF-at-30s-since-last-INPUT timers (`docs/specs/
S26-device-lifecycle.md`) apply to Music exactly as they do to every
other quiet face — the same "the launcher deliberately does NOT keep
awake" precedent this function already established for a different
face. **Sound is never an input.** `ff_shell_keep_awake`'s doc comment
(`ff_shell.h`) carries the full writeup at the removed branch's old
call site.

The mic's own power policy did not need a threshold either — it was
already correctly gated on the idle FSM's OUTPUT (`idle_state ==
FF_IDLE_STATE_ACTIVE`, the "Power policy" section above), which only
ever failed to matter because keep-awake never let idle LEAVE ACTIVE in
the first place. Fixing keep-awake alone was enough to fix the mic too;
`ff_shell_music_wants_mic` (`ff_shell.h`) is a shared-function pull-out
of that same pre-existing gate, not a new decision. `ff_mic_stop()`
still fires on DIM (not just OFF/SLEEP/leaving-the-face/flare
takeover) via that exact same `idle_state == ACTIVE` gate — DIM is
`idle_state != ACTIVE`, so it was always covered, it just never used to
be reachable while Music was open.

The swarm's OWN per-frame timer (`scr_music.c`) is a second, independent
half of this fix: it used to keep stepping/redrawing at 15-30fps
regardless of screen state (the mic not being fed new samples does not
stop the timer from re-animating stale ones). It now reads
`state->music.screen_awake` — the S26 idle FSM's own ACTIVE fact,
pushed by `ff_shell_set_screen_awake` (`ff_shell.h`) — and skips
stepping/redrawing entirely while not ACTIVE, resuming cleanly (no
elapsed-time jump) the instant it is again. See `ff_scr_music_debug_
render_ticks` (`scr_music.h`, test-only) and `targets/sim/tests/
test_ctl_music_idle_drain.c` for the regression coverage that measures
this rather than assuming it (AGENTS.md item 6).

`sleep_inhibit` already includes `ff_mic_status().running` (S30) — a
mic sample in flight is never cut off mid-frame by
`esp_light_sleep_start()`; this PR adds no second inhibit source.

**Power diagnostic (2026-09-09, this same PR)**: the mic's cumulative
on-time since boot (never reset by a start/stop cycle) is now visible
on the bench console's `mic` line (`total_on_s=`) and the DIAGNOSTICS
page's own **MIC ON-TIME** row — see `ff_mic_status_t.total_on_ms`
(`ff_mic.h`) and `ff_shell_set_mic_total_on_ms` (`ff_shell.h`). This
overnight bug had no such number to look at; it does now, so a stuck-on
mic can never hide in a point-in-time status read again.

## Launcher: the fifth satellite

`FF_APP_FACE_MUSIC` is appended to `ff_app_face_t` (after `LAUNCHER`,
this enum's own append-only convention) and to `ff_route.c`'s
`k_swipe_axis` (renamed in comment to "every launcher-reached base
face", since swipe itself is a retired, callerless primitive — S26e —
but its membership test is what both `ff_route_launcher_select` and
`ff_route_push_modal`'s base-validity check share; Music must be on it
for the launcher tap AND a PWR long-press from Music to both work).
`ff_shell.c`'s `k_launcher_faces[]` gains a sixth entry (idx 5).

`scr_launcher.c`: `LAUNCHER_SAT_COUNT` 4 → 5. Per that file's own
long-standing "Music-readiness contract" comment, the new satellite is
`compass_pos = 4` (the next open slot in the N-agnostic
`ff_scr_launcher_satellite_deg` formula — 288° at N=5) — the other
four satellites KEEP their existing `compass_pos` values (0/1/2/3)
rather than being reshuffled into a different pentagon order this repo
has no design-canvas reference for (interpretation call, flagged per
AGENTS.md). New icon (`launcher_icon_music`): three fireflies of
varying size, an abstract loose cluster — deliberately not a musical
note/headphone/speaker glyph, since "it is the brand, moving" means the
launcher icon should read as a small swarm, not a generic "music app"
pictogram.

**Goldens updated, deliberately** (the N=4→5 angle-formula change moves
every existing satellite's pixel position): `launcher.png`,
`launcher_unread.png`, `launcher_low_battery.png`,
`banner_on_launcher.png`, and `settings_clock_24h.png` (a launcher-face
fixture despite its name — it exercises 24h clock formatting on the
launcher's own status row). All five are byte-for-byte deterministic
re-renders (verified against both the clang and gcc-14 sim builds);
every one of the other 93 pre-existing goldens is untouched (confirmed
via `git status` after `--update-golden`).

## Music face (`scr_music.c`)

Own top-level branch in `ff_face_dispatch.c` (not routed through
`ff_scr_nav_build`'s shared five-swipe-face shell — that shell's chrome
is shaped around the RADAR/NOW/SIGNALS/MAP/SETTINGS quintet and has
nothing in common with Music's per-frame timer/draw-op pool; same
"own dedicated build function" shape COMPOSE/POWER_MENU/LAUNCHER
already use). BACK/HOME still work here unconditionally — S28's gesture
recognition reads raw indev points independent of which screen is
built (`ff_gesture_glue.c`), never touching `scr_music.c` at all
(pinned by `test_gesture_glue.c`'s `S31_back_on_music_goes_home`).

Chrome, centred: the wall clock (`state->radar.clock_str`, the same
honest "--:--" convention every other face's clock uses) and — ONLY
while `state->music.source != FF_APP_MUSIC_SRC_MIC` (S31 polish, see
that section above for the owner's call and the honesty reasoning) — a
small muted source chip underneath (IMU in `FF_THEME_COLOR_AMBER`, NO
SOURCE in `FF_THEME_COLOR_MUTED` — "stays calm", never an alarm color).
No QUIET/LOUD word (removed, S31 polish); with the mic running (the
normal case), the glass shows nothing but the clock and the swarm.

### Golden determinism without ever running the per-frame timer

The sim's one-shot headless renderer (`ff_run_headless_once`) builds a
fixture and calls `lv_refr_now()` exactly once — it never drains
LVGL's timer queue, so `scr_music.c`'s `lv_timer_t` never fires for a
golden. `ff_scr_music_build` therefore performs its own ONE
deterministic "settle" step at build time — `ff_swarm_init(seed)` then
exactly one `ff_swarm_step` at a fixed frame duration (1/30 s), fed
`state->music.loudness` and "was there already a beat"
(`state->music.beat_count != 0`) — so the very first painted frame
already reflects the fixture's specified loudness/beat state, fully
reproducibly from `seed` alone. Live operation (device + interactive
sim) continues stepping from exactly that same settled state once the
timer starts firing for real.

## Console

Two commands, following S30's `mic`/`mic watch` shape. The `source`/
`loudness`/`bpm` fields need no platform hook: `ff_beat_t` is core
state the shell already owns on every target (unlike `ff_mic`,
esp32s3-only), so `music`/`music seed <n>` are real (non-"unavailable")
on both the device and the sim — `ff_debug_console.c` reaches them
through two public getters/setters (`ff_shell_music_debug`,
`ff_shell_set_music_seed`), the same "public getter, never reach into
`shell_t`" rule this file's own top comment states for every read-only
command.

**2026-09-09 amendment (canvas renderer)**: the `music` line gained a
`frame_ms=.../canvas_us=...` fragment — the Music screen's own
per-frame timer's last-CLOSED-one-second rolling average frame period
(ms) and canvas composite draw time (us), the numbers this PR's own
measured-cost section above is built from. UNLIKE `source`/`loudness`/
`bpm`, this fragment DOES need a platform hook (`ff_dbgconsole_music_
frame_fn`, ff_debug_console.h): it is sourced from `scr_music.c`'s own
timer, and `ff-debug-console` (this file's link target) deliberately
excludes LVGL/`ff-app-ui`, so only the wiring layer that already links
both (`app_main.c` on the esp32s3 target) can reach `scr_music.h`'s
`ff_scr_music_debug_frame_stats` getter directly. A NULL hook or a
window that hasn't closed yet both report the honest `frame_ms=n/a
canvas_us=n/a` — never a fabricated 0.00/0. The sim never wires the
debug console into its own interactive runtime at all today (only
`app_main.c` and `test_debug_console.c` call `ff_dbgconsole_handle_
line`), so this fragment is `n/a` in every sim/test context; it is real
on the esp32s3 target.

The `perf` command's `lvgl_refresh` line (2026-09-08 QA hardening,
app_main.c) is UNCHANGED by this PR and remains the tool that found the
145ms/frame regression in the first place — it is what a bench engineer
should watch after this fix to confirm the canvas renderer actually
brought that number back down on real hardware.

**2026-09-09 amendment (fix/s31-beat-real-audio)**: on-device evidence
(this file's own "On-device evidence" section above) showed
`frame_ms=n/a canvas_us=n/a` for an ENTIRE 15s real-music session —
this fragment never populated at all on real hardware, despite
`loudness`/`bpm` reading correctly off the same live session. Two
changes:

- `scr_music.c`'s frame-stats accumulator (`s_frame_stats`) now stamps
  `last_valid_ms` (`lv_tick_get()`) every time a window closes, and
  `ff_scr_music_debug_frame_stats()` reports the honest `n/a` (not the
  real numbers) once more than `FF_SCR_MUSIC_FRAME_STATS_KEEP_MS`
  (30s) has passed since the last close — before this fix, a value
  that HAD populated was kept until the NEXT face build with no upper
  bound at all (not itself a correctness bug, but nothing previously
  guaranteed the console showed a RECENT number rather than an
  arbitrarily old one from a session long over). This is the concrete
  form of the deliverable's own "keep the last values for 30s after
  leaving the face so a console read after the session still shows
  them" — before this fix there was no expiry logic at all to test.
- A new sim regression (`firmware/targets/sim/tests/
  test_ctl_music_frame_stats.c`) builds Music for real (a real
  `ff_ctl_loop_pump` session) and drives its per-frame timer with real
  `lv_timer_handler()` calls under a mock clock, then reads
  `ff_scr_music_debug_frame_stats()` directly — proving it reports
  real, non-n/a numbers after ~2s of a real build+timer run (and
  honestly n/a immediately before the first window closes), and that
  those numbers survive leaving the face for a few seconds before
  correctly expiring back to n/a past the 30s keep window. This sim
  test PASSES against the current mechanism — the accumulation/getter
  logic itself is verified correct end-to-end in the sim; the
  on-device "never populates for 15 real seconds" symptom could not be
  reproduced here and remains open as a hardware-only observation (a
  real puck's LVGL tick/render-loop timing, not this code path, is the
  likely remaining suspect — flagged per AGENTS.md rather than claimed
  fixed without evidence).

**2026-09-09+ amendment (fix/mic-dump-device-path) — root cause found:
`s_frame_stats` was a cross-task producer/consumer with no lock at
all.** The "hardware-only observation" flagged above is this: on the
esp32s3 target, `music_timer_cb` (`scr_music.c`) — the code that WRITES
`s_frame_stats` — only ever runs on `esp_lvgl_port`'s own task
("taskLVGL"), inside an `lv_timer_handler()` pass. `ff_scr_music_debug_
frame_stats()` — the getter the `music` console command READS through
`dbgconsole_music_frame` — runs on a COMPLETELY DIFFERENT task
(`app_main.c`'s render loop, the same task `dbgconsole_poll` is called
from). Before this fix, that read had NO synchronization at all: the
sim regression above cannot catch this class of bug even in principle
— it is single-threaded (this file's own "Golden determinism" section:
no LVGL port task exists there; the SAME thread that calls
`lv_timer_handler()` is the one that then reads the getter), so there
is no cross-task race for it to exercise regardless of how faithfully
it drives the timer. This is exactly the shape `ff_display.c`'s own
"2026-09-08 QA hardening item 2" comment already documents and guards
for its sibling `lvgl_refresh`/`flush` perf windows (`s_refresh_perf`/
`s_flush_perf`, a `portMUX_TYPE` spinlock) — `s_frame_stats` was simply
never given the same treatment when it was added. Fixed by wrapping
every touch point (`music_frame_stats_reset`, `music_frame_stats_add`,
and the getter's read) in the identical `portMUX_TYPE`/
`portENTER_CRITICAL`/`portEXIT_CRITICAL` discipline (compiled to a
no-op on `FF_TARGET_SIM`, where there is nothing to protect against).
Also hardened, defensively: `ff_scr_music_build`'s `lv_timer_create`
call had no NULL check (this feature has a real history of tight
internal-RAM headroom on device — PR #253, "device out of internal
RAM" from this same S31 canvas renderer's own sprite table) — a failed
allocation there would silently leave the swarm frozen after its one
build-time settle frame and the stats window permanently unclosed,
which would present identically to this bug from the console's point
of view. Both are now documented inline (`scr_music.c`'s own
`s_frame_stats_lock` and `lv_timer_create` comments) rather than left
as an unexplained gap.

**2026-09-09+ amendment (fix/mic-dump-device-path) — "one line tells
the whole story"**: the `music` line now also folds in the `perf`
command's own `lvgl_refresh` window (`ff_display_perf_get`, the SAME
data `dbgconsole_perf`'s `lvgl_refresh` line already reports) whenever
the frame stats are fresh — `lvgl_refresh_avg_us=<N> lvgl_refresh_max_
us=<N>`, or `lvgl_refresh=n/a` if that window hasn't closed yet (the
device's first `FF_PERF_WINDOW_MS` (5s) after boot, regardless of
Music). A bench operator diagnosing a slow music frame no longer has to
run `perf` separately to tell apart "the swarm's OWN canvas composite
is slow" (`canvas_us`) from "LVGL's broader refresh/flush pass is slow
underneath it" (`lvgl_refresh_*`, shared by every face) — the exact
distinction this hook's own history above ("the 145ms/frame regression
found via perf's own lvgl_refresh line") once needed two commands to
draw.

| Command | Effect |
|---|---|
| `music` | `dbg: music source=<mic\|imu\|none> loudness=X.XX bpm=XX.X frame_ms=<X.XX\|n/a> canvas_us=<N\|n/a> lvgl_refresh_avg_us=<N\|n/a> lvgl_refresh_max_us=<N>` (the `lvgl_refresh_*` pair collapses to the single fragment `lvgl_refresh=n/a` when that window hasn't closed yet, and both `lvgl_refresh_*` fields are omitted entirely alongside a `frame_ms=n/a canvas_us=n/a` — see `dbgconsole_music`'s own doc comment) |
| `music seed <n>` | reseeds the swarm (bench determinism); `dbg: music seed=<n>` |

## Sim fixtures

Four fixtures (`firmware/tests/fixtures/music_swarm_*.json`), one per
honesty/loudness state the concept sheet names, all sharing the same
`seed` (424242) so their particle LAYOUTS are directly comparable
frame to frame — only chrome/glow/pull differ. **Regenerated for the
S31 polish** (new motion model + no QUIET/LOUD word + narrowed chip
visibility — see "S31 polish" above); no MIC/IMU/NO SOURCE word is
rendered any more for the MIC case specifically, since that chip is
no longer built at all while `source == MIC`:

- `music_swarm_quiet.json` — a steady, quiet MIC reading
  (`mic_stream: {"kind":"static","level":0.08}`) — no chip (MIC), calm
  idle glow.
- `music_swarm_loud.json` — a 128 BPM click train, captured mid-beat
  (`mic_stream: {"kind":"click","bpm":128,"loud":0.85}`) — no chip
  (MIC), the swarm mid-beat-pull.
- `music_swarm_imu.json` — the IMU fallback source, a moderate
  loudness (`source: "imu"`, `loudness: 0.55`) — amber IMU chip.
- `music_swarm_nosource.json` — honestly `source: "none"`, `loudness:
  0` — muted "NO SOURCE" chip, never an alarm.

`ff_app_music_t`'s fixture schema (a `music` section, plus the `music`
member of `fx_face_table`) is documented in
`firmware/tests/fixtures/README.md`. The `mic_stream` key is
convenience sugar (`kind: "static"|"click"`) applied BEFORE the direct
`loudness`/`beat_count`/`source` keys, so a fixture can always override
what the sugar derived.

**Regenerated AGAIN, deliberately, 2026-09-09 (canvas renderer)**: the
same four fixtures/seed above, byte-identical FIXTURE JSON — only the
committed PNGs changed, since the renderer (not the motion model, not
any fixture) is what moved: `music_swarm_quiet.png`, `music_swarm_
loud.png`, `music_swarm_imu.png`, `music_swarm_nosource.png`. Verified
determinism-clean (byte-identical across two renders each, and 0/169744
pixels differing across the clang and gcc-14 sim builds — see this
PR's own gate results); every one of the other 98 pre-existing goldens
(102 total minus these 4) is untouched (confirmed via `git status`
after `tests/run_goldens.sh --update-golden`).

## Interpretation calls / questions (flagged per AGENTS.md)

- Every numeric constant in `ff_beat.h`/`ff_swarm.h` (floor/ceil
  attack-release windows, onset threshold, refractory, drift velocity
  ranges, envelope decay rate, pull-strength loudness base, twinkle
  floor/gain/rate, halo opacity gain) is a judgment call — no existing
  spec pinned any of them before this change (or before the S31 polish,
  for the motion-model constants specifically). The bench protocol
  below is the mechanism to find out whether they feel right on glass;
  nothing here claims they are correct, only plausible (same posture
  `ff_audio.h`'s `FF_AUDIO_AMPLITUDE`/S30's dBFS range already carry).
- The launcher's fifth-satellite `compass_pos` (288°, appended after
  Map) is a defensible reading of "adding a fifth app" with no
  design-canvas pentagon reference in-tree to consult; a real mockup
  could place Music somewhere else in the ring.
- The Music launcher icon (three fireflies) has no mockup reference
  either — an intentional departure from a generic "music note" glyph,
  flagged as a judgment call the coordinator/owner may want to revise.
- **Known IMU-fallback gap** (see "Power policy" above): unavailable
  on a puck with no magnetometer wired, since `ff_compass_read()` has
  no IMU-only sampling path. Not fixed here — flagged as a natural
  follow-up once a real consumer needs it independent of the
  magnetometer.
- `music seed <n>`'s bound (0-999, `parse_u32_dec`'s own 3-digit
  ceiling, same helper `mic watch`'s `<secs>` argument reuses) is
  "cheap" per the task's own wording, not a considered domain limit —
  a bench operator wanting a specific larger seed can extend the parser
  later if that ever matters.

## Acceptance criteria

- **AC1** — `ff_beat_update` computes `loudness` via the documented
  auto-ranging floor/ceiling mapping, floored/clamped, never NaN/-inf,
  for both MIC (`env_dbfs`) and IMU (converted `accel_mag_g`) sources.
- **AC2** — A 128 BPM MIC click train produces beats within ±30 ms of
  each click (`test_beat.c`); silence and a slow swell produce none; a
  2 Hz IMU bounce produces 2 beats/s.
- **AC3** — `ff_swarm_step` is a pure, deterministic function of
  (state, loudness, beat_now, dt_s): the same seed + stimulus sequence
  reproduces bit-identical particle state (`test_swarm.c`).
- **AC4** — Music is reachable from the launcher (idx 5), rendered as
  the fifth compass-ring satellite at the N-agnostic formula's next
  slot, with an updated, deterministic set of goldens (listed above).
- **AC5** — `music.beat_count`/`bpm_estimate`/`loudness` (S31 polish:
  `loudness` joined the other two, unconditionally zeroed, once the
  QUIET/LOUD word it used to bucket for was removed) never contribute
  to the shell's render key; `music.source` contributes only while
  Music is the active face, exactly matching what the chrome draws
  (`S16_render_key_churn_budget_music` ≤ 60/min;
  `S31_music_loudness_never_dirties_render_key` pins the zeroing
  directly — a settled quiet baseline AND a swing all the way to a
  sustained loud level both produce zero dirty ticks).
- **AC6** — `ff_mic_start`/`stop` fire exactly on the
  enter/leave-Music (or DIM/OFF/takeover) edge, never continuously, via
  the shared `ff_shell_music_wants_mic` predicate. **AMENDED
  2026-09-09**: `ff_shell_keep_awake` no longer has a Music branch at
  all — see "Power policy" > "Keep-awake (REMOVED, 2026-09-09
  amendment)" above for the bench evidence and root cause. Music obeys
  the same DIM-at-15s/OFF-at-30s S26 timers as every other face,
  regardless of loudness; the mic-power half of AC6 above is unchanged
  (it was always gated on the idle FSM's own ACTIVE output, not on
  loudness).
- **AC7** — BACK/HOME rim gestures work unchanged on the Music face
  (`test_gesture_glue.c`'s `S31_back_on_music_goes_home`, which
  exercises a full Music screen build+teardown — the same test that
  caught the 180-object LVGL-heap crash during the S31 polish pass).
- **AC8** — The source chip (S31 polish: shown only while `source !=
  FF_APP_MUSIC_SRC_MIC`) always shows IMU or NO SOURCE — never
  blended, never fabricated — matching whichever source
  `ff_shell_set_beat_input` actually fed the detector; with the mic
  running, no chip is built at all.
- **AC9** — clang and gcc-14 sim builds are warning-clean; `ctest`
  passes in full (including the S16 churn budgets and the
  `test_beat`/`test_swarm` suites); all 102 goldens pass (98 untouched
  by the S31 polish + the 4 regenerated Music fixtures listed above),
  byte-identical across both compilers; the ESP32-S3 device build
  (both the bench sdkconfig and `sdkconfig.ci`) is warning-clean.
- **AC10** (2026-09-09 canvas renderer) — the swarm renders from ONE
  `lv_canvas` (see "2026-09-09 amendment" above), its pixel buffer
  confirmed to live in PSRAM on the esp32s3 target (`heap_caps_malloc`
  with `MALLOC_CAP_SPIRAM`, never the LVGL heap arena) and plain
  `malloc` in the sim; the four Music goldens are regenerated
  deliberately and every other golden is untouched (this PR's own
  "Sim fixtures" amendment above); clang and gcc-14 sim builds stay
  warning-clean and `ctest` stays green, including the S16 churn
  budgets (the canvas touches no `ff_app_state_t` field, so `shell_
  render_key`'s existing `music.*` masking is unaffected); the `music`
  console line carries the new `frame_ms=.../canvas_us=...` fragment
  (honest `n/a` when unavailable) and the `perf` command's `lvgl_
  refresh` line — the tool that found the original regression — still
  works; the ESP32-S3 device build (bench sdkconfig, minus `CONFIG_
  FREERTOS_USE_TRACE_FACILITY`, plus `CONFIG_FF_DEBUG_CONSOLE=y` and
  `CONFIG_FF_COMPASS=y`) is warning-clean.

## Bench acceptance protocol (for the coordinator)

Run on the real puck, `CONFIG_FF_DEBUG_CONSOLE=y`:

1. From the launcher, tap the MUSIC circle (top-left satellite). The
   face opens; with the mic running (the normal case), no chip shows at
   all — just the clock and the swarm (S31 polish). Pull the mic (or
   otherwise force an IMU/no-source fallback) and the chip reads `IMU`
   or `NO SOURCE`, honestly.
2. Clap near the puck: a visible inward pull pulse on the swarm (every
   firefly's radius snapping toward the shared pull target together),
   and `music` on the console shows `beat`-driven `bpm` climbing toward
   a plausible value within a few claps.
3. Cover the mic port with a finger: the swarm calms to its idle
   drift-and-twinkle within about 2 seconds (the auto-ranging
   floor/ceiling and the fast/slow onset filters both settle well
   inside that window) — fireflies keep wandering, never freeze.
4. `mic` on the console (S30's own command) shows `running=1` only
   while the Music face is the one on screen — leave the face (BACK or
   HOME) and `mic` immediately after shows `running=0`.
5. `music seed 7` then re-enter the Music face: the swarm's layout
   changes (a different, but still deterministic, arrangement) —
   useful for a bench operator comparing two runs side by side.
