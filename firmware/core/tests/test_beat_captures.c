/**
 * test_beat_captures.c — 2026-09-09 amendment (fix/s31-beat-real-
 * captures): regression coverage for `ff_beat`/`ff_bandenergy` against
 * REAL captured audio, not just synthetic signals — the deliverable's
 * own "add the two real captures as regression tests with the numbers
 * you establish".
 *
 * Fixtures (`firmware/core/tests/fixtures/audio/`):
 *   - `mic_dump_set_1.wav` / `mic_dump_set_2.wav` — two 10s, 16kHz
 *     16-bit mono captures of a real dubstep set (Crankdat) playing
 *     from a speaker near Jake's puck (no subwoofer), taken with the
 *     bench console's `mic dump 10` and decoded by `tools/
 *     beat_replay.py` — see docs/specs/S30-audio-input.md's own dated
 *     addition for that command, and docs/specs/S31-music-swarm.md's
 *     2026-09-09 amendment for the full writeup this test's own numbers
 *     come from.
 *   - `mic_dump_set_1_onsets.txt` / `mic_dump_set_2_onsets.txt` —
 *     ground-truth onset timestamps (seconds, one per line): local
 *     maxima of each capture's own LOW-band onset flux (the SAME
 *     one-pole-cascade filter `ff_bandenergy.c` actually runs, fed
 *     through this test's own reference pipeline offline) that clear
 *     that flux's own median + 2*MAD (or a 0.5dB absolute floor,
 *     whichever is higher) and sit at least 350ms apart — i.e. "the
 *     low-band onsets the [flux's own] autocorrelation implies", per
 *     the deliverable's own wording. Regenerate with `python3 tools/
 *     beat_capture_onsets.py <wav>` if a fixture ever changes (see
 *     that script's own header for the exact method).
 *
 * This file runs BOTH captures through the EXACT SAME per-frame
 * pipeline `beat_sim_replay.c` and the real esp32s3 `ff_mic.c` reader
 * task use (DC-remove -> RMS/envelope -> band energy -> `ff_beat_
 * update`, at the real 20ms/50Hz cadence) and asserts, per capture:
 *   - at least 80% of that capture's own ground-truth onsets are
 *     matched by a detected beat within a fixed 150ms window (see
 *     `beat_sim_replay.c`'s own SUMMARY line doc comment for why a
 *     fixed window, not one derived from the onset file's own local
 *     gaps);
 *   - the FINAL `bpm_estimate` is within a couple BPM of this
 *     capture's own honestly-established number (125.0 for capture 1,
 *     142.9 for capture 2 — see `docs/specs/S31-music-swarm.md`'s
 *     dated amendment for exactly how these were determined: two
 *     independent low-band-flux-autocorrelation analyses of the SAME
 *     captures, and the "kick pulse vs. half-time downbeat" call
 *     `ff_beat.h`'s own "Beat-tracking" doc comment explains);
 *   - the estimate has actually SETTLED by the end, not merely landed
 *     there by chance on the very last beat: the last several
 *     `bpm_estimate` readings are all equal (within the same couple-
 *     BPM band) — this is the "stable" half of "a stable BPM within
 *     +-3 of the dominant periodicity".
 *
 * A third test, `synthetic_click_track_replay_detects_beats_near_
 * 128bpm` (below the two real-capture tests), runs the SAME shared
 * pipeline (`replay_wav_bytes`) on an in-memory, hand-synthesized 128
 * BPM click train instead of a fixture file — the deliverable's own
 * "a test that runs the replay on a short synthetic click track",
 * covering the WAV-parse -> frame-pipeline PATH itself (which the two
 * real-capture tests above also exercise, but only via committed
 * binary fixtures) without adding a third audio file to the repo. See
 * that test's own comment for how it differs from test_beat.c's
 * pre-existing `click_train_128bpm_yields_beats_within_30ms`.
 */
#include "ff_bandenergy.h"
#include "ff_beat.h"
#include "ff_miclevel.h"
#include "ff_wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#ifndef FF_BEAT_FIXTURES_DIR
#define FF_BEAT_FIXTURES_DIR "tests/fixtures/audio"
#endif

#define FRAME_SAMPLES FF_MICLEVEL_FRAME_SAMPLES /* 320 — 20ms at 16kHz */
#define MATCH_WINDOW_S 0.15f                    /* mirrors beat_sim_replay.c's own SUMMARY window */
#define MAX_ONSETS 64u
#define MAX_BEATS 64u

