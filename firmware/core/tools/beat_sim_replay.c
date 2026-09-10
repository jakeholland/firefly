/**
 * beat_sim_replay.c — 2026-09-09 amendment (fix/s31-beat-real-audio):
 * the sim test harness deliverable's own "feeds a WAV/raw capture
 * through ff_miclevel + ff_beat at the real 50Hz cadence and prints
 * loudness, detected beats and the BPM estimate over time" tool.
 *
 * Links against the REAL `ff_miclevel`/`ff_bandenergy`/`ff_beat`
 * implementations (this same binary IS `firmware/core`'s own code,
 * not a reimplementation) — so this is the coordinator's own way to
 * validate a real `mic dump` capture (decoded to WAV by `tools/
 * beat_replay.py`, this repo's top-level tools/) against the actual
 * detector without flashing new firmware for every experiment, and to
 * sanity-check `beat_replay.py`'s own synthesized test signals by ear/
 * eye before trusting them.
 *
 * Reads a 16-bit mono PCM WAV file (16kHz — this module's own
 * `ff_bandenergy.h` dependency is fixed at that rate, matching the
 * real mic HAL's one supported rate, docs/specs/S30-audio-input.md's
 * own "Format" section) via `ff_wav.h` (2026-09-09 amendment,
 * fix/s31-beat-real-captures — the parser used to live inline here;
 * it moved to `firmware/core/include/ff_wav.h` so the SAME parsing
 * code also serves `test_beat_captures.c`'s real-capture regression
 * tests, see that header's own top comment), feeds it through the
 * EXACT SAME per-frame pipeline `ff_mic.c`'s reader task runs on real
 * hardware (DC-remove -> RMS/peak/envelope -> band energy ->
 * `ff_beat_update`) at the real 20ms/50Hz frame cadence, and prints:
 *   - a status line every ~100ms (`t=... loudness=... low_dbfs=...
 *     mid_dbfs=... bpm=...`);
 *   - a `BEAT` line the instant `beat_count` increments (never delayed
 *     to the next status line — a bench engineer wants beats visible
 *     to their own real timing, not sampled at this tool's print
 *     cadence) — tagged `(predicted)` when this beat came from the
 *     beat-tracker's own period prediction rather than a confirmed
 *     band onset (`ff_beat.h`'s "Beat-tracking" section, 2026-09-09
 *     amendment fix/s31-beat-real-captures);
 *   - a one-line SUMMARY at the end (2026-09-09 amendment,
 *     fix/s31-beat-real-captures — the deliverable's own "one-line
 *     summary ... so the coordinator can re-run it on future
 *     captures"): beat count, mean inter-beat interval, final BPM
 *     estimate, and — only when a ground-truth onsets file is given as
 *     the optional 2nd argument (one flux-peak timestamp in seconds
 *     per line, e.g. `firmware/core/tests/fixtures/audio/`, _onsets.txt)
 *     — the fraction of those onsets this run actually matched (a
 *     detected beat within +-20% of the run's own mean inter-beat
 *     interval of the onset timestamp — the same acceptance-window
 *     convention `ff_beat.c`'s own beat-tracker uses).
 *
 * Sim-only CLI tool (not part of ctest — no pass/fail assertion of its
 * own; it prints for a human to read, mirroring `compare_png`'s own
 * "tool, not a test" role in `firmware/tools/`). Deliberately minimal
 * WAV parsing: 16-bit PCM mono only, canonical RIFF/WAVE chunk layout
 * — this tool only ever needs to read what `tools/beat_replay.py`
 * itself writes (Python's own `wave` module, the plainest possible
 * WAV), never an arbitrary third-party file.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff_bandenergy.h"
#include "ff_beat.h"
#include "ff_miclevel.h"
#include "ff_wav.h"

#define FRAME_SAMPLES FF_MICLEVEL_FRAME_SAMPLES /* 320 — 20ms at 16kHz */
#define STATUS_PERIOD_MS 100u
#define MAX_ONSETS 4096u
#define MAX_BEATS 4096u

