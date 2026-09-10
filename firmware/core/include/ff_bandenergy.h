/**
 * ff_bandenergy.h — S31 amendment (2026-09-09, fix/s31-beat-real-audio):
 * pure, host-testable BAND-LIMITED ENERGY math, split out of `ff_beat.c`
 * so the filter design (and its own dedicated test coverage) is not
 * buried inside the detector state machine. Zero I/O, zero hardware
 * knowledge — the same `firmware/core/` placement rule `ff_miclevel.h`
 * and `ff_beat.h` already follow, applied to "how much energy is in
 * THIS frequency band, this 20ms frame".
 *
 * Spec: docs/specs/S31-music-swarm.md's 2026-09-09 amendment.
 *
 * ## Why this exists — the on-device evidence
 * Jake's puck (main a853bb5 + coordinator hotfix), real music playing
 * from a speaker: `music source=mic loudness=0.82 bpm=0.0` for the
 * whole 15s set. The BROADBAND envelope (`ff_miclevel.h`'s RMS dBFS)
 * that `ff_beat.c` used to run its onset detector against barely moves
 * on a kick drum in a real, dynamic-range-COMPRESSED mix — the whole
 * track already sits near the limiter's ceiling, so a kick's actual
 * energy punch is invisible in the one broadband number. It is NOT
 * invisible in a narrow LOW band (roughly 60-200Hz, where a kick/bass
 * note's fundamental actually lives): a compressor/limiter working on
 * the full mix still lets a kick's low-frequency content spike sharply
 * relative to everything else briefly sharing that same band. This
 * module computes exactly that — a LOW (60-200Hz) and a MID
 * (200-2000Hz, where a snare/clap/other percussive-or-melodic transient
 * shows up instead) band energy per 20ms frame — so `ff_beat.c`'s onset
 * detector (its own 2026-09-09 amendment) can compare a frame's
 * ENERGY-RISE in each band against that band's own recent history,
 * rather than a single already-compressed broadband number.
 *
 * ## Filter design — two cascaded one-pole lowpasses, differenced
 * A "real" bandpass (biquad, resonant) is more surgical but also has
 * failure modes (instability at the wrong Q, ringing) this bench-and-
 * ship codebase has no bandwidth to chase down on hardware. This module
 * instead builds each band as the DIFFERENCE of two one-pole (RC)
 * lowpass filters at the band's low/high edge — `LP(f_hi, x) -
 * LP(f_lo, x)` passes roughly the energy between `f_lo` and `f_hi` and
 * is unconditionally stable (real poles only, no complex-conjugate pair
 * to mis-tune) — the same "simplest thing that is honestly a bandpass"
 * tradeoff `ff_miclevel.h`'s own DC-blocking one-pole filter already
 * makes for its own, simpler job. THREE one-pole lowpasses run per
 * sample (60Hz, 200Hz, 2000Hz corners); LOW = LP200-LP60, MID =
 * LP2000-LP200 — one shared LP200 stage feeds both bands.
 *
 * `FF_BANDENERGY_SAMPLE_RATE_HZ` is fixed at `ff_miclevel.h`'s own
 * `FF_MICLEVEL_SAMPLE_RATE_HZ` (16kHz) — this module has no reason to
 * support any other rate; the mic HAL that feeds it has exactly one.
 *
 * ## Output convention
 * `ff_bandenergy_frame_compute` reports each band's RMS level for the
 * frame in the SAME dBFS convention `ff_miclevel_to_dbfs` already uses
 * (floored at `FF_MICLEVEL_FLOOR_DBFS`, never NaN/-inf) — one shared
 * unit across every level this codebase ever prints, per this repo's
 * existing "one convention, everywhere" discipline (`ff_beat.h`'s own
 * top comment makes the identical point about MIC vs IMU).
 */
#ifndef FF_BANDENERGY_H
#define FF_BANDENERGY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** This module's one supported sample rate — see this header's top
 *  comment. Mirrors `FF_MICLEVEL_SAMPLE_RATE_HZ` (ff_miclevel.h)
 *  verbatim; not `#include`d from there to keep this header dependency-
 *  light (`ff_theme.h`'s own precedent, cited by `ff_beat.h` for the
 *  identical tradeoff on `FF_BEAT_FLOOR_DBFS`) — `ff_bandenergy.c`
 *  itself DOES include ff_miclevel.h, for `ff_miclevel_to_dbfs`. */
#define FF_BANDENERGY_SAMPLE_RATE_HZ 16000u

/** Band edges (Hz) — see this header's top comment, "Why this exists",
 *  for the reasoning behind these specific numbers (a kick/bass
 *  fundamental vs. a snare/clap/other percussive-or-melodic transient).
 *  Flagged per AGENTS.md's "note the interpretation" rule: no spec
 *  pinned these before this amendment. */
#define FF_BANDENERGY_LOW_LO_HZ  60.0f
#define FF_BANDENERGY_LOW_HI_HZ  200.0f
#define FF_BANDENERGY_MID_LO_HZ  200.0f
#define FF_BANDENERGY_MID_HI_HZ  2000.0f

/**
 * ff_bandenergy_t / ff_bandenergy_reset — the three one-pole lowpass
 * filters' running state (`y[n-1]` for each of the 60/200/2000Hz
 * corners). Call `ff_bandenergy_reset` once when a capture session
 * starts (mirrors `ff_miclevel_dc_state_t`'s own reset-on-restart
 * discipline) so a previous session's filter state never bleeds into a
 * fresh one.
 */
typedef struct {
    float lp60;
    float lp200;
    float lp2000;
    bool  primed; /* false until the first sample seeds all three stages at that sample's own value, so a fresh
                     reset's filters don't ring from an assumed 0 starting point against a nonzero first sample */
} ff_bandenergy_t;

void ff_bandenergy_reset(ff_bandenergy_t *st);

/** One frame's worth of band energy, dBFS (see this header's top
 *  comment, "Output convention"). */
typedef struct {
    float low_dbfs;
    float mid_dbfs;
} ff_bandenergy_frame_t;

/**
 * ff_bandenergy_frame_compute — filter `n` already-DC-removed linear
 * samples (the SAME samples `ff_miclevel_frame_compute` computes its
 * own broadband RMS/peak from — this module runs on the identical
 * input, just a different filter) through the three-stage lowpass
 * cascade, updating `st` in place, and report the LOW/MID band RMS
 * dBFS for this frame in `*out`. `st == NULL`, `out == NULL`, `samples
 * == NULL`, or `n == 0` is a safe no-op that leaves `*out` at
 * `FF_MICLEVEL_FLOOR_DBFS` for both fields (an empty frame has no
 * signal to report — mirrors `ff_miclevel_frame_compute`'s own `n==0`
 * contract).
 */
void ff_bandenergy_frame_compute(ff_bandenergy_t *st, float const *samples, size_t n, ff_bandenergy_frame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* FF_BANDENERGY_H */
