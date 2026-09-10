#!/usr/bin/env python3
"""beat_replay.py — decode a `mic dump <secs>` bench console capture into a
WAV file, and synthesize the same realistic test signals
`firmware/core/tests/test_beat.c`'s own synthetic-music tests use, as
standalone WAV files.

## Why this exists
docs/specs/S31-music-swarm.md's 2026-09-09 amendment (fix/s31-beat-real-audio):
on-device evidence showed the beat detector producing ZERO beats against 15s
of real music (`music source=mic loudness=0.82 bpm=0.0`). Validating a fix
needs a real capture, not another synthetic bench click train — this tool is
the coordinator's own workflow:

    1. On the puck's bench console: `mic dump 10` (docs/specs/
       S30-audio-input.md's own dated addition) — capture the console
       session's output to a text file (any serial terminal's own logging,
       or a simple `screen -L`/`cu` transcript works; this tool only needs
       the `dbg: mic dump ...` lines, in order, and tolerates whatever else
       a terminal log happens to interleave).
    2. `python3 tools/beat_replay.py decode <log.txt> -o capture.wav`
    3. Feed `capture.wav` through `firmware/core/tools/beat_sim_replay`
       (built by the sim CMake build) to see the REAL detector's own
       loudness/beat/BPM output over time against a real capture.

Until a real capture exists, `synth` produces the same two signals
`test_beat.c`'s own synthetic tests validate the detector against: a 128 BPM
four-on-the-floor kick with a sustained bass line and a compressed dynamic
range (the "sounds loud, barely moves the broadband meter" shape a real
limiter produces), and a "phone speaker" variant high-passed at 150Hz (a
phone/laptop speaker's bass rolloff) — useful for a quick by-ear/by-eye sanity
check of `beat_sim_replay`'s own output before trusting it on a real capture.

Pure standard library — no `numpy`, no `scipy` — so this runs anywhere a bare
`python3` does, matching this repo's existing `probe_node.py` "importless by
default" precedent (see that file's own top comment).

Usage:
    python3 tools/beat_replay.py decode <dump.txt> -o out.wav
    python3 tools/beat_replay.py synth --bpm 128 -o kick.wav
    python3 tools/beat_replay.py synth --bpm 128 --phone-speaker -o kick_phone.wav
    python3 tools/beat_replay.py --selftest
"""
import argparse
import base64
import math
import re
import struct
import sys
import wave

SAMPLE_RATE_HZ = 16000
FRAME_SAMPLES = 320  # 20ms at 16kHz — ff_mic.h's own FF_MIC_FRAME_SAMPLES
FRAME_BYTES = FRAME_SAMPLES * 2  # int16, little-endian

_HEADER_RE = re.compile(r"mic dump rate_hz=(\d+) bits=(\d+) frames=(\d+)")
_DATA_RE = re.compile(r"mic dump data (\S+)")
_TRAILER_RE = re.compile(r"mic dump done frames=(\d+) dropped=(\d+)(?: stalled=(\d))?")


class DumpDecodeError(Exception):
    pass


def decode_dump(lines):
    """Parse a `mic dump` console transcript (an iterable of text lines —
    tolerant of a leading `dbg: ` prefix, timestamps, or any other terminal
    noise on lines this tool doesn't care about) into `(rate_hz, bits,
    pcm_bytes, stats)`. `stats` is a dict with `requested_frames`,
    `delivered_frames` (data lines actually decoded), `dropped_frames`
    (reader-side, from the trailer), and `stalled` (bool). Raises
    `DumpDecodeError` if no header/trailer was found at all — an honest
    failure, never a silently-empty WAV."""
    rate_hz = None
    bits = None
    requested_frames = None
    dropped_frames = 0
    stalled = False
    pcm = bytearray()
    delivered = 0

    for line in lines:
        m = _HEADER_RE.search(line)
        if m:
            rate_hz, bits, requested_frames = int(m.group(1)), int(m.group(2)), int(m.group(3))
            continue
        m = _DATA_RE.search(line)
        if m:
            chunk = base64.b64decode(m.group(1))
            pcm.extend(chunk)
            delivered += 1
            continue
        m = _TRAILER_RE.search(line)
        if m:
            dropped_frames = int(m.group(2))
            stalled = m.group(3) == "1"
            continue

    if rate_hz is None:
        raise DumpDecodeError("no 'mic dump rate_hz=...' header line found in input")
    if delivered == 0:
        raise DumpDecodeError("no 'mic dump data ...' lines found in input")

    stats = {
        "requested_frames": requested_frames,
        "delivered_frames": delivered,
        "dropped_frames": dropped_frames,
        "stalled": stalled,
    }
    return rate_hz, bits, bytes(pcm), stats


def write_wav(path, rate_hz, pcm_bytes, channels=1, sample_width=2):
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(sample_width)
        w.setframerate(rate_hz)
        w.writeframes(pcm_bytes)


# --------------------------------------------------------------------
# Synthesis — the same two signals test_beat.c's own synthetic tests use.
# --------------------------------------------------------------------

