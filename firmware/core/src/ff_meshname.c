/**
 * ff_meshname.c — see ff_meshname.h.
 */
#include "ff_meshname.h"

#include <ctype.h>

static int is_allowed(char c)
{
    return isalnum((unsigned char)c) || c == ' ';
}

void ff_meshname_sanitize(char const *in, char *out, size_t out_cap)
{
    if (out == NULL || out_cap == 0u) return;

    size_t n = 0u;
    if (in != NULL) {
        for (size_t i = 0u; in[i] != '\0' && n < out_cap - 1u; ++i) {
            if (is_allowed(in[i])) {
                out[n++] = in[i];
            }
        }
    }
    out[n] = '\0';

    /* Trim trailing spaces (in place — n only ever shrinks). */
    while (n > 0u && out[n - 1u] == ' ') {
        out[--n] = '\0';
    }

    /* Trim leading spaces by shifting the remainder down. */
    size_t lead = 0u;
    while (lead < n && out[lead] == ' ') {
        ++lead;
    }
    if (lead > 0u) {
        size_t j = 0u;
        for (size_t i = lead; i <= n; ++i, ++j) { /* copies the NUL too */
            out[j] = out[i];
        }
    }
}

void ff_meshname_derive_short(char const *long_name, char out[FF_MESHNAME_SHORT_LEN])
{
    if (out == NULL) return;

    size_t n = 0u;
    if (long_name != NULL) {
        for (size_t i = 0u; long_name[i] != '\0' && n < FF_MESHNAME_SHORT_LEN - 1u; ++i) {
            unsigned char const c = (unsigned char)long_name[i];
            if (isalnum(c)) {
                out[n++] = (char)toupper(c);
            }
        }
    }
    out[n] = '\0';
}
