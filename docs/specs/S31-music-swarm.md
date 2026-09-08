# S31 — Music/Swarm: the fifth launcher app

Status: draft (2026-09-08). Builds on [S30](S30-audio-input.md) (mic
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
third object per firefly again.

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

`ff_mic_start()`/`ff_mic_stop()` (app_main.c, the esp32s3 target — the
shell never calls these directly; it has no `ff_mic.h` dependency, per
CLAUDE.md's placement rule) are driven by one boolean, re-derived every
main-loop iteration:

```c
music_wants_mic = (active_face == FF_APP_FACE_MUSIC)
                && !flare.takeover_active
                && (idle_state == FF_IDLE_STATE_ACTIVE);
```

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

**Keep-awake**: `ff_shell_keep_awake` gets one new branch —
`active_face == FF_APP_FACE_MUSIC && view->music.loudness >
FF_BEAT_KEEPAWAKE_LOUDNESS` (0.05) — the face keeps the puck awake only
while it is genuinely hearing/feeling something above the auto-ranged
floor. A silent, still room lets the idle FSM dim/sleep normally, even
with Music on screen; "do not keep a silent room awake forever" (the
task's own wording).

`sleep_inhibit` already includes `ff_mic_status().running` (S30) — a
mic sample in flight is never cut off mid-frame by
`esp_light_sleep_start()`; this PR adds no second inhibit source.

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

Two commands, following S30's `mic`/`mic watch` shape but WITHOUT a
platform hook: `ff_beat_t` is core state the shell already owns on
every target (unlike `ff_mic`, esp32s3-only), so `music`/`music seed
<n>` are real (non-"unavailable") on both the device and the sim —
`ff_debug_console.c` reaches them through two new public getters/
setters (`ff_shell_music_debug`, `ff_shell_set_music_seed`), the same
"public getter, never reach into `shell_t`" rule this file's own top
comment states for every read-only command.

| Command | Effect |
|---|---|
| `music` | `dbg: music source=<mic\|imu\|none> loudness=X.XX bpm=XX.X` |
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
  enter/leave-Music (or DIM/OFF/takeover) edge, never continuously;
  `ff_shell_keep_awake` holds the puck awake only while
  `music.loudness > FF_BEAT_KEEPAWAKE_LOUDNESS`.
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