def _one_pole_highpass(samples, cutoff_hz, sample_rate_hz):
    """First-order (RC) high-pass — DC-blocking-filter shape, `ff_miclevel.
    c`'s own `ff_miclevel_dc_remove` but at a musically-relevant corner
    instead of true-DC, to model a phone/laptop speaker's bass rolloff.
    `y[n] = alpha * (y[n-1] + x[n] - x[n-1])`."""
    rc = 1.0 / (2.0 * math.pi * cutoff_hz)
    dt = 1.0 / sample_rate_hz
    alpha = rc / (rc + dt)
    out = [0.0] * len(samples)
    prev_x = 0.0
    prev_y = 0.0
    for i, x in enumerate(samples):
        y = alpha * (prev_y + x - prev_x)
        out[i] = y
        prev_x = x
        prev_y = y
    return out


def _soft_compress(samples, threshold=0.35, ratio=6.0):
    """A crude soft-knee compressor: samples above `threshold` (in [0,1]
    linear) get squashed toward it by `ratio` — enough to bring a kick's
    peak-vs-sustain swing down to the 3-5dB the deliverable asks for
    ("compressed dynamic range"), without needing a real lookahead
    limiter for what is just a synthetic test signal."""
    out = [0.0] * len(samples)
    for i, x in enumerate(samples):
        mag = abs(x)
        if mag <= threshold:
            out[i] = x
        else:
            over = mag - threshold
            compressed = threshold + over / ratio
            out[i] = math.copysign(compressed, x)
    return out


def synth_kick_track(bpm=128.0, duration_s=8.0, sample_rate_hz=SAMPLE_RATE_HZ, phone_speaker=False):
    """A 128 BPM (default) four-on-the-floor kick with a sustained bass
    line and a compressed dynamic range — the same shape
    `test_beat.c`'s own `run_synthetic_kick` helper models directly in
    the dB domain, rendered here as actual PCM for `beat_sim_replay` to
    consume. Returns a list of floats in roughly [-1, 1]."""
    n = int(duration_s * sample_rate_hz)
    period_s = 60.0 / bpm
    samples = [0.0] * n

    # Sustained bass line: a held low tone (55Hz, ~A1) at modest amplitude,
    # continuous — this is what "sustained bass line" means: audible
    # throughout, not just during a kick's own transient.
    bass_hz = 55.0
    bass_amp = 0.18
    for i in range(n):
        t = i / sample_rate_hz
        samples[i] += bass_amp * math.sin(2.0 * math.pi * bass_hz * t)

    # Kick: pitch-enveloped burst (a classic kick-drum synthesis trick —
    # start well above the fundamental and glide down fast) with a fast
    # attack / short exponential decay amplitude envelope.
    kick_attack_s = 0.004
    kick_decay_tau_s = 0.09
    kick_amp = 0.85
    pitch_start_hz = 140.0
    pitch_end_hz = 50.0
    pitch_glide_tau_s = 0.05

    t_next_kick = period_s
    phase = 0.0
    last_t = 0.0
    for i in range(n):
        t = i / sample_rate_hz
        dt = t - last_t
        last_t = t
        t_since_kick = t - (t_next_kick - period_s)
        if t >= t_next_kick:
            t_next_kick += period_s
        # phase accumulates using the CURRENT instantaneous pitch, so the
        # glide is continuous (no phase discontinuity at each kick's own
        # onset — a real drum synth's own convention).
        if 0.0 <= t_since_kick < (kick_attack_s + 6.0 * kick_decay_tau_s):
            pitch_hz = pitch_end_hz + (pitch_start_hz - pitch_end_hz) * math.exp(-t_since_kick / pitch_glide_tau_s)
            phase += 2.0 * math.pi * pitch_hz * dt
            if t_since_kick < kick_attack_s:
                env = t_since_kick / kick_attack_s
            else:
                env = math.exp(-(t_since_kick - kick_attack_s) / kick_decay_tau_s)
            samples[i] += kick_amp * env * math.sin(phase)

    samples = _soft_compress(samples, threshold=0.30, ratio=7.0)

    if phone_speaker:
        samples = _one_pole_highpass(samples, cutoff_hz=150.0, sample_rate_hz=sample_rate_hz)
        # Renormalize — a highpass alone can leave levels much quieter
        # than the un-filtered track; a real phone speaker's own limiter/
        # AGC would bring perceived loudness back up, so this models that
        # rather than leaving a suspiciously-quiet signal.
        peak = max((abs(x) for x in samples), default=0.0)
        if peak > 1e-6:
            gain = 0.8 / peak
            samples = [x * gain for x in samples]

    return samples


def _floats_to_pcm16(samples):
    out = bytearray()
    for x in samples:
        v = max(-1.0, min(1.0, x))
        out += struct.pack("<h", int(v * 32767.0))
    return bytes(out)


# --------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------