void setUp(void) {}
void tearDown(void) {}

/* --------------------------------------------------------------------
 * Fixture loading — mirrors test_proto.c's own "read a fixture file
 * relative to FF_..._FIXTURES_DIR" pattern (ff_beat.h's top comment,
 * cited by this file's own header, plus test_proto.c precedent).
 * ------------------------------------------------------------------- */

static uint8_t *read_fixture_file(char const *name, size_t *out_len)
{
    char path[256];
    (void)snprintf(path, sizeof(path), "%s/%s", FF_BEAT_FIXTURES_DIR, name);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fseek(f, 0, SEEK_END);
    long const len = ftell(f);
    fseek(f, 0, SEEK_SET);
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, len, path);
    uint8_t *buf = malloc((size_t)len);
    TEST_ASSERT_NOT_NULL(buf);
    size_t const got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((size_t)len, got, path);
    *out_len = (size_t)len;
    return buf;
}

static size_t load_onsets(char const *name, float *out, size_t cap)
{
    char path[256];
    (void)snprintf(path, sizeof(path), "%s/%s", FF_BEAT_FIXTURES_DIR, name);
    FILE *f = fopen(path, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
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

/* --------------------------------------------------------------------
 * Reference pipeline — the SAME per-frame chain beat_sim_replay.c and
 * the real esp32s3 ff_mic.c reader task run: DC-remove -> RMS/envelope
 * -> band energy -> ff_beat_update, at the real 20ms/50Hz cadence.
 * ------------------------------------------------------------------- */

typedef struct {
    float beat_times_s[MAX_BEATS];
    size_t n_beats;
    float bpm_history[MAX_BEATS]; /* bpm_estimate AT the moment of each beat_times_s entry */
    float final_bpm;
    uint32_t final_beat_count;
} replay_result_t;

/* Core replay pipeline, over an already-in-memory WAV byte buffer —
 * factored out of `replay_wav` (below) so the SAME pipeline also serves
 * `synthetic_click_track_replay_...` (this file's own in-memory
 * synthetic fixture, no file I/O) without a second hand-copied loop.
 * Does NOT free/own `file_bytes` — the caller does. */
static void replay_wav_bytes(uint8_t const *file_bytes, size_t file_len, char const *label, replay_result_t *out)
{
    memset(out, 0, sizeof(*out));

    ff_wav_t wav;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ff_wav_parse(file_bytes, file_len, &wav), label);
    TEST_ASSERT_TRUE_MESSAGE(ff_wav_is_16bit_mono_at(&wav, FF_BANDENERGY_SAMPLE_RATE_HZ), label);

    ff_miclevel_dc_state_t dc;
    ff_miclevel_envelope_t env;
    ff_bandenergy_t band;
    ff_beat_t beat;
    ff_miclevel_dc_reset(&dc);
    ff_miclevel_envelope_reset(&env);
    ff_bandenergy_reset(&band);
    ff_beat_reset(&beat);

    size_t const n_frames = wav.n_samples / FRAME_SAMPLES;
    float float_buf[FRAME_SAMPLES];
    uint32_t now_ms = 0u;
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
            if (out->n_beats < MAX_BEATS) {
                out->beat_times_s[out->n_beats] = (float)now_ms / 1000.0f;
                out->bpm_history[out->n_beats] = beat.bpm_estimate;
                out->n_beats++;
            }
        }
    }

    out->final_bpm = beat.bpm_estimate;
    out->final_beat_count = beat.beat_count;
}

/* Fixture-file wrapper around `replay_wav_bytes` — reads
 * `FF_BEAT_FIXTURES_DIR/wav_name`, runs it through the same pipeline,
 * and frees the file buffer itself. */
static void replay_wav(char const *wav_name, replay_result_t *out)
{
    size_t file_len = 0u;
    uint8_t *file_bytes = read_fixture_file(wav_name, &file_len);
    replay_wav_bytes(file_bytes, file_len, wav_name, out);
    free(file_bytes);
}

/* Fraction of `n_onsets` ground-truth timestamps matched by some
 * detected beat within MATCH_WINDOW_S. */
static float match_fraction(replay_result_t const *r, float const *onsets, size_t n_onsets)
{
    size_t matched = 0u;
    for (size_t i = 0; i < n_onsets; i++) {
        for (size_t j = 0; j < r->n_beats; j++) {
            float diff = r->beat_times_s[j] - onsets[i];
            if (diff < 0.0f) diff = -diff;
            if (diff <= MATCH_WINDOW_S) {
                matched++;
                break;
            }
        }
    }
    return (n_onsets > 0u) ? ((float)matched / (float)n_onsets) : 0.0f;
}

