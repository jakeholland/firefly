/**
 * ff_audio.h — the DEVICE HAL that turns a `ff_sound_pattern_t` (core:
 * `ff_sound.h`, S27) into actual PCM samples on the Waveshare
 * ESP32-S3-Touch-LCD-1.46's onboard PCM5101 I2S DAC + speaker.
 *
 * Spec: docs/specs/S27-sounds.md, "Amendments — device half". The CORE +
 * SHELL + SETTINGS + SIM half of S27 (event vocabulary, pattern table,
 * quiet-hours/priority policy, the `play_sound` shell seam) landed in
 * `feat/s27-sounds-core` (#184) and is unchanged by this component. This
 * header is the OTHER end of that seam: `ff_shell_cfg_t.play_sound` on a
 * real device is wired (app_main.c) to a thin adapter that calls
 * `ff_audio_play` below.
 *
 * ## Hardware (Waveshare ESP32-S3-Touch-LCD-1.46 reference `Audio_Driver/
 * PCM5101.c`, https://github.com/yaosy1997/ESP32-S3-Touch-LCD-1.46-Test)
 * PCM5101 I2S DAC feeding the onboard speaker. I2S port 0, standard
 * (Philips) format, BCLK GPIO48, WS/LRCK GPIO38, DOUT GPIO47. MCLK and DIN
 * are NOT connected on this board (the DAC free-runs off BCLK/LRCK; there
 * is nothing to receive on DIN — this is a TX-only, playback-only path).
 * 44.1 kHz, 16-bit, stereo slots (the DAC is wired for two channels even
 * though every pattern this HAL renders is a single tone — see "Mono to
 * stereo" below). The reference driver only plays pre-recorded files and
 * applies volume as a software sample-scale before `i2s_channel_write`
 * (mute is a documented no-op there, and there is no PA-enable pin on this
 * board to gate) — this repo's patterns are SYNTHESIZED tones, not file
 * playback, so none of the reference driver's file/volume-table code
 * applies; only the I2S wiring and format are shared.
 *
 * ## What this HAL does and does not decide
 * Every decision about WHICH sound plays and WHEN — the event vocabulary,
 * the pattern table (which notes, which durations), sounds-on, quiet
 * hours, priority/preemption — already happened in core (`ff_sound.h`)
 * and shell (`ff_shell.c`'s `shell_sound`/`ff_shell_sound_sink`) before
 * `ff_audio_play` is ever called. This file's only job is turning an
 * ALREADY-APPROVED `ff_sound_event_t` into PCM samples on the wire. It
 * contains no policy `if` about product behavior (CLAUDE.md's house rule
 * — "all logic goes in firmware/core/"): the one `if` it does contain
 * (`ff_sound_preempts`) is arbitration between two SOUNDS already both
 * approved to play, which is exactly what a HAL with one speaker has to
 * resolve, not a product decision.
 *
 * ## Synthesis: sine, not square, with a soft envelope
 * Each `ff_sound_step_t` (`{freq_hz, ms}`) is rendered as a SINE wave (a
 * square wave is harsh and buzzy on a tiny speaker like this board's — a
 * pure tone reads as a "chime", a square wave reads as a "beep-boop"
 * alarm clock) at `FF_AUDIO_AMPLITUDE`, with a 5 ms linear attack and a
 * 10 ms linear release on every note (`freq_hz != 0`) to avoid the click
 * a hard on/off transition puts on a speaker cone. `freq_hz == 0` (a
 * REST, per `ff_sound.h`) is rendered as true silence for the step's
 * duration — no envelope needed, there is nothing to click into or out
 * of. Samples are written in ~10 ms chunks (`FF_AUDIO_CHUNK_MS`) so a
 * preemption request can take effect within one chunk's latency rather
 * than only at a step or pattern boundary.
 *
 * ## Mono to stereo
 * The DAC is wired stereo (two I2S slots) but every pattern is a single
 * tone — the same mono sample is duplicated into both the left and right
 * slot of every frame written to `i2s_channel_write`. There is no stereo
 * content anywhere in this system; this is purely to match the slot
 * config the hardware needs (`I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(...,
 * I2S_SLOT_MODE_STEREO)`), not a design choice about stereo sound.
 *
 * ## Amplitude — a documented, maintainer-tunable constant
 * `FF_AUDIO_AMPLITUDE` (ff_audio.c) is fixed at ~25% of INT16 full scale
 * to start. This project has no scope and no calibrated-loudness
 * reference for this specific speaker/enclosure — 25% is a conservative
 * starting guess (loud enough to be heard, quiet enough not to clip or
 * distort a small speaker at max excursion) that the maintainer is
 * expected to RE-TUNE ON GLASS (see `docs/specs/S27-sounds.md`'s device
 * on-glass steps) once the puck can actually be heard in hand. Nothing in
 * this codebase claims 25% is correct — it is an interpretation call,
 * flagged per AGENTS.md.
 *
 * ## Preemption, concurrency, and the one-speaker rule
 * The device has exactly one speaker; `ff_audio_play` never blocks the
 * caller (it is called from `ff_shell_sound_sink`/the shell's own
 * `play_sound` call sites, which must not stall on audio) — it hands the
 * event to a small dedicated FreeRTOS task through a 1-deep mailbox
 * (a length-1 queue, always holding at most the ONE most recent request)
 * and returns immediately. If nothing is currently playing, the new event
 * starts. If something IS currently playing, `ff_sound_preempts(incoming,
 * playing)` (core, `ff_sound.h`) decides: preempt → the current pattern
 * stops at the next ~10 ms chunk boundary and the new one starts; do NOT
 * preempt → the incoming event is silently DROPPED.
 *
 * **Interpretation call (flagged per AGENTS.md): a queue, not a mailbox,
 * would let a burst of chimes pile up and play back-to-back long after
 * the events that triggered them are stale** — a MESSAGE sound firing
 * three seconds after the message it was for is more confusing than no
 * sound at all on a "second sense" feature whose whole point is
 * immediate feedback. The 1-deep mailbox with drop-on-no-preempt is the
 * deliberate choice: at most one sound is ever "in flight" plus one
 * "about to start", and every dropped event is silently dropped — no
 * error, no fallback beep, matching the priority table's own "MESSAGE
 * never interrupts a FLARE_*" spec intent extended to "and doesn't queue
 * up behind it either".
 *
 * ## Failure handling — audio is never a boot blocker
 * `ff_audio_init` failures (I2S channel allocation, GPIO conflict, etc.)
 * are logged and non-fatal: the puck MUST boot and be usable without a
 * working speaker (a silent puck is still a working compass; a puck that
 * refuses to boot because a DAC didn't come up is not). Every subsequent
 * `ff_audio_play` call after a failed `ff_audio_init` is a silent no-op
 * (logged once, not per call, so a busy shell doesn't spam the log).
 *
 * ## Light sleep
 * A pattern in progress must not be cut off mid-note by
 * `esp_light_sleep_start()` (S26f). `ff_audio_busy()` reports "audio has
 * unfinished work" (a pattern actively rendering, OR a just-queued
 * request the task has not started yet — see ff_audio.c's own comment on
 * why the busy flag is set synchronously in `ff_audio_play`, not only
 * inside the task, to close that race) and app_main composes it into the
 * SAME `sleep_inhibit` parameter S26f's amendment already added for USB
 * (`ff_idle_tick`'s 4th argument) — `usb_connected || ff_audio_busy()`.
 * DIM/OFF are untouched; only the OFF→SLEEP transition is withheld while
 * audio is busy, exactly like the USB case.
 *
 * See ff_audio.c's own top comment for the I2S channel enable/disable
 * strategy and the post-light-sleep re-enable handling.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "ff_sound.h" /* ff_sound_event_t (core, S27) */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_audio_init — bring up I2S0 in standard (Philips) TX mode for the
 * PCM5101 (BCLK GPIO48, WS GPIO38, DOUT GPIO47; MCLK/DIN unused), 44.1 kHz
 * 16-bit stereo slots (mono content duplicated to both — see this
 * header's top comment), and start the audio render task.
 *
 * Call once, from app_main, AFTER the display bring-up (panel + boot
 * splash) so a slow or failing I2S bring-up can never delay the S26g
 * power-latch timestamp or the first splash pixel — see app_main.c's own
 * call-site comment for the exact placement and why.
 *
 * Non-fatal on failure: logs the underlying `esp_err_t` and returns it,
 * but the caller is expected to IGNORE the return value and continue
 * booting regardless (same "log and continue" posture as
 * `ff_power_latch_on`/`ff_bringup_panel`'s other non-fatal steps) — a
 * puck with no working speaker still boots and is fully usable. Every
 * `ff_audio_play` call after a failed init is a no-op (logged once).
 *
 * Idempotent: a second call when already initialized is a no-op that
 * returns ESP_OK immediately (there is exactly one I2S0 TX channel and
 * one render task for the process lifetime).
 */
esp_err_t ff_audio_init(void);

/**
 * ff_audio_play — render `ev`'s pattern (`ff_sound_pattern_for`, core) on
 * the speaker. NON-BLOCKING: hands `ev` to the render task via a 1-deep
 * mailbox and returns immediately — safe to call from the shell's
 * `play_sound` hook, which must never stall waiting on audio I/O.
 *
 * - `ev` outside the vocabulary (`ff_sound_pattern_for` returns NULL) is
 *   silently ignored — reject, not guess, this codebase's usual honest-
 *   data discipline applied to an unrecognised event.
 * - Nothing currently playing: `ev` starts immediately (well, as soon as
 *   the render task is scheduled).
 * - Something IS currently playing: `ff_sound_preempts(ev, playing)`
 *   (core) decides — preempt (true) stops the current pattern at the
 *   next ~10 ms chunk boundary and starts `ev`; otherwise `ev` is
 *   silently DROPPED (see this header's top comment, "Preemption,
 *   concurrency, and the one-speaker rule", for why a queue was
 *   deliberately NOT used here).
 * - `ff_audio_init` never succeeded (or was never called): silent no-op,
 *   logged once.
 */
void ff_audio_play(ff_sound_event_t ev);

/**
 * ff_audio_stop — abort whatever is currently playing (or queued to
 * start next) at the next ~10 ms chunk boundary, and go silent. Not used
 * by any S27 call site today (no spec event asks for "stop the current
 * sound"), but exposed as a general HAL primitive — e.g. a future power-
 * off/reboot path that wants to guarantee silence before cutting power,
 * mirroring `ff_power_off_cb`'s own "leave the hardware in a clean state"
 * posture. Safe to call when nothing is playing (a no-op) or before
 * `ff_audio_init` ever succeeded (also a no-op).
 */
void ff_audio_stop(void);

/**
 * ff_audio_busy — true while the audio HAL has unfinished work: a
 * pattern actively rendering, OR a request just handed to
 * `ff_audio_play` that the render task has not started yet (closes the
 * race between "queued" and "the task got scheduled" — see this header's
 * top comment, "Light sleep"). Feed this into app_main's light-sleep
 * `sleep_inhibit` composition (`ff_idle_tick`'s 4th argument), OR'd with
 * the existing USB-connected inhibit (S26f amendment) — a pattern in
 * flight must never be cut off by `esp_light_sleep_start()`.
 *
 * Always false when `ff_audio_init` never succeeded.
 */
bool ff_audio_busy(void);

#ifdef __cplusplus
}
#endif