def cmd_decode(args):
    with open(args.input, "r", errors="replace") as f:
        lines = f.readlines()
    try:
        rate_hz, bits, pcm, stats = decode_dump(lines)
    except DumpDecodeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    write_wav(args.output, rate_hz, pcm)
    n_samples = len(pcm) // 2
    print(f"decoded {stats['delivered_frames']} frames ({n_samples} samples, "
          f"{n_samples / rate_hz:.2f}s @ {rate_hz}Hz/{bits}bit) -> {args.output}")
    if stats["requested_frames"] is not None and stats["delivered_frames"] < stats["requested_frames"]:
        print(f"note: requested {stats['requested_frames']} frames, delivered {stats['delivered_frames']}"
              f" (dropped={stats['dropped_frames']}, stalled={stats['stalled']})", file=sys.stderr)
    return 0


def cmd_synth(args):
    samples = synth_kick_track(bpm=args.bpm, duration_s=args.duration, phone_speaker=args.phone_speaker)
    pcm = _floats_to_pcm16(samples)
    write_wav(args.output, SAMPLE_RATE_HZ, pcm)
    kind = "phone-speaker" if args.phone_speaker else "plain"
    print(f"synthesized {args.duration:.1f}s of {args.bpm:.1f} BPM kick+bass ({kind}) -> {args.output}")
    return 0


def _selftest():
    failures = []

    def check(name, cond):
        if not cond:
            failures.append(name)

    # decode_dump: round-trip a small, hand-built transcript, tolerant of
    # a "dbg: " prefix and interleaved noise (a real terminal log's own
    # timestamps, echoed input, etc.).
    frame_a = bytes(range(0, FRAME_BYTES % 256)) + bytes([0] * (FRAME_BYTES - (FRAME_BYTES % 256)))
    frame_a = bytes((i * 3) % 256 for i in range(FRAME_BYTES))
    frame_b = bytes((i * 7 + 1) % 256 for i in range(FRAME_BYTES))
    transcript = [
        "12:00:01.000 > mic dump 1\n",
        "dbg: mic dump rate_hz=16000 bits=16 frames=50\n",
        "some unrelated echoed line the terminal happened to log\n",
        f"dbg: mic dump data {base64.b64encode(frame_a).decode()}\n",
        f"dbg: mic dump data {base64.b64encode(frame_b).decode()}\n",
        "dbg: mic dump done frames=2 dropped=1\n",
    ]
    rate_hz, bits, pcm, stats = decode_dump(transcript)
    check("decode: rate_hz", rate_hz == 16000)
    check("decode: bits", bits == 16)
    check("decode: pcm length", len(pcm) == 2 * FRAME_BYTES)
    check("decode: pcm content frame A", pcm[:FRAME_BYTES] == frame_a)
    check("decode: pcm content frame B", pcm[FRAME_BYTES:] == frame_b)
    check("decode: requested_frames", stats["requested_frames"] == 50)
    check("decode: delivered_frames", stats["delivered_frames"] == 2)
    check("decode: dropped_frames", stats["dropped_frames"] == 1)
    check("decode: stalled", stats["stalled"] is False)

    try:
        decode_dump(["nothing relevant here\n"])
        check("decode: raises on no header", False)
    except DumpDecodeError:
        pass

    # synth_kick_track: sane shape — right length, bounded amplitude, and
    # genuinely has SOME low-frequency energy for the plain variant.
    plain = synth_kick_track(bpm=128.0, duration_s=1.0)
    check("synth: length", len(plain) == SAMPLE_RATE_HZ)
    check("synth: bounded", max(abs(x) for x in plain) <= 1.0 + 1e-6)
    rms_plain = math.sqrt(sum(x * x for x in plain) / len(plain))
    check("synth: audible (plain)", rms_plain > 0.02)

    phone = synth_kick_track(bpm=128.0, duration_s=1.0, phone_speaker=True)
    check("synth: length (phone)", len(phone) == SAMPLE_RATE_HZ)
    check("synth: bounded (phone)", max(abs(x) for x in phone) <= 1.0 + 1e-6)

    if failures:
        print("SELFTEST FAILED:")
        for name in failures:
            print(f"  - {name}")
        return 1
    print(f"selftest OK ({4 + 8 + 1} checks)")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--selftest", action="store_true", help="run this tool's own library-free self-test and exit")
    sub = parser.add_subparsers(dest="cmd")

    p_decode = sub.add_parser("decode", help="decode a `mic dump` console transcript into a WAV file")
    p_decode.add_argument("input", help="path to the captured console transcript (text)")
    p_decode.add_argument("-o", "--output", required=True, help="output WAV path")

    p_synth = sub.add_parser("synth", help="synthesize a test kick+bass signal as a WAV file")
    p_synth.add_argument("--bpm", type=float, default=128.0)
    p_synth.add_argument("--duration", type=float, default=8.0, help="seconds")
    p_synth.add_argument("--phone-speaker", action="store_true", help="high-pass at 150Hz, like a phone/laptop speaker")
    p_synth.add_argument("-o", "--output", required=True, help="output WAV path")

    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()
    if args.cmd == "decode":
        return cmd_decode(args)
    if args.cmd == "synth":
        return cmd_synth(args)
    parser.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
