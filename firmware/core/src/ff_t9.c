#include "ff_t9.h"

#include <string.h>

/* Key letter tables, indexed by key (1-9). Key 0 (space) has no table —
 * handled directly by ff_t9_space / the key==0 branch of ff_t9_key. */
static char const *const kT9Letters[10] = {
    NULL,   /* 0: space, no cycle table */
    ".,?!", /* 1: punctuation */
    "abc",  /* 2 */
    "def",  /* 3 */
    "ghi",  /* 4 */
    "jkl",  /* 5 */
    "mno",  /* 6 */
    "pqrs", /* 7 */
    "tuv",  /* 8 */
    "wxyz", /* 9 */
};

static void t9_refresh_display(ff_t9_t *t)
{
    memcpy(t->display, t->buf, t->len);
    size_t n = t->len;
    if (t->has_pending) {
        t->display[n++] = t->pending_char;
    }
    t->display[n] = '\0';
}

/* Commit the pending char (if any) into the committed buffer, subject to
 * the 160-char cap. Does not touch the display cache — callers refresh it
 * afterward once they know the final state. */
static void t9_commit_pending(ff_t9_t *t)
{
    if (!t->has_pending) {
        return;
    }
    if (t->len < FF_T9_MAX_LEN) {
        t->buf[t->len] = t->pending_char;
        t->len++;
        t->buf[t->len] = '\0';
    }
    t->has_pending = false;
}

void ff_t9_reset(ff_t9_t *t)
{
    if (!t) {
        return;
    }
    memset(t, 0, sizeof(*t));
    /* buf[0], display[0] already '\0' via memset */
}

void ff_t9_space(ff_t9_t *t)
{
    if (!t) {
        return;
    }
    t9_commit_pending(t);
    if (t->len < FF_T9_MAX_LEN) {
        t->buf[t->len] = ' ';
        t->len++;
        t->buf[t->len] = '\0';
    }
    t9_refresh_display(t);
}

void ff_t9_key(ff_t9_t *t, uint8_t key, uint32_t now_ms)
{
    if (!t) {
        return;
    }
    if (key == 0) {
        ff_t9_space(t); /* header doc: identical to ff_t9_space */
        return;
    }
    if (key > 9) {
        return; /* defensive: ignore an invalid raw key code */
    }

    /* Half-open commit window: strictly-less-than 900ms since the last
     * press of THIS SAME key continues the cycle; anything else (a
     * different key, or >= 900ms elapsed) commits first. */
    uint32_t elapsed = now_ms - t->last_key_ms; /* wraparound-safe, ff_clock_t convention */
    bool continue_cycle = t->has_pending && key == t->pending_key && elapsed < FF_T9_COMMIT_MS;

    if (continue_cycle) {
        char const *letters = kT9Letters[key];
        uint8_t n = (uint8_t)strlen(letters);
        t->cycle_idx = (uint8_t)((t->cycle_idx + 1u) % n);
        t->pending_char = letters[t->cycle_idx];
        t->last_key_ms = now_ms;
        t9_refresh_display(t);
        return;
    }

    t9_commit_pending(t);

    if (t->len >= FF_T9_MAX_LEN) {
        /* At the cap: no room for a new pending char either. Leave
         * whatever just got committed in place and stop. */
        t9_refresh_display(t);
        return;
    }

    char const *letters = kT9Letters[key];
    t->cycle_idx = 0;
    t->pending_char = letters[0];
    t->pending_key = key;
    t->has_pending = true;
    t->last_key_ms = now_ms;
    t9_refresh_display(t);
}

void ff_t9_tick(ff_t9_t *t, uint32_t now_ms)
{
    if (!t || !t->has_pending) {
        return;
    }
    uint32_t elapsed = now_ms - t->last_key_ms; /* wraparound-safe */
    if (elapsed >= FF_T9_COMMIT_MS) {
        t9_commit_pending(t);
        t9_refresh_display(t);
    }
}

void ff_t9_backspace(ff_t9_t *t)
{
    if (!t) {
        return;
    }
    if (t->has_pending) {
        /* Discard the pending char outright (not a step back in its
         * cycle) — the next press of that key starts a fresh cycle. */
        t->has_pending = false;
        t->pending_key = 0;
        t->cycle_idx = 0;
    } else if (t->len > 0) {
        t->len--;
        t->buf[t->len] = '\0';
    }
    t9_refresh_display(t);
}

char const *ff_t9_text(ff_t9_t const *t)
{
    if (!t) {
        return "";
    }
    return t->display;
}

size_t ff_t9_suggest(ff_t9_t const *t, char const **out, size_t max_out)
{
    (void)t;
    (void)out;
    (void)max_out;
    return 0; /* v1: multi-tap only, no predictive dictionary — see header doc */
}
