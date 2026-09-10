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
 * own "Format" section), feeds it through the EXACT SAME per-frame
 * pipeline `ff_mic.c`'s reader task runs on real hardware (DC-remove
 * -> RMS/peak/envelope -> band energy -> `ff_beat_update`) at the
 * real 20ms/50Hz frame cadence, and prints:
 *   - a status line every ~100ms (`t=... loudness=... low_dbfs=...
 *     mid_dbfs=... bpm=...`);
 *   - a `BEAT` line the instant `beat_count` increments (never delayed
 *     to the next status line — a bench engineer wants beats visible
 *     to their own real timing, not sampled at this tool's print
 *     cadence).
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

#define FRAME_SAMPLES FF_MICLEVEL_FRAME_SAMPLES /* 320 — 20ms at 16kHz */
#define STATUS_PERIOD_MS 100u

typedef struct {
    uint32_t sample_rate_hz;
    uint16_t bits_per_sample;
    uint16_t channels;
    int16_t const *samples; /* points into the caller's own file buffer */
    size_t n_samples;
} wav_t;

/* Minimal canonical RIFF/WAVE parser — see this file's own top comment
 * for why this deliberately does not handle every real-world WAV
 * variant (extra chunks, non-PCM formats, etc.). Returns 0 on success;
 * a negative value (with a message already printed to stderr) on any
 * parse failure. `file_bytes` is owned by the caller and must outlive
 * `out->samples`. */
static int wav_parse(uint8_t const *file_bytes, size_t file_len, wav_t *out)
{
    memset(out, 0, sizeof(*out));
    if (file_len < 44u || memcmp(file_bytes, "RIFF", 4) != 0 || memcmp(file_bytes + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "not a RIFF/WAVE file\n");
        return -1;
    }

    size_t pos = 12u;
    bool have_fmt = false;
    while (pos + 8u <= file_len) {
        char id[5] = {0};
        memcpy(id, file_bytes + pos, 4);
        uint32_t chunk_size;
        memcpy(&chunk_size, file_bytes + pos + 4u, 4);
        size_t const body = pos + 8u;
        if (body + chunk_size > file_len) {
            fprintf(stderr, "truncated '%s' chunk\n", id);
            return -1;
        }

        if (memcmp(id, "fmt ", 4) == 0) {
            if (chunk_size < 16u) {
                fprintf(stderr, "fmt chunk too small\n");
                return -1;
            }
            uint16_t audio_format, channels, bits_per_sample;
            uint32_t sample_rate;
            memcpy(&audio_format, file_bytes + body + 0u, 2);
            memcpy(&channels, file_bytes + body + 2u, 2);
            memcpy(&sample_rate, file_bytes + body + 4u, 4);
            memcpy(&bits_per_sample, file_bytes + body + 14u, 2);
            if (audio_format != 1u /* PCM */) {
                fprintf(stderr, "only PCM WAV is supported (audio_format=%u)\n", (unsigned)audio_format);
                return -1;
            }
            out->sample_rate_hz = sample_rate;
            out->channels = channels;
            out->bits_per_sample = bits_per_sample;
            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            if (!have_fmt) {
                fprintf(stderr, "'data' chunk arrived before 'fmt '\n");
                return -1;
            }
            out->samples = (int16_t const *)(file_bytes + body);
            out->n_samples = chunk_size / sizeof(int16_t);
        }

        pos = body + chunk_size + (chunk_size & 1u); /* chunks are word-aligned */
    }

    if (!have_fmt || out->samples == NULL) {
        fprintf(stderr, "missing 'fmt ' or 'data' chunk\n");
        return -1;
    }
    if (out->channels != 1u || out->bits_per_sample != 16u) {
        fprintf(stderr, "only 16-bit mono PCM is supported (channels=%u, bits=%u)\n", (unsigned)out->channels,
                (unsigned)out->bits_per_sample);
        return -1;
    }
    if (out->sample_rate_hz != FF_BANDENERGY_SAMPLE_RATE_HZ) {
        fprintf(stderr, "only %uHz is supported (file is %uHz)\n", (unsigned)FF_BANDENERGY_SAMPLE_RATE_HZ,
                (unsigned)out->sample_rate_hz);
        return -1;
    }
    return 0;
}

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

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <capture.wav>\n", argv[0]);
        fprintf(stderr, "  16-bit mono PCM WAV @ %uHz — see tools/beat_replay.py (decode/synth)\n",
                (unsigned)FF_BANDENERGY_SAMPLE_RATE_HZ);
        return 2;
    }

    size_t file_len = 0u;
    uint8_t *file_bytes = read_whole_file(argv[1], &file_len);
    if (file_bytes == NULL) {
        fprintf(stderr, "failed to read '%s'\n", argv[1]);
        return 1;
    }

    wav_t wav;
    if (wav_parse(file_bytes, file_len, &wav) != 0) {
        free(file_bytes);
        return 1;
    }

    size_t const n_frames = wav.n_samples / FRAME_SAMPLES;
    printf("beat_sim_replay: %s — %zu samples (%.2fs @ %uHz), %zu frames\n", argv[1], wav.n_samples,
           (double)wav.n_samples / (double)wav.sample_rate_hz, (unsigned)wav.sample_rate_hz, n_frames);

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
            printf("  BEAT   t=%7.3fs  #%-4u  bpm=%.1f\n", (double)now_ms / 1000.0, (unsigned)beat.beat_count,
                   (double)beat.bpm_estimate);
        }

        if (now_ms - last_status_ms >= STATUS_PERIOD_MS) {
            last_status_ms = now_ms;
            printf("t=%7.3fs  loudness=%.2f  low_dbfs=%6.1f  mid_dbfs=%6.1f  bpm=%.1f\n", (double)now_ms / 1000.0,
                   (double)beat.loudness, (double)bandf.low_dbfs, (double)bandf.mid_dbfs, (double)beat.bpm_estimate);
        }
    }

    printf("done: %zu frames, %u beats, final bpm_estimate=%.1f\n", n_frames, (unsigned)beat.beat_count,
           (double)beat.bpm_estimate);

    free(file_bytes);
    return 0;
}
