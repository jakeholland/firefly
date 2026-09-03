# S27 — Sounds

Status: draft (2026-09-03). This spec covers the CORE + SHELL + SETTINGS +
SIM half only (`[api]`) — the DEVICE HAL that turns a pattern into actual
PCM samples on the Waveshare board's PCM5101 I2S DAC (GPIO48/38/47,
44.1 kHz 16-bit) is a separate, stacked PR. The reference firmware has no
tone generator, so tones are synthesized in firmware by that HAL, not
played from an audio file.

## Why

The puck is silent today — every alert is haptic-or-visual only. The
maintainer's ask: "we should add noises overall." A small, fixed
vocabulary of short tone patterns gives the puck a second sense a wearer
can react to without looking at the screen, without asking for a full
audio subsystem (no files, no mixer, no volume curve — six hand-picked
patterns and a policy for when they may play).

## Vocabulary

One `ff_sound_event_t` per distinct "why did the puck just make a noise":

| Event | Trigger | Shape |
|---|---|---|
| `FF_SOUND_FLARE_SENT` | Any flare-send trigger (Radar CLOSE-mode FLARE button; the 5x-HOME quick-flare multitap) — confirmation for the SENDER that a flare went out | Short rising 3-note |
| `FF_SOUND_FLARE_INCOMING` | A paired crew member's flare just took over the screen | Urgent, repeated twice — the loudest thing the puck makes |
| `FF_SOUND_MESSAGE` | New inbox message / banner | One soft blip |
| `FF_SOUND_RALLY` | Rally point received | Two-note |
| `FF_SOUND_BATT_LOW` | Battery crosses INTO the low band (`<= FF_BATT_LOW_PCT`, `ff_radar.h`) | Descending two-note |
| `FF_SOUND_TAP` | UI tick on button press | Single short click; see Policy below — gated by a second setting, default OFF |

## Patterns are data

```c
typedef struct { uint16_t freq_hz; uint16_t ms; } ff_sound_step_t; /* freq 0 = rest */
typedef struct { ff_sound_step_t steps[FF_SOUND_PATTERN_MAX_STEPS]; uint8_t n; } ff_sound_pattern_t;
ff_sound_pattern_t const *ff_sound_pattern_for(ff_sound_event_t ev);
```

`FF_SOUND_PATTERN_MAX_STEPS` = 8, `FF_SOUND_PATTERN_MAX_MS` = 1500 —
every pattern in the table satisfies both, enforced as a RUNTIME property
by `test_sound.c`'s sweep (not `_Static_assert`: reading an element out
of a `static const` array at a compile-time-known index is not an
integer constant expression per C11 §6.6p6 — clang accepts it as an
extension, GCC (CI's authority, `CLAUDE.md`) does not; see
`core/src/ff_sound.c`'s own comment on the earlier version of this file
that hit exactly that).

Note choices (equal-temperament, A4=440):

| Event | Notes | Reasoning |
|---|---|---|
| FLARE_SENT | C5 – E5 – G5 (rising major triad, 90/90/130 ms) | "confirmation" — the classic settled rising shape |
| FLARE_INCOMING | C6–G5 chirp, rest, C6–G5 chirp again (110/110/150/110/110 ms) | "repeated twice", per spec |
| MESSAGE | E5 (80 ms) | One soft blip — deliberately the shortest, quietest entry so it never competes with the two flare sounds |
| RALLY | G5 – B5 (110/150 ms, rising) | Two-note, distinct shape from BATT_LOW's falling pair |
| BATT_LOW | E5 – C5 (150/220 ms, falling) | Descending shape reads as "going down" |
| TAP | 1200 Hz, 20 ms | A tick, not a musical phrase |

## Policy — `ff_sound_should_play`

```c
bool ff_sound_should_play(ff_sound_event_t ev, bool sounds_on, bool quiet_now);
```

- `sounds_on == false` → **nothing plays**, no exception (not even
  FLARE_INCOMING — unlike haptics/quiet-hours, where the flare alert
  overrides the WINDOW but never the user's own master switch, there is
  no "sounds off but flare should sound anyway" case here).
