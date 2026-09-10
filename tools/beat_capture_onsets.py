#!/usr/bin/env python3
"""beat_capture_onsets.py — pick ground-truth LOW-band onset timestamps
out of a real `mic dump` capture (WAV, 16-bit mono @ 16kHz), for use as
`firmware/core/tests/fixtures/audio/*_onsets.txt` — the regression
fixtures `test_beat_captures.c` checks the tuned `ff_beat` detector's
own output against (see that test file's own top comment).

## Why this exists / the method
docs/specs/S31-music-swarm.md's 2026-09-09 amendment (fix/s31-beat-
real-captures): tuning the real-onset-flux detector needs an honest,
independent answer to "where are the real onsets in this capture's LOW
band" that does NOT come from the detector being tuned. This script
runs the SAME one-pole-cascade LOW-band filter `ff_bandenergy.c`
actually implements (three cascaded one-pole lowpasses at 60/200/2000Hz,
LOW = LP200 - LP60 — see that module's own top comment for why a
cascade, not a resonant bandpass) over the capture, computes the same
half-wave-rectified frame-to-frame flux `ff_beat.c`'s onset detector
uses, and picks LOCAL MAXIMA of that flux that clear an adaptive
median + 2*MAD bar (or a fixed 0.5dB floor, whichever is higher) and
sit at least 350ms apart (a "these are genuinely separate onsets, not
one attack's shoulder" spacing floor). This is deliberately a MORE
sensitive/permissive picker than the shipped detector's own threshold
(`FF_BEAT_MUSIC_FLUX_MAD_K`/`FF_BEAT_MUSIC_FLUX_MIN_DB`,
`firmware/core/include/ff_beat.h`) — it exists to answer "what is
honestly THERE in the low band", not to reproduce what the shipped
detector currently fires on; `test_beat_captures.c` then measures what
FRACTION of these the real, shipped detector catches.

Pure standard library — no `numpy`, no `scipy` — matching `tools/
beat_replay.py`'s own "importless by default" precedent.

Usage:
    python3 tools/beat_capture_onsets.py <capture.wav> [-o onsets.txt]

With no `-o`, prints the timestamps (and the chosen threshold) to
stdout instead of writing a file.
"""
import argparse
import math
import sys
import wave
import array

SAMPLE_RATE_HZ = 16000
FRAME_SAMPLES = 320  # 20ms @ 16kHz — matches FF_MICLEVEL_FRAME_SAMPLES
LOW_LO_HZ = 60.0
LOW_HI_HZ = 200.0
MIN_SEP_S = 0.35
MAD_K = 2.0
MIN_DB_FLOOR = 0.5
FULL_SCALE = 32768.0
FLOOR_DBFS = -120.0


def one_pole_alpha(fc_hz, sample_rate_hz):
    """Matches ff_bandenergy.c's own one-pole (RC) lowpass corner ->
    per-sample alpha conversion: alpha = dt / (RC + dt), RC = 1/(2*pi*fc)."""
    rc = 1.0 / (2.0 * math.pi * fc_hz)
    dt = 1.0 / sample_rate_hz
    return dt / (rc + dt)


def to_dbfs(rms):
    if rms <= 0.0:
        return FLOOR_DBFS
    db = 20.0 * math.log10(rms / FULL_SCALE)
    return db if db > FLOOR_DBFS else FLOOR_DBFS


def load_wav(path):
    with wave.open(path, "rb") as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 2:
            raise SystemExit(f"{path}: need 16-bit mono PCM (got channels={w.getnchannels()} "
                              f"width={w.getsampwidth()})")
        if w.getframerate() != SAMPLE_RATE_HZ:
            raise SystemExit(f"{path}: need {SAMPLE_RATE_HZ}Hz (got {w.getframerate()}Hz)")
        raw = w.readframes(w.getnframes())
    samples = array.array("h")
    samples.frombytes(raw)
    return [float(s) for s in samples]


def low_band_flux(samples):
    """Per-frame LOW-band RMS dBFS (ff_bandenergy.c's own filter, see
    this file's top comment), then half-wave-rectified frame-to-frame
    flux — the exact same feature ff_beat.c's onset detector thresholds."""
    mean = sum(samples) / len(samples)
    samples = [s - mean for s in samples]  # crude whole-file DC removal for this offline analysis

    a_lo = one_pole_alpha(LOW_LO_HZ, SAMPLE_RATE_HZ)
    a_hi = one_pole_alpha(LOW_HI_HZ, SAMPLE_RATE_HZ)
    lp_lo = lp_hi = samples[0]

    n_frames = len(samples) // FRAME_SAMPLES
    low_dbfs = []
    for f in range(n_frames):
        sumsq = 0.0
        for i in range(FRAME_SAMPLES):
            x = samples[f * FRAME_SAMPLES + i]
            lp_lo += a_lo * (x - lp_lo)
            lp_hi += a_hi * (x - lp_hi)
            low = lp_hi - lp_lo
            sumsq += low * low
        rms = math.sqrt(sumsq / FRAME_SAMPLES)
        low_dbfs.append(to_dbfs(rms))

    flux = [0.0]
    for i in range(1, len(low_dbfs)):
        d = low_dbfs[i] - low_dbfs[i - 1]
        flux.append(d if d > 0.0 else 0.0)
    return flux


def median(values):
    s = sorted(values)
    return s[len(s) // 2]


def pick_onsets(flux, frame_hz):
    med = median(flux)
    mad = median([abs(v - med) for v in flux])
    threshold = max(med + MAD_K * mad, MIN_DB_FLOOR)

    peaks = []
    for i in range(1, len(flux) - 1):
        if flux[i] >= threshold and flux[i] >= flux[i - 1] and flux[i] >= flux[i + 1]:
            peaks.append((i / frame_hz, flux[i]))

    # Enforce minimum separation, keeping the taller peak in any cluster.
    peaks.sort()
    filtered = []
    for t, v in peaks:
        if filtered and t - filtered[-1][0] < MIN_SEP_S:
            if v > filtered[-1][1]:
                filtered[-1] = (t, v)
        else:
            filtered.append((t, v))
    return [t for t, _ in filtered], threshold


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav", help="16-bit mono PCM WAV @ 16kHz")
    ap.add_argument("-o", "--out", help="write timestamps here instead of stdout")
    args = ap.parse_args()

    samples = load_wav(args.wav)
    flux = low_band_flux(samples)
    frame_hz = SAMPLE_RATE_HZ / FRAME_SAMPLES
    onsets, threshold = pick_onsets(flux, frame_hz)

    header = (
        f"# ground-truth low-band onset timestamps (seconds) for {args.wav}\n"
        f"# one-pole cascade matching ff_bandenergy.c's own LOW band filter,\n"
        f"# local maxima of the half-wave-rectified frame flux above\n"
        f"# median+{MAD_K:.0f}*MAD (threshold={threshold:.3f}dB this file), >= {MIN_SEP_S*1000:.0f}ms apart\n"
        f"# -- see tools/beat_capture_onsets.py's own header, and\n"
        f"# docs/specs/S31-music-swarm.md's dated amendment.\n"
    )
    body = "".join(f"{t:.3f}\n" for t in onsets)

    if args.out:
        with open(args.out, "w") as f:
            f.write(header)
            f.write(body)
        print(f"wrote {len(onsets)} onset(s) to {args.out} (threshold={threshold:.3f}dB)", file=sys.stderr)
    else:
        sys.stdout.write(header)
        sys.stdout.write(body)


if __name__ == "__main__":
    main()