/* The estimate must have actually SETTLED by the end — the last
 * `n_tail` bpm_history entries all within +-3 of the final value, not
 * merely "the last one happened to land close". */
static bool bpm_settled(replay_result_t const *r, size_t n_tail, float tolerance)
{
    if (r->n_beats < n_tail) return false;
    for (size_t i = r->n_beats - n_tail; i < r->n_beats; i++) {
        float diff = r->bpm_history[i] - r->final_bpm;
        if (diff < 0.0f) diff = -diff;
        if (diff > tolerance) return false;
    }
    return true;
}

/* --------------------------------------------------------------------
 * capture 1 — established BPM 125.0, from a stable-by-mid-file
 * convergence (see this file's own top comment).
 * ------------------------------------------------------------------- */

static void capture_1_detects_most_onsets_and_settles_near_125bpm(void)
{
    replay_result_t r;
    replay_wav("mic_dump_set_1.wav", &r);

    static float onsets[MAX_ONSETS];
    size_t const n_onsets = load_onsets("mic_dump_set_1_onsets.txt", onsets, MAX_ONSETS);
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0, n_onsets, "onsets fixture failed to load");

    float const frac = match_fraction(&r, onsets, n_onsets);
    TEST_ASSERT_GREATER_OR_EQUAL_FLOAT_MESSAGE(0.80f, frac, "capture 1: detected fewer than 80% of its low-band onsets");

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(3.0f, 125.0f, r.final_bpm, "capture 1: final bpm_estimate drifted from 125.0");
    TEST_ASSERT_TRUE_MESSAGE(bpm_settled(&r, 6u, 3.0f), "capture 1: bpm_estimate never settled (last 6 beats not stable)");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(15u, r.final_beat_count,
                                                 "capture 1: too few beats over 10s (PR #254's own '2 beats' regression)");
}

/* --------------------------------------------------------------------
 * capture 2 — established BPM 142.9.
 * ------------------------------------------------------------------- */

static void capture_2_detects_most_onsets_and_settles_near_142_9bpm(void)
{
    replay_result_t r;
    replay_wav("mic_dump_set_2.wav", &r);

    static float onsets[MAX_ONSETS];
    size_t const n_onsets = load_onsets("mic_dump_set_2_onsets.txt", onsets, MAX_ONSETS);
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0, n_onsets, "onsets fixture failed to load");

    float const frac = match_fraction(&r, onsets, n_onsets);
    TEST_ASSERT_GREATER_OR_EQUAL_FLOAT_MESSAGE(0.80f, frac, "capture 2: detected fewer than 80% of its low-band onsets");

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(3.0f, 142.9f, r.final_bpm, "capture 2: final bpm_estimate drifted from 142.9");
    TEST_ASSERT_TRUE_MESSAGE(bpm_settled(&r, 6u, 3.0f), "capture 2: bpm_estimate never settled (last 6 beats not stable)");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(15u, r.final_beat_count,
                                                 "capture 2: too few beats over 10s (PR #254's own '2 beats' regression)");
}

/* --------------------------------------------------------------------
 * Synthetic click-track replay — exercises the SAME WAV-parse ->
 * frame-pipeline path (`replay_wav_bytes` above, shared with
 * `beat_sim_replay.c`'s own main loop) as the two real-capture tests
 * above, but on an in-memory, hand-synthesized 128 BPM click train
 * instead of a committed fixture file. This is the deliverable's own
 * "a test that runs the replay on a short synthetic click track" —
 * complementary to, not a replacement for, `test_beat.c`'s own
 * `click_train_128bpm_yields_beats_within_30ms` (which feeds
 * `ff_beat_update` directly with hand-picked dBFS levels and so never
 * exercises `ff_wav.c`'s parser or the DC-remove/RMS/band-energy
 * frame chain those precomputed levels bypass). No fixture file is
 * committed for this — the WAV bytes are built here, in C, so the
 * replay PATH itself (parse -> frame pipeline) gets covered without
 * adding a third audio binary to the repo.
 * ------------------------------------------------------------------- */

#define CLICK_SAMPLE_RATE_HZ FF_BANDENERGY_SAMPLE_RATE_HZ
#define CLICK_BURST_SAMPLES  8u /* ~0.5ms alternating +/-max-amplitude burst per click — a broadband transient,
                                    the same "genuine broadband event in near-silence" test_beat.c's own click
                                    train models, just as actual PCM samples instead of a precomputed dBFS level */