- `quiet_now == true` (from `ff_quiet_now`) → **only**
  `FF_SOUND_FLARE_SENT` and `FF_SOUND_FLARE_INCOMING` still play — safety
  beats quiet. **Interpretation call** (flagged per `AGENTS.md`):
  FLARE_SENT is included alongside FLARE_INCOMING, not just the receive
  side, because the sender needs the same confirmation their own flare
  went out regardless of the hour. See "Questions" below for the
  counter-argument.
- Otherwise every event plays.
- An `ev` outside the vocabulary → `false` (reject, not guess).

`ff_sound_should_play` does **not** know about `ui_ticks` — TAP's second
gate is composed by the caller (`ff_shell_should_tap_sound`, below); the
3-argument shape is fixed and has no slot for a setting that applies to
exactly one event.

## Priority / preemption — `ff_sound_priority`

The device HAL has one speaker; at most one pattern plays. Three tiers
(gaps left for future events to slot into without renumbering):

| Tier | Value | Events |
|---|---|---|
| URGENT | 20 | FLARE_INCOMING |
| NORMAL | 10 | FLARE_SENT, RALLY, BATT_LOW |
| LOW | 0 | MESSAGE, TAP |

`ff_sound_preempts(incoming, playing)` = `priority(incoming) >=
priority(playing)`. Consequences, both spec-mandated and both simple
results of the tier table (not special-cased):