static uint8_t *read_whole_file(char const *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open '%s'\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long const len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)len);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t const n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (n != (size_t)len) {
        free(buf);
        return NULL;
    }
    *out_len = (size_t)len;
    return buf;
}

/* Loads a ground-truth onsets file (one timestamp in seconds per line,
 * blank lines and '#'-prefixed comments ignored) — see this file's own
 * top comment. Returns the count loaded (0 on a missing/empty file,
 * never an error — the summary line just reports "n/a" for the match
 * fraction then). */
static size_t load_onsets(char const *path, float *out, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) return 0u;
    size_t n = 0u;
    char line[128];
    while (n < cap && fgets(line, sizeof(line), f) != NULL) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '#') continue;
        out[n++] = strtof(p, NULL);
    }
    fclose(f);
    return n;
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: %s <capture.wav> [onsets.txt]\n", argv[0]);
        fprintf(stderr, "  16-bit mono PCM WAV @ %uHz — see tools/beat_replay.py (decode/synth)\n",
                (unsigned)FF_BANDENERGY_SAMPLE_RATE_HZ);
        fprintf(stderr, "  onsets.txt: optional ground-truth onset timestamps (seconds, one per\n");
        fprintf(stderr, "  line) — the SUMMARY line reports what fraction this run matched.\n");
        return 2;
    }

    size_t file_len = 0u;
    uint8_t *file_bytes = read_whole_file(argv[1], &file_len);
    if (file_bytes == NULL) {
        fprintf(stderr, "failed to read '%s'\n", argv[1]);
        return 1;
    }

    ff_wav_t wav;
    if (ff_wav_parse(file_bytes, file_len, &wav) != 0) {
        fprintf(stderr, "'%s': not a valid RIFF/WAVE file\n", argv[1]);
        free(file_bytes);
        return 1;
    }
    if (!ff_wav_is_16bit_mono_at(&wav, FF_BANDENERGY_SAMPLE_RATE_HZ)) {
        fprintf(stderr, "'%s': need 16-bit mono PCM @ %uHz (got channels=%u bits=%u rate=%u)\n", argv[1],
                (unsigned)FF_BANDENERGY_SAMPLE_RATE_HZ, (unsigned)wav.channels, (unsigned)wav.bits_per_sample,
                (unsigned)wav.sample_rate_hz);
        free(file_bytes);
        return 1;
    }

    static float onsets_s[MAX_ONSETS];
    size_t const n_onsets = (argc == 3) ? load_onsets(argv[2], onsets_s, MAX_ONSETS) : 0u;

    size_t const n_frames = wav.n_samples / FRAME_SAMPLES;
    printf("beat_sim_replay: %s — %zu samples (%.2fs @ %uHz), %zu frames\n", argv[1], wav.n_samples,
           (double)wav.n_samples / (double)wav.sample_rate_hz, (unsigned)wav.sample_rate_hz, n_frames);
    if (argc == 3) {
        printf("  ground truth: %zu onset(s) from '%s'\n", n_onsets, argv[2]);
    }

    ff_miclevel_dc_state_t dc;
    ff_miclevel_envelope_t env;
    ff_bandenergy_t band;
    ff_beat_t beat;
    ff_miclevel_dc_reset(&dc);
    ff_miclevel_envelope_reset(&env);
    ff_bandenergy_reset(&band);
    ff_beat_reset(&beat);

    float float_buf[FRAME_SAMPLES];
    uint32_t now_ms = 0u;
    uint32_t last_status_ms = 0u;
    uint32_t last_beat_count = 0u;

    static float beat_times_s[MAX_BEATS];
    size_t n_beats = 0u;

    for (size_t f = 0; f < n_frames; f++) {
        int16_t const *raw = wav.samples + f * FRAME_SAMPLES;
        for (size_t i = 0; i < FRAME_SAMPLES; i++) {
            float_buf[i] = ff_miclevel_dc_remove(&dc, (float)raw[i]);
        }

        ff_miclevel_frame_t frame;
        ff_miclevel_frame_compute(float_buf, FRAME_SAMPLES, &frame);
        ff_miclevel_envelope_update(&env, frame.rms_dbfs, 20u);

        ff_bandenergy_frame_t bandf;
        ff_bandenergy_frame_compute(&band, float_buf, FRAME_SAMPLES, &bandf);

        ff_beat_sample_t sample = {0};
        sample.source = FF_BEAT_SOURCE_MIC;
        sample.rms_dbfs = frame.rms_dbfs;
        sample.env_dbfs = env.value_dbfs;
        sample.low_band_dbfs = bandf.low_dbfs;
        sample.mid_band_dbfs = bandf.mid_dbfs;

        now_ms += 20u;
        ff_beat_update(&beat, &sample, 20u, now_ms);

        if (beat.beat_count != last_beat_count) {
            last_beat_count = beat.beat_count;
            if (n_beats < MAX_BEATS) beat_times_s[n_beats++] = (float)now_ms / 1000.0f;
            printf("  BEAT   t=%7.3fs  #%-4u  bpm=%.1f%s\n", (double)now_ms / 1000.0, (unsigned)beat.beat_count,
                   (double)beat.bpm_estimate, beat.last_beat_predicted ? "  (predicted)" : "");
        }

        if (now_ms - last_status_ms >= STATUS_PERIOD_MS) {
            last_status_ms = now_ms;
            printf("t=%7.3fs  loudness=%.2f  low_dbfs=%6.1f  mid_dbfs=%6.1f  bpm=%.1f\n", (double)now_ms / 1000.0,
                   (double)beat.loudness, (double)bandf.low_dbfs, (double)bandf.mid_dbfs, (double)beat.bpm_estimate);
        }
    }

    float mean_interval_ms = 0.0f;
    if (n_beats >= 2u) {
        mean_interval_ms = (beat_times_s[n_beats - 1u] - beat_times_s[0]) * 1000.0f / (float)(n_beats - 1u);
    }

    printf("done: %zu frames, %u beats, final bpm_estimate=%.1f\n", n_frames, (unsigned)beat.beat_count,
           (double)beat.bpm_estimate);

    /* SUMMARY line — 2026-09-09 amendment (fix/s31-beat-real-captures),
     * the deliverable's own "one-line summary ... so the coordinator
     * can re-run it on future captures". Match window: a FIXED 150ms —
     * roughly 20% of the ~500-750ms beat period these captures (and
     * ordinary 80-150 BPM dance tempos generally) actually show. A
     * window derived from the onsets file's own local gaps was tried
     * and rejected: real, syncopated low-band onsets are NOT evenly
     * spaced (see docs/specs/S31-music-swarm.md's dated amendment), so
     * a median-of-local-gaps window swings with exactly the same
     * irregularity the match check is trying to look past. A fixed,
     * documented tolerance is simpler and does not get tighter or
     * looser depending on which onsets happen to sit next to each
     * other in the ground-truth file. */
    if (n_onsets > 0u) {
        float const window_s = 0.15f;
        size_t matched = 0u;
        for (size_t i = 0; i < n_onsets; i++) {
            for (size_t j = 0; j < n_beats; j++) {
                float const diff = beat_times_s[j] - onsets_s[i];
                float const adiff = (diff < 0.0f) ? -diff : diff;
                if (adiff <= window_s) {
                    matched++;
                    break;
                }
            }
        }
        printf("SUMMARY beats=%zu mean_interval_ms=%.1f bpm=%.1f matched=%zu/%zu (%.0f%%)\n", n_beats,
               (double)mean_interval_ms, (double)beat.bpm_estimate, matched, n_onsets,
               (double)matched * 100.0 / (double)n_onsets);
    } else {
        printf("SUMMARY beats=%zu mean_interval_ms=%.1f bpm=%.1f matched=n/a (no onsets file given)\n", n_beats,
               (double)mean_interval_ms, (double)beat.bpm_estimate);
    }

    free(file_bytes);
    return 0;
}