/* Builds a minimal canonical RIFF/WAVE (16-bit mono PCM @ 16kHz) byte
 * buffer holding a `bpm`-periodic click train `duration_s` long:
 * silence except for a CLICK_BURST_SAMPLES alternating +/-32000 burst
 * at the start of every period. Caller frees the returned buffer. */
static uint8_t *build_click_wav(float bpm, float duration_s, size_t *out_len)
{
    size_t const n_samples = (size_t)(duration_s * (float)CLICK_SAMPLE_RATE_HZ);
    size_t const data_bytes = n_samples * sizeof(int16_t);
    size_t const total_len = 44u + data_bytes; /* canonical 44-byte header, no extra chunks */
    uint8_t *buf = malloc(total_len);
    TEST_ASSERT_NOT_NULL(buf);

    uint32_t const riff_chunk_size = (uint32_t)(36u + data_bytes);
    uint32_t const fmt_chunk_size = 16u;
    uint16_t const audio_format = 1u; /* PCM */
    uint16_t const channels = 1u;
    uint32_t const sample_rate = CLICK_SAMPLE_RATE_HZ;
    uint16_t const bits_per_sample = 16u;
    uint32_t const byte_rate = sample_rate * channels * (bits_per_sample / 8u);
    uint16_t const block_align = (uint16_t)(channels * (bits_per_sample / 8u));
    uint32_t const data_chunk_size = (uint32_t)data_bytes;

    uint8_t *p = buf;
    memcpy(p, "RIFF", 4); p += 4;
    memcpy(p, &riff_chunk_size, 4); p += 4;
    memcpy(p, "WAVE", 4); p += 4;
    memcpy(p, "fmt ", 4); p += 4;
    memcpy(p, &fmt_chunk_size, 4); p += 4;
    memcpy(p, &audio_format, 2); p += 2;
    memcpy(p, &channels, 2); p += 2;
    memcpy(p, &sample_rate, 4); p += 4;
    memcpy(p, &byte_rate, 4); p += 4;
    memcpy(p, &block_align, 2); p += 2;
    memcpy(p, &bits_per_sample, 2); p += 2;
    memcpy(p, "data", 4); p += 4;
    memcpy(p, &data_chunk_size, 4); p += 4;

    int16_t *samples = (int16_t *)(void *)p;
    float const period_samples = (60.0f / bpm) * (float)CLICK_SAMPLE_RATE_HZ;
    float next_click_at = 0.0f;
    for (size_t i = 0; i < n_samples; i++) {
        size_t const since_click = (i >= (size_t)next_click_at) ? (i - (size_t)next_click_at) : (size_t)-1;
        if (since_click < CLICK_BURST_SAMPLES) {
            samples[i] = (since_click % 2u == 0u) ? (int16_t)32000 : (int16_t)-32000;
        } else {
            samples[i] = 0;
        }
        if ((float)i >= next_click_at + period_samples) {
            next_click_at += period_samples;
        }
    }

    *out_len = total_len;
    return buf;
}

static void synthetic_click_track_replay_detects_beats_near_128bpm(void)
{
    size_t wav_len = 0u;
    uint8_t *wav_bytes = build_click_wav(/*bpm=*/128.0f, /*duration_s=*/6.0f, &wav_len);

    replay_result_t r;
    replay_wav_bytes(wav_bytes, wav_len, "synthetic click track", &r);
    free(wav_bytes);

    /* 6s at 128 BPM (468.75ms/click) is ~12.8 periods; the shared
     * refractory window (250ms, well under the ~469ms period) never
     * suppresses a real click here, so every click should be caught. */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(11u, r.final_beat_count,
                                                 "synthetic click track: too few beats over 6s at 128 BPM");
    TEST_ASSERT_LESS_OR_EQUAL_UINT32_MESSAGE(14u, r.final_beat_count,
                                              "synthetic click track: too many beats over 6s at 128 BPM");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(3.0f, 128.0f, r.final_bpm,
                                      "synthetic click track: final bpm_estimate drifted from the click train's own 128 BPM");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(capture_1_detects_most_onsets_and_settles_near_125bpm);
    RUN_TEST(capture_2_detects_most_onsets_and_settles_near_142_9bpm);
    RUN_TEST(synthetic_click_track_replay_detects_beats_near_128bpm);
    return UNITY_END();
}