- **FLARE_INCOMING preempts anything** — URGENT beats every tier,
  including a currently-playing FLARE_INCOMING of its own ("newest
  wins", mirroring `ff_flare.h`'s own takeover rule).
- **A MESSAGE never interrupts a FLARE_\*** — LOW never beats NORMAL or
  URGENT.

If nothing is currently playing, the HAL has nothing to ask
`ff_sound_preempts` at all — it just plays the incoming event.

## Settings — format v9 (`[api]`)

Two new fields on `ff_settings_t`, forward-migrated per the house rule
(v6→v7→v8→v9 chain, `≤v5` still rejects — see `ff_settings.c`'s v9
comment for the full migration reasoning):

- `bool sounds_on` — default **TRUE**. The master switch. Unlike every
  earlier migrated-in field (which always lands at its "never had it"
  false), a migrated v8/v7/v6 blob lands `sounds_on` at TRUE too — sound
  is an opt-OUT feature here (the maintainer's own framing: "we should
  add noises overall"), the same precedent `haptics` (also default true)
  already sets.
- `bool ui_ticks` — default **FALSE**. TAP's second gate. Opt-IN: a tick
  on every single press is easy to find annoying fast, so this ships
  silent and the maintainer can flip the default later.

Settings screen: two new toggle rows, **SOUNDS** and **UI TICKS**, sit
directly under HAPTICS (the pill/toggle pattern `scr_settings.c` already
uses for every other two-state row); every row from GLOW down shifts by
two more `FF_SETTINGS_ROW_STEP` (the scroll list absorbs it — the same
mechanics CLOCK's and SCREEN's own insertions established). Both rows
emit `FF_INTENT_SETTING_SET` with `FF_SETTING_SOUNDS_ON`/
`FF_SETTING_UI_TICKS` through the existing `shell_setting_set` seam.

## Shell seam

`ff_shell_cfg_t` gains a NULL-safe device HAL hook, mirroring
`power_off`:

```c
void (*play_sound)(void *user, ff_sound_event_t ev);
void *play_sound_user;
```

A shell-private helper, `shell_sound(sh, ev)`, is the ONE place every
shell-driven event (everything except TAP) funnels through: it evaluates
`ff_sound_should_play(ev, settings.sounds_on, shell_quiet_now(sh))`
against the live settings + wall clock, and only then calls
`play_sound`. Call sites:

- **`FF_INTENT_FLARE_START`** (any trigger) → `FLARE_SENT`, right after
  `ff_flare_send_begin` (which is unconditional w.r.t. receive, per
  `ff_flare.h`, so this always fires when the intent is reachable).
- **Inbound FLARE, `r.should_alert`** (the same gate the flare-alert
  haptic uses — an unpaired sender never reaches here at all,
  `ff_flare_on_flare_rx`'s own trust gate) → `FLARE_INCOMING`.
- **`shell_notify_push_banner`** (S26(d)'s one call site for both
  banner-eligible kinds, already re-checked paired) → `MESSAGE` or
  `RALLY` depending on `ff_notify_kind_t` — but **only when
  `ff_notify_push` reports `FF_NOTIFY_PUSH_NEW`**, `[api]` amendment
  (PR #184 review): `ff_notify_push`'s return type changed from a bare
  `bool` to `ff_notify_push_result_t` (`REJECTED`/`NEW`/`COALESCED`,
  `core/include/ff_notify.h`) precisely so this call site can tell a
  genuinely new notification from a coalesced refresh of the one
  already queued (two texts from the same sender within the notify
  queue's 2s coalesce window collapse into ONE banner entry,
  `ff_notify.h`'s own documented behavior) — a coalesced update still
  wakes the screen (`wake_pending`) but does not get a second sound.
  See `test_notify.c`'s `S27_push_result_new_vs_coalesced_vs_overflow`
  and `test_shell.c`'s
  `S27_coalesced_message_from_same_sender_sounds_once_different_sender_sounds_again`.
- **`shell_project`, right after `batt_pct` is projected** → `BATT_LOW`,
  once per CROSSING into the low band (`ff_radar_batt_is_low`), tracked
  by a shell-owned `bool batt_was_low` edge detector — not a level check,
  so a battery that stays low for hours sounds exactly once.

### Battery: rides on #180's real gauge (merged concurrently)

`view.radar.batt_pct` is `sh->batt_filter.displayed_pct` — S25 slice c's
mV→percent gauge + display filter (`ff_batt.h`, `ff_shell_set_batt_mv`),
which landed on `main` while this PR was in flight and was merged into
this branch rather than worked around. The BATT_LOW crossing detector
sits at the exact same projection line either way (a single `bool
batt_was_low` edge detector reading `ff_radar_batt_is_low(view.radar.
batt_pct)`), so no separate test/dev seam is needed — `test_shell.c`
drives it through the real `ff_shell_set_batt_mv` push API, same as any
other caller would.

### TAP — a second seam (say what we chose)

The shell never sees a raw button press — only the screens layer's
shared button base (`ff_scr_button_create`, `scr_nav.c`, the ONE choke
point every button funnels through) does, via LVGL's `LV_EVENT_CLICKED`
(a completed tap, not a press that turns into a scroll drag). Two
options were on the table: (a) expose `ff_shell_should_tap_sound(sh)`
and have the screen call it directly, or (b) put the decision in the
view.

**Chosen: both, composed.** A new, dependency-free process-global seam,
`ff_sound_emit`/`ff_sound_emit_bind` (`app/include/ff_sound_emit.h`,
mirroring `ff_intent.h`'s emit/bind shape exactly), is what
`ff_scr_button_create` calls UNCONDITIONALLY on every click — the screen
makes no gating decision, same as how a screen never decides what an
intent does. The target binds this seam to a new adapter,
`ff_shell_sound_sink` (mirrors `ff_shell_intent_sink`), which is where
`ff_shell_should_tap_sound(sh)` — `ui_ticks && ff_sound_should_play(
FF_SOUND_TAP, sounds_on, quiet_now)` — is actually asked, and only then
calls the SAME per-shell `play_sound` hook every other event uses. So
the device HAL sees one uniform call site regardless of which of the six
events fired it, and `ff_shell_should_tap_sound` is exposed publicly too
so it is unit-testable on its own.

`ff_sound_emit.c` is built into the SAME CMake library as `ff_intent.c`
(`FF_INTENT_SOURCES`) rather than getting its own — the esp32s3 target's
`ff_app`/`ff_app_ui` components list `FF_INTENT_SOURCES` by name and
this PR does not touch `firmware/targets/esp32s3/**`, so this is what
lets the new seam reach the device build with zero changes there.

## Sim

`targets/sim/ctl_loop.c`'s `ctl_loop_play_sound_cb` is the sim's
`play_sound` hook: logs to stderr (`ffsim: sound <NAME>`) and appends to
a small fixed-capacity, ctl-observable log (`ff_ctl_loop_sound_log_count`/
`_at`, `ctl_loop.h`) so a test can assert "FLARE_SENT was played" through
the real wiring, not just the shell's own hooks
(`test_ctl_flare_sequence.c`'s `S27_ctl_loop_play_sound_hook_logs_flare_sent`).
The windowed (`--connect`) path gets the same stderr log via
`ff_win_play_sound_cb` in `main.c`. Both bind `ff_sound_emit_bind(
ff_shell_sound_sink, shell)` alongside the existing intent bind, and
`ff_ctl_loop_close` unbinds it before the shell goes away (same LIFETIME
contract as the intent seam).

## Acceptance criteria

- **AC1** `ff_sound_pattern_for` returns the documented pattern for every
  event, NULL outside the vocabulary; every pattern satisfies the
  step/duration budgets (runtime-swept, `test_sound.c`).
- **AC2** `ff_sound_should_play`: sounds off silences everything
  including FLARE_INCOMING; quiet hours exempts only the two FLARE
  events; otherwise every event plays; out-of-range rejects.
- **AC3** `ff_sound_priority`/`ff_sound_preempts`: FLARE_INCOMING is the
  highest tier and preempts anything including itself; MESSAGE never
  preempts a FLARE event.
- **AC4** Settings format v9: `sounds_on` defaults true, `ui_ticks`
  defaults false; a v6/v7/v8 blob forward-migrates every prior value and
  lands the two new fields at their own honest defaults; a wrong-size/
  truncated v8 blob rejects to full defaults (mirrors #171's v6/v7 guard
  tests). SOUNDS/UI TICKS rows present and reachable in the scrolling
  list; the hit-target sweep and `S21_AC1_..._every_row_reachable`'s row
  list stay green.
- **AC5** Each shell call site fires its event exactly once with the
  expected event, through the real `ff_shell_intent`/`ff_shell_events`
  paths: FLARE_START → FLARE_SENT; a paired inbound flare →
  FLARE_INCOMING (an unpaired one → nothing); a paired MESSAGE/RALLY →
  the matching sound (unpaired → nothing, S22 stranger rule); sounds_on
  = false → none of the above; quiet hours → only the two FLARE events.
- **AC6** BATT_LOW fires exactly once on a crossing into the low band,
  never again while it stays low, and again on a genuine new crossing
  after recovering; an unknown reading never fires it; the boundary
  (`== FF_BATT_LOW_PCT`) is inclusive.
- **AC7** `ff_shell_should_tap_sound` composes `ui_ticks` AND the
  sounds_on/quiet policy correctly in all four combinations; a real click
  through `ff_scr_button_create` reaches `ff_sound_emit` unconditionally,
  and `ff_shell_sound_sink` gates it correctly end to end.

## Mutations checked (fresh, hash-verified, targeted reverts)

- **(a)** Drop the quiet-hours exemption for FLARE_INCOMING in
  `ff_sound_should_play` → `test_sound.c`'s
  `S27_quiet_hours_exempts_only_the_two_flare_events` fails, AND
  `test_shell.c`'s `S27_quiet_hours_only_the_two_flare_events_play` fails
  (both layers — the core policy and the shell's real call sites).
- **(b)** Fire BATT_LOW on every tick it reads low, not just the crossing
  (drop the `!sh->batt_was_low` guard in `shell_project`) →
  `test_shell.c`'s `S27_batt_low_fires_once_per_crossing_not_every_tick`
  fails (`Expected 1 Was 21`).
- **(c)** Drop the `ui_ticks` default in the v8→v9 migration (flipped to
  a wrong value, since the field's zero-init default happens to coincide
  with the honest default — a bare line-removal would be invisible for
  exactly that reason, see `ff_settings.c`'s v9 migration comment) →
  three `test_settings.c` tests fail
  (`S21_v6_blob_forward_migrates_preserving_every_value`,
  `S21_v7_blob_forward_migrates_preserving_every_value`,
  `S27_v8_blob_forward_migrates_preserving_every_value`).
- **(d)** (PR #184 review) Sound MESSAGE/RALLY unconditionally on every
  `ff_notify_push`, ignoring the NEW-vs-COALESCED result → `test_shell.c`'s
  `S27_coalesced_message_from_same_sender_sounds_once_different_sender_sounds_again`
  fails (`Expected 1 Was 2`).

See the PR body for exact `ctest` output.

## Questions

- **Should FLARE_SENT really be exempt from quiet hours, or only
  FLARE_INCOMING?** The spec text names both; the counter-argument is
  that the sender already gets a full-screen amber "you are flaring"
  state and the receive side's takeover/haptic already breaks quiet
  hours — the SOUND specifically could stay quiet-gated on the sending
  side without losing any safety property, since the sender is looking
  at the screen already. Shipped as written (both exempt); flag if the
  maintainer disagrees.
- **TAP's default OFF** — noted in the settings section above, not
  reopened here; flip the default in `ff_settings_apply_defaults` if
  field use says otherwise.

## Amendments

- **2026-09-03 — device half lands (`feat/s27-sounds-device`, stacked on
  this PR's `feat/s27-sounds-core`).** This spec's own intro says the
  device HAL is "a separate, stacked PR" — it has now landed as a new
  `firmware/targets/esp32s3/components/ff_audio/` component
  (`ff_audio.h`/`ff_audio.c`), plus the `app_main.c` wiring that connects
  `ff_shell_cfg_t.play_sound` and the TAP seam (`ff_sound_emit_bind`) to
  it. Nothing in this Amendment changes the core+shell+settings+sim
  contract described above — it is exactly the "DEVICE HAL that turns a
  `ff_sound_pattern_t` into actual PCM samples" this spec always deferred.

  **Hardware** (Waveshare ESP32-S3-Touch-LCD-1.46 reference
  `Audio_Driver/PCM5101.c`,
  https://github.com/yaosy1997/ESP32-S3-Touch-LCD-1.46-Test): a PCM5101
  I2S DAC feeding the onboard speaker. I2S port 0, standard (Philips)
  format, BCLK GPIO48, WS/LRCK GPIO38, DOUT GPIO47; MCLK and DIN are not
  wired on this board. 44.1 kHz, 16-bit, stereo slots
  (`I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG`) — every pattern is a single mono
  tone, duplicated into both slots to match the wiring. The reference
  driver only plays pre-recorded files and applies volume as a software
  sample-scale before `i2s_channel_write` (mute is a no-op there, and
  there is no PA-enable pin on this board); this repo synthesizes tones
  instead of playing files, so only the I2S wiring/format is shared with
  the reference, not its playback code.

  **Synthesis.** Each `{freq_hz, ms}` step is rendered as a SINE wave
  (not square — a square wave is harsh/buzzy on a speaker this small; a
  sine reads as a chime) with a 5 ms linear attack and 10 ms linear
  release per note to avoid clicking the speaker cone, at a fixed
  `FF_AUDIO_AMPLITUDE` (~25% of INT16 full scale to start — **no scope,
  no calibrated-loudness reference for this speaker/enclosure exists
  yet; this is a conservative starting guess the maintainer is expected
  to re-tune on glass, flagged as an interpretation call per AGENTS.md**;
  see the on-glass steps below and `ff_audio.c`'s tunable). `freq_hz ==
  0` (a rest) is true silence, no envelope. Samples are written in ~10 ms
  chunks so a preemption request takes effect within one chunk, not only
  at a step/pattern boundary.

  **Preemption / concurrency — a mailbox, not a queue (interpretation
  call).** `ff_audio_play` is non-blocking: it hands the event to a
  dedicated FreeRTOS render task through a 1-deep mailbox and returns.
  Nothing playing → the new event starts. Something already playing →
  `ff_sound_preempts(incoming, playing)` (core, unchanged by this PR)
  decides: preempt → the current pattern stops at the next ~10 ms chunk
  boundary and the new one starts; otherwise the incoming event is
  silently DROPPED. A queue was deliberately NOT used — this puck's "second
  sense" sounds are meant to be immediate feedback; a MESSAGE chime that
  fires three seconds late (because it was queued behind two higher-tier
  sounds) is more confusing than no sound at all. At most one sound is
  ever "in flight" plus one "about to start."

  **Failure handling.** `ff_audio_init` failures (I2S channel alloc,
  GPIO conflict, task/queue creation) are logged and non-fatal — the
  puck must boot and be fully usable with no speaker. Every
  `ff_audio_play` call after a failed/skipped init is a silent no-op,
  logged once (not per call).

  **I2S channel enable/disable strategy (interpretation call, no scope
  used).** The channel is enabled only while a pattern is actively
  rendering and disabled at pattern end — chosen over "keep it enabled,
  write silence between patterns" because sounds are short (≤1.5 s) and
  infrequent, so the vast majority of runtime has no sound playing at
  all; keeping the channel/DMA/clock alive the whole time would burn
  power for essentially no benefit. The pop/click risk a hard
  enable/disable could cause is exactly what the attack/release envelope
  already prevents (every note ramps to zero amplitude before disable
  runs; a cold enable starts the DMA before any nonzero sample exists).
  `FF_AUDIO_KEEP_CHANNEL_ENABLED` in `ff_audio.c` is the tunable if
  on-glass testing proves this reasoning wrong.

  **Light sleep (S26f interaction).** This project runs with
  `CONFIG_PM_ENABLE` unset, so `esp_driver_i2s`'s own PM-lock guard never
  engages — nothing in the I2S driver itself stops
  `esp_light_sleep_start()` from cutting a pattern mid-flight. `bool
  ff_audio_busy(void)` reports "audio has unfinished work" (rendering, or
  a just-queued request the render task hasn't started yet) and
  `app_main.c` composes it into the SAME `sleep_inhibit` parameter S26f's
  USB amendment already added (`ff_idle_tick`'s 4th argument):
  `usb_connected || ff_audio_busy()`. Because the channel is only ever
  enabled while busy is true (previous paragraph), the channel should in
  practice never be enabled at the moment sleep is entered — but
  `ff_audio_channel_enable_with_recovery` (`ff_audio.c`) is defensive
  anyway: if `i2s_channel_enable` ever fails post-wake, it logs once and
  does a full re-init (delete + recreate the channel) rather than trust a
  half-recovered peripheral, then retries. A pattern that hits this path
  is dropped (logged), never a crash or a wedge.

  **`CONFIG_FF_AUDIO_SELFTEST`** (Kconfig, default n,
  `firmware/targets/esp32s3/main/Kconfig.projbuild`): plays FLARE_SENT
  once, right after the first face is on glass, so the maintainer can
  hear the speaker without triggering a real flare (which takes over the
  whole UI). Compiles out entirely of a normal build.

  **Touched files (device half only — this Amendment does not modify
  any core/shell/settings/sim file from the section above):**
  `firmware/targets/esp32s3/components/ff_audio/{include/ff_audio.h,
  ff_audio.c, CMakeLists.txt}`, `firmware/targets/esp32s3/main/app_main.c`
  (hook wiring + `ff_audio_init` call + `sleep_inhibit` composition),
  `firmware/targets/esp32s3/main/CMakeLists.txt` (`REQUIRES ff_audio`),
  `firmware/targets/esp32s3/main/Kconfig.projbuild` (the selftest option —
  not in this PR's original touch list but required to deliver it; flagged
  here as the interpretation call, AGENTS.md), and this spec file.

  ### On-glass steps

  1. **Hear it:** menuconfig → Firefly bring-up → enable
     `CONFIG_FF_AUDIO_SELFTEST`, flash, boot to STAGE ≥2. Hear the
     FLARE_SENT 3-note rising chime once, shortly after the first face
     renders. Disable the option again before a field build.
  2. **Quick-flare, once #183 (5×-HOME) lands:** from Home, tap HOME five
     times quickly → the quick-flare chime (FLARE_SENT) plays alongside
     the flare overlay.
  3. **Settings → SOUNDS off:** every sound (including FLARE_INCOMING)
     goes silent — the master switch has no exception on-device either,
     matching `ff_sound_should_play`'s core contract.
  4. **Settings → UI TICKS on:** a tick plays on every button press
     across every screen (the `ff_scr_button_create` choke point) —
     confirms the TAP seam (`ff_sound_emit` → `ff_shell_sound_sink`)
     reaches the device HAL end to end.
  5. **Amplitude tuning:** with the puck in hand (not on a bench), adjust
     `FF_AUDIO_AMPLITUDE` in `ff_audio.c` up/down from its ~25%-full-scale
     starting point until FLARE_INCOMING ("the loudest thing the puck
     makes") is clearly audible in a pocket without distorting the
     speaker at that level, and MESSAGE's soft blip is still audibly
     softer than it. Re-flash and re-listen; no build-system change
     needed, it's one `#define`.
