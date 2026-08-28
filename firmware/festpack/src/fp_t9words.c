#include "fp_t9words.h"

/*
 * festpack -> T9 supplementary-word bridge. See fp_t9words.h for placement
 * rationale and contract. Pure: a bounded walk of the pack's fixed-size name
 * tables, no heap, no I/O. Note this file does NOT include ff_t9pred.h — the
 * bridge only produces plain C strings; the caller feeds them to the engine.
 * That one-way arrow (app -> {festpack bridge, core/t9pred}) is what keeps
 * core/t9pred free of any festpack dependency.
 */

static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive ASCII string equality (local copy — the bridge does not
 * link the engine). */
static bool ci_equal(char const *a, char const *b)
{
    size_t i = 0;
    for (; a[i] != '\0' && b[i] != '\0'; i++) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return a[i] == b[i];
}

/* Append `name` to out[] unless empty or a case-insensitive duplicate of an
 * already-collected name. Returns the updated count. */
static int push_unique(char const *name, char const **out, int count, int max)
{
    if (!name || name[0] == '\0' || count >= max) {
        return count;
    }
    for (int i = 0; i < count; i++) {
        if (ci_equal(out[i], name)) {
            return count; /* already have it */
        }
    }
    out[count++] = name;
    return count;
}

int fp_t9words_collect(fp_pack_t const *pack, char const **out, int max)
{
    if (!pack || !out || max <= 0) {
        return 0;
    }

    int count = 0;

    for (uint16_t i = 0; i < pack->n_sets && count < max; i++) {
        count = push_unique(pack->sets[i].artist, out, count, max);
    }
    for (uint8_t i = 0; i < pack->n_stages && count < max; i++) {
        count = push_unique(pack->stages[i].name, out, count, max);
    }
    for (uint8_t i = 0; i < pack->n_landmarks && count < max; i++) {
        count = push_unique(pack->landmarks[i].name, out, count, max);
    }

    return count;
}
