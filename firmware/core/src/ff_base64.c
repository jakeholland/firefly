/**
 * ff_base64.c — see ff_base64.h.
 */
#include "ff_base64.h"

static char const k_alphabet[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t ff_base64_encoded_len(size_t n)
{
    return 4u * ((n + 2u) / 3u);
}

bool ff_base64_encode(uint8_t const *in, size_t n, char *out, size_t out_cap)
{
    if (out == NULL) return false;
    if (in == NULL && n > 0u) return false;

    size_t const need = ff_base64_encoded_len(n) + 1u; /* +1 for the NUL */
    if (out_cap < need) return false;

    size_t oi = 0u;
    size_t i = 0u;
    while (i + 3u <= n) {
        uint32_t const v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1u] << 8) | (uint32_t)in[i + 2u];
        out[oi++] = k_alphabet[(v >> 18) & 0x3Fu];
        out[oi++] = k_alphabet[(v >> 12) & 0x3Fu];
        out[oi++] = k_alphabet[(v >> 6) & 0x3Fu];
        out[oi++] = k_alphabet[v & 0x3Fu];
        i += 3u;
    }

    size_t const remaining = n - i;
    if (remaining == 1u) {
        uint32_t const v = (uint32_t)in[i] << 16;
        out[oi++] = k_alphabet[(v >> 18) & 0x3Fu];
        out[oi++] = k_alphabet[(v >> 12) & 0x3Fu];
        out[oi++] = '=';
        out[oi++] = '=';
    } else if (remaining == 2u) {
        uint32_t const v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1u] << 8);
        out[oi++] = k_alphabet[(v >> 18) & 0x3Fu];
        out[oi++] = k_alphabet[(v >> 12) & 0x3Fu];
        out[oi++] = k_alphabet[(v >> 6) & 0x3Fu];
        out[oi++] = '=';
    }

    out[oi] = '\0';
    return true;
}
