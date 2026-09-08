# S30 — Audio input (mic bring-up)

Status: draft (2026-09-08). Device-only bring-up: brings the Waveshare
ESP32-S3-Touch-LCD-1.46's onboard microphone up as a first-class HAL
component (`ff_mic`) with honest diagnostics — status, level, and a
bench console — but wires it into no product face yet. S29 (FIND
mode) and a future Music/listening face are the intended consumers;
this spec's own "What comes next" section names exactly what they get
to build on and, just as importantly, what they don't get yet (no FFT).

## Why

The puck has a microphone and, until now, nothing in this codebase has
ever read it — `ff_shell_diag_debug`'s DIAGNOSTICS page and the bench
console can report link quality, position, compass, and battery, but
have no honest answer to "is the mic even working" at all. Bringing the
driver up on its own, gated behind an off-by-default power policy and
covered by the SAME honest-absence discipline `ff_compass`/`ff_audio`
already established, gives the maintainer something to point a real
finger-snap at on the bench before any product feature depends on it.

## Hardware

I2S MEMS mic, **not** PDM — behaves like an INMP441/MSM261-class
24-bit-in-32-bit device (exact part number not published by Waveshare;
verified against Waveshare's own reference firmware, `MIC_Speech.h`/
`.c`, https://github.com/yaosy1997/ESP32-S3-Touch-LCD-1.46-Test).

| Signal | GPIO |
|---|---|
| `MIC_SCK` (bit clock) | 15 |
| `MIC_WS` (word select) | 2 |
| `MIC_SD` (data in) | 39 |
| MCLK | not connected |

Confirmed conflict-free against this repo's own back-header notes
(`docs/hardware/comms-brain.md`, "The puck's back header": "15 is the
mic clock; 12/13/0/1 are free GPIO") and against every existing
`GPIO_NUM_2`/`GPIO_NUM_15`/`GPIO_NUM_39` reference under
`firmware/targets/esp32s3` (none — checked before writing a line of
this driver).

**I2S port: `I2S_NUM_1`, deliberately separate from `ff_audio`'s own
`I2S_NUM_0`** (the PCM5101 speaker TX channel, `docs/specs/S27-sounds.md`
— `firmware/targets/esp32s3/components/ff_audio`). The two never share a
port, a GPIO, or a peripheral instance; bringing the mic up never
disturbs speaker playback and vice versa (part of this spec's own bench
acceptance protocol, below, is proving that directly).

### I2S config (reference firmware, verified working on this board)

```c
i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
i2s_new_channel(&chan_cfg, NULL /* tx */, &rx_chan);

i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
    .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = GPIO_NUM_15,
        .ws   = GPIO_NUM_2,
        .dout = I2S_GPIO_UNUSED,
        .din  = GPIO_NUM_39,
    },
};
std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT; /* this mic's fixed I2S address strap */
i2s_channel_init_std_mode(rx_chan, &std_cfg);
```

No inversions. Read with `i2s_channel_read` (bounded `timeout_ms` —
**never** `portMAX_DELAY`, per this project's task-watchdog policy, PR
#239). `i2s_channel_read`'s `timeout_ms` parameter is literal
milliseconds (`esp_driver_i2s/include/driver/i2s_common.h`'s own doc
comment) — pass the millisecond value directly, not `pdMS_TO_TICKS`-
wrapped. (Real finding surfaced while writing this driver: `ff_audio.c`'s
existing `i2s_channel_write` call passes `pdMS_TO_TICKS(200)` into that
same millisecond-typed parameter, silently shrinking its intended 200ms
write budget to ~20ms at this project's `CONFIG_FREERTOS_HZ=100`. Out of
scope for this PR — `ff_audio.c` is unchanged here — flagged separately
for its own fix.)

## Format

Each 32-bit I2S word carries this mic's useful audio in its **top 24
bits**; the bottom 8 are don't-care padding (Waveshare's own reference
firmware's own documented shape for this part). `ff_mic` takes the
simpler of the two options this fact allows: right-shift each signed
32-bit word by **16** (not 8), keeping the top 16 bits as a plain
`int16_t` sample — 8 bits less precision than the full 24-bit value, in
exchange for a format every downstream consumer (dBFS math, PCM
buffers, a future FFT) already expects with zero extra conversion. A
future consumer that wants the full 24 bits can revisit this — nothing
here throws the low byte away before the shift, it is simply never
read.

16 kHz sample rate, mono (right slot only — the left slot reads silence
on this board). 20ms frames (`FF_MICLEVEL_FRAME_SAMPLES` = 320 samples,
`firmware/core/include/ff_miclevel.h`).

## Power policy — off unless a face asks

The I2S1 channel and the reader task are both **off by default**.
`ff_mic_init()` (called once from `app_main.c`, alongside `ff_audio_init`)
only allocates the channel — it stays disabled, and the reader task sits
idle, until something calls `ff_mic_start()`. Nothing in this PR calls
it: no product face reads the mic yet. `ff_mic_start()`/`ff_mic_stop()`
are the on/off switch — the bench console's `mic on`/`mic off`
(below) is the only caller today.

While running, `ff_mic_status().running` is composed into `app_main.c`'s
`sleep_inhibit` parameter alongside `ff_audio_busy()` — an active I2S1
DMA transfer must not be cut off mid-frame by `esp_light_sleep_start()`,
the same reasoning `ff_audio`'s own busy-flag inhibit already applies to
the speaker's I2S0 TX channel.

## Honesty rules

`ff_mic_status_t.present` is the one fact every consumer branches on. It
goes false in exactly two cases, both reading identically everywhere
this driver is observed (bench console, DIAGNOSTICS) — neither is a
working microphone, so neither pretends to be one:

1. **Init failure** — `ff_mic_init()` never brought the I2S1 channel up
   at all (GPIO conflict, allocation failure, ...). Non-fatal: the puck
   boots and is fully usable with no mic, same "log and continue"
   posture as `ff_audio_init`/`ff_compass_init`.
2. **Sentinel rejection** — the reader task's own "stuck data" check
   (mirrors `ff_compass`'s `FF_COMPASS_IMU_NO_DATA` discipline): a frame
   is "stuck" if every sample is either exactly 0, or byte-identical to
   the previous frame's corresponding sample. Once 1000ms of
   consecutive stuck frames accumulate, `present` flips false — logged
   once, not per frame. **Self-healing**: a single real (non-stuck)
   frame resets the counter and, if `present` had gone false, brings it
   back true — there is no hardware re-bring-up step to retry (unlike
   `ff_compass`'s QMI8658 re-init on the same shape of failure), since
   the I2S peripheral itself never reported an error in the first
   place.

`ff_mic_last_frame`/`ff_mic_level` never fabricate a reading: before the
first frame is ever read, RMS/peak/envelope are all
`FF_MICLEVEL_FLOOR_DBFS` (-120 dBFS, a real, comparable, non-`-inf`
number — never a fabricated 0 or silently-omitted field).

## Level math (`firmware/core/include/ff_miclevel.h`)

Pure, host-testable, zero I/O — the CLAUDE.md placement rule applied to
"how loud is this frame", shared verbatim by the device driver and the
Unity test (`firmware/core/tests/test_miclevel.c`):

- **DC removal**: a one-pole DC-blocking filter (`y[n] = x[n] - x[n-1] +
  0.995*y[n-1]`, ~25 Hz corner at 16 kHz) applied to every raw sample
  before any RMS/peak/sentinel math — a MEMS mic's raw output typically
  rides on a nonzero DC bias that would otherwise inflate every level
  reading.
- **RMS + peak dBFS**: per 20ms frame, against `INT16_MAX+1` = 32768
  full scale.
- **300ms-ish attack/release envelope**: 30ms attack, 300ms release
  (interpretation call, tunable — no existing spec pinned these
  numbers). Fast enough that a finger-snap is visible on the next `mic
  watch` print (250ms cadence); slow enough the printed number doesn't
  flicker between two adjacent prints.

## API (`ff_mic.h`)

```c
esp_err_t ff_mic_init(void);
void ff_mic_start(void);
void ff_mic_stop(void);

typedef struct {
    bool present;
    bool running;
    uint32_t sample_rate_hz;
    uint32_t frames_read;
    uint32_t read_errors;
    uint32_t last_read_age_ms;
} ff_mic_status_t;
ff_mic_status_t ff_mic_status(void);

typedef struct { float rms_dbfs; float peak_dbfs; float envelope_dbfs; } ff_mic_level_t;
ff_mic_level_t ff_mic_level(void);

size_t ff_mic_last_frame(int16_t *dst, size_t n); /* for future consumers — no caller in this PR */
```

## Console (`CONFIG_FF_DEBUG_CONSOLE`)

Four commands, one platform hook (`ff_dbgconsole_mic_fn`,
`firmware/app/include/ff_debug_console.h`) mirroring `perf`'s own
"handed the reply sink directly" shape — see that header's doc comment
for the full rationale, including why `mic watch` deliberately BLOCKS
the calling task for its whole duration rather than ticking
asynchronously (a small, explicitly-bounded, bench-only tradeoff).

| Command | Effect |
|---|---|
| `mic` | one-shot status + level: `present`, `running`, `rate_hz`, `frames`, `errs`, `age_ms`, `rms_dbfs`, `peak_dbfs`, `env_dbfs` — `present=0` alone when absent |
| `mic on` | start the I2S1 channel + reader task |
| `mic off` | stop them |
| `mic watch <secs>` | print RMS/peak/envelope once per 250ms, for 1-30 seconds (bounds enforced by the parser, `ff_dbgcmd.h`), then stop |

All four honestly report `present=0` on a target with no mic hardware
(the sim) or a real, absent-for-either-reason mic. Example session:

```
mic
dbg: mic present=0

mic on
dbg: mic present=1 running=1 rate_hz=16000 frames=0 errs=0 age_ms=0 rms_dbfs=-120.0 peak_dbfs=-120.0 env_dbfs=-120.0

mic watch 5
dbg: mic watch rms_dbfs=-58.2 peak_dbfs=-51.0 env_dbfs=-57.9
dbg: mic watch rms_dbfs=-57.8 peak_dbfs=-49.3 env_dbfs=-57.8
... (20 lines total, one per 250ms) ...
dbg: mic watch done

mic off
dbg: mic present=1 running=0 rate_hz=0 frames=1203 errs=0 age_ms=0 rms_dbfs=-58.1 peak_dbfs=-49.0 env_dbfs=-57.9
```

## DIAGNOSTICS (Settings → DIAGNOSTICS)

One new row, appended to the existing DEVICE section (the same "append a
fact to an existing section" precedent the DIAGNOSTICS row itself
followed when it was added to DEVICE):

| State | Row text |
|---|---|
| `present=false` | `MIC   absent` |
| `present=true, running=false` | `MIC   off` |
| `present=true, running=true`, no frame read yet | `MIC   on` |
| `present=true, running=true`, has a level | `MIC   -42 dBFS` (the smoothed envelope, whole dB) |

Fed through `ff_shell_set_mic_status` (a sibling of `ff_shell_set_
device_stats`, `app/include/ff_shell.h`) at the same 2s cadence
(`FF_DEVICE_STATS_SAMPLE_PERIOD_MS`) the free-heap/compass push already
uses. Per the render-key churn-budget rule (PR #237): the envelope dBFS
is coarsened to whole dB and, like every other DIAGNOSTICS field, zeroed
in the render key whenever the DIAGNOSTICS subview isn't the one
currently showing — a sub-dB wobble (real mic noise, or the envelope
filter settling) must never dirty the key and trigger a repaint while
the page is open; a genuine ≥1dB, rendered change must.

## Sim

The sim has no mic hardware, and this driver is confined to
`firmware/targets/esp32s3/components/ff_mic` — the same "esp32s3-only
component, never included by portable `firmware/app/` code" shape
`ff_audio`/`ff_compass` already established (their public headers are
included ONLY by `app_main.c`, never by `ff_shell.c`/`ff_debug_
console.c`). `ff_shell_set_mic_status` is simply never called on the
sim, so `ff_app_diag_t.mic_present` stays at the shell's own
zero-init `false` default — the DIAGNOSTICS page's honest "MIC absent"
requires no stub component, no sim-side `ff_mic.c`, and no extra
`#ifdef`: it falls out of the same push-API boundary crossing every
other esp32s3-only sensor already crosses. The bench console's own `mic`
hook is `NULL` on the sim build (mirrors `perf`/`i2c`'s own NULL-hook
convention), replying the single honest line `dbg: mic unavailable on
this target`. `firmware/tests/fixtures/settings_diag_full.json` sets a
running mic with a level (`mic_present`/`mic_running`/`has_mic_level`/
`mic_envelope_dbfs`); `settings_diag_unknown.json` omits all four keys,
defaulting to the same honest absent row a real target with no mic shows.

## What S29 (FIND) / a future Music face will build on this

- The 20ms-frame RMS/peak dBFS and the 300ms attack/release envelope
  (`ff_mic_level()`) are the ready-to-use "how loud is it right now"
  primitive — no consumer in this PR reads it, but it needs no further
  bring-up work to start.
- `ff_mic_last_frame()` hands out the latest DC-removed 20ms int16 frame
  for anything that needs raw-ish PCM (a future waveform display, a
  clap-detector threshold, ...).
- **Explicitly deferred, not built here**: a 256-point FFT via
  `esp-dsp` (spectral content — frequency bins, not just a single
  loudness number). Nothing in this PR adds an `esp-dsp` dependency,
  computes a spectrum, or exposes a frequency-domain API. That is a
  separate, stacked PR once a real consumer (Music, or a future
  clap-pattern recognizer) needs it — building it speculatively now
  would be scope no acceptance criterion below asks for.

## Bench acceptance protocol

Run over the USB-Serial-JTAG bench console (`CONFIG_FF_DEBUG_CONSOLE=y`),
puck on the bench, USB connected:

1. `mic` at boot shows `present=0` (nothing has called `ff_mic_start`
   yet) — or, if `CONFIG_FF_MIC=n` on this build, the same honest
   `present=0`.
2. `mic on` — reply shows `present=1 running=1`.
3. `mic watch 10` in a quiet room: RMS settles around **-60 to -45
   dBFS**, peak within roughly 10 dB of RMS.
4. Snap fingers near the puck during a `mic watch` window: peak reads
   **above -20 dBFS** on at least one line.
5. Cover the mic port with a finger: RMS visibly drops (lower dBFS,
   more negative) on the next `mic watch` window.
6. `mic off` — reply shows `running=0`; `mic` immediately after confirms
   `running=0` and, via `i2s_channel` state, the I2S1 channel is
   disabled (no further `frames`/`errs` counter movement across a
   second `mic` query a few seconds later).
7. Speaker playback still works with the mic running: start `mic on`,
   then trigger a TAP sound (`ff_audio`'s own pattern — tap the
   touchscreen, or replay the S27 bench steps) — the chime plays
   normally, proving I2S0 (speaker) and I2S1 (mic) never interfere.

## Acceptance criteria

- **AC1** — `ff_mic_init` brings up I2S1 RX (BCLK 15/WS 2/DIN 39, 16kHz
  mono) in a DISABLED state; non-fatal on failure, logged once.
- **AC2** — `ff_mic_start`/`ff_mic_stop` enable/disable the channel and
  the reader task; off by default; idempotent.
- **AC3** — `ff_mic_status()` reports `present`/`running`/`sample_rate_hz`/
  `frames_read`/`read_errors`/`last_read_age_ms`, all honest, all zeroed
  before `ff_mic_init` ever succeeds.
- **AC4** — `ff_mic_level()` reports RMS/peak dBFS for the latest 20ms
  frame plus a 300ms-ish attack/release envelope; floored at
  `FF_MICLEVEL_FLOOR_DBFS`, never `-inf`/NaN.
- **AC5** — All-zero or frame-to-frame-identical raw data for 1000ms
  straight flips `present` false, logged once; a real frame flips it
  back true.
- **AC6** — DC-offset one-pole high-pass filter is applied before RMS/
  peak/sentinel math.
- **AC7** — `mic`/`mic on`/`mic off`/`mic watch <secs>` all honestly
  report `present=0`/"unavailable on this target" absent a mic driver;
  `mic watch` bounds `<secs>` to [1, 30] at the parser.
- **AC8** — DIAGNOSTICS' MIC row shows absent/off/on/dBFS per this
  spec's own table, fed at the 2s device-stats cadence, coarsened to
  whole dB and zeroed outside the DIAGNOSTICS subview in the render key.
- **AC9** — The sim build compiles and links with zero mic-specific
  stub/glue code; `settings_diag_full`/`settings_diag_unknown` fixtures
  and their goldens exercise both the populated and absent MIC row
  states.
- **AC10** — `ff_mic`'s reader task never calls a FreeRTOS/I2S API with
  an unbounded (`portMAX_DELAY`) timeout, and self-subscribes to the
  task watchdog (PR #239's log-only policy), resetting it every loop
  iteration whether idle or running.

## Questions

- Is -60 to -45 dBFS quiet-room RMS (the bench protocol's own AC3-style
  check) actually right for THIS mic/enclosure? No calibrated-loudness
  reference exists yet — same "interpretation call, re-tune on glass"
  posture `ff_audio.h`'s `FF_AUDIO_AMPLITUDE` already carries for the
  speaker side. The bench acceptance protocol above is the mechanism to
  find out; nothing here claims the range is correct, only plausible.
- Should `mic watch` support early cancellation (e.g. typing `mic off`
  mid-watch)? Not built — the console's own USB-read loop is not polled
  again until the blocking watch call returns, so no mid-watch command
  can reach the parser at all in this design. A future async ticker
  (this spec's own "console" section names the tradeoff) would need to
  reintroduce that, if 30 seconds of an unresponsive console/UI ever
  proves too long a wait in practice.
