/**
 * ff_wav.h — 2026-09-09 amendment (fix/s31-beat-real-captures): minimal,
 * pure (zero I/O) canonical RIFF/WAVE parser, factored out of
 * `firmware/core/tools/beat_sim_replay.c` so the SAME parsing code
 * serves both that sim tool and `test_beat_captures.c`'s real-capture
 * regression tests (`firmware/core/tests/fixtures/audio/`, *.wav)
 * instead
 * of two hand-copied implementations quietly drifting apart — the exact
 * "one function, not two hand-copied blocks" rationale
 * `ff_beat_band_tracker_t`'s own doc comment (`ff_beat.h`) already gives
 * for a near-identical duplication risk.
 *
 * Deliberately minimal, mirroring `beat_sim_replay.c`'s own original
 * top comment: 16-bit PCM mono only, canonical chunk layout — this only
 * ever needs to read what `tools/beat_replay.py` (Python's own `wave`
 * module) or a real `mic dump` capture produces, never an arbitrary
 * third-party WAV file. `ff_wav_parse` takes an in-memory byte buffer
 * (owned by the caller, who does the actual `fopen`/`fread` — see
 * `ff_wav_t`'s own doc comment) so this module itself does no I/O,
 * matching every other `firmware/core/` module's placement rule.
 */
#ifndef FF_WAV_H
#define FF_WAV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A parsed WAV file's format + a pointer to its PCM sample data.
 *  `samples` points INTO the caller's own `file_bytes` buffer (passed to
 *  `ff_wav_parse`) — it must outlive `samples`; this module allocates
 *  nothing and copies nothing. */
typedef struct {
    uint32_t sample_rate_hz;
    uint16_t bits_per_sample;
    uint16_t channels;
    int16_t const *samples; /* points into the caller's own file buffer */
    size_t n_samples;
} ff_wav_t;

/** ff_wav_parse — parse a canonical RIFF/WAVE byte buffer (`file_bytes`,
 *  `file_len` bytes) into `*out`. Returns 0 on success; a negative value
 *  on any parse failure (not a RIFF/WAVE file, missing/truncated 'fmt '
 *  or 'data' chunk, non-PCM format) — `out` is zeroed first, so a
 *  failed parse never leaves stale/partial data in it. `file_bytes`,
 *  `out == NULL` is a safe no-op returning -1. This function does NOT
 *  validate channel count/bit depth/sample rate against any particular
 *  target rate — see `ff_wav_is_16bit_mono_at` for that (a separate,
 *  composable check, since `beat_sim_replay.c` and a future stereo/
 *  higher-rate consumer might want different constraints). */
int ff_wav_parse(uint8_t const *file_bytes, size_t file_len, ff_wav_t *out);

/** ff_wav_is_16bit_mono_at — the one format this repo's consumers
 *  (`beat_sim_replay.c`, `test_beat_captures.c`) actually need: 16-bit
 *  mono PCM at exactly `expected_sample_rate_hz` (matching
 *  `ff_bandenergy.h`'s own fixed-rate contract). `wav == NULL` reads as
 *  false. */
bool ff_wav_is_16bit_mono_at(ff_wav_t const *wav, uint32_t expected_sample_rate_hz);

#ifdef __cplusplus
}
#endif

#endif /* FF_WAV_H */
