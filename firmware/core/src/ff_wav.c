/**
 * ff_wav.c — see ff_wav.h.
 */
#include "ff_wav.h"

#include <string.h>

int ff_wav_parse(uint8_t const *file_bytes, size_t file_len, ff_wav_t *out)
{
    if (out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    if (file_bytes == NULL) return -1;
    if (file_len < 44u || memcmp(file_bytes, "RIFF", 4) != 0 || memcmp(file_bytes + 8, "WAVE", 4) != 0) {
        return -1;
    }

    size_t pos = 12u;
    bool have_fmt = false;
    while (pos + 8u <= file_len) {
        uint32_t chunk_size;
        memcpy(&chunk_size, file_bytes + pos + 4u, 4);
        size_t const body = pos + 8u;
        if (body + chunk_size > file_len) {
            return -1; /* truncated chunk */
        }

        if (memcmp(file_bytes + pos, "fmt ", 4) == 0) {
            if (chunk_size < 16u) return -1; /* fmt chunk too small */
            uint16_t audio_format, channels, bits_per_sample;
            uint32_t sample_rate;
            memcpy(&audio_format, file_bytes + body + 0u, 2);
            memcpy(&channels, file_bytes + body + 2u, 2);
            memcpy(&sample_rate, file_bytes + body + 4u, 4);
            memcpy(&bits_per_sample, file_bytes + body + 14u, 2);
            if (audio_format != 1u /* PCM */) return -1;
            out->sample_rate_hz = sample_rate;
            out->channels = channels;
            out->bits_per_sample = bits_per_sample;
            have_fmt = true;
        } else if (memcmp(file_bytes + pos, "data", 4) == 0) {
            if (!have_fmt) return -1; /* 'data' before 'fmt ' */
            out->samples = (int16_t const *)(void const *)(file_bytes + body);
            out->n_samples = chunk_size / sizeof(int16_t);
        }

        pos = body + chunk_size + (chunk_size & 1u); /* chunks are word-aligned */
    }

    if (!have_fmt || out->samples == NULL) return -1; /* missing 'fmt ' or 'data' */
    return 0;
}

bool ff_wav_is_16bit_mono_at(ff_wav_t const *wav, uint32_t expected_sample_rate_hz)
{
    if (wav == NULL) return false;
    return wav->channels == 1u && wav->bits_per_sample == 16u && wav->sample_rate_hz == expected_sample_rate_hz;
}
