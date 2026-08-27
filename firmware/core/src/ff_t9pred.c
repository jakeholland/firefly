#include "ff_t9pred.h"

#include "ff_t9dict.h"

/*
 * Predictive T9 engine. See ff_t9pred.h for the contract. The dictionary is a
 * frequency-ordered, NUL-terminated word blob (ff_t9dict.h); everything below
 * is a bounded forward walk of that blob — no heap, no index, no I/O.
 */

/* Letter -> keypad digit, for 'a'..'z'. 0 marks a non-letter (never happens
 * for dictionary words, which are [a-z] by construction, but keeps the mapper
 * total and defensive). */
static const uint8_t kLetterToDigit[26] = {
    /* a b c */ 2, 2, 2,
    /* d e f */ 3, 3, 3,
    /* g h i */ 4, 4, 4,
    /* j k l */ 5, 5, 5,
    /* m n o */ 6, 6, 6,
    /* p q r s */ 7, 7, 7, 7,
    /* t u v */ 8, 8, 8,
    /* w x y z */ 9, 9, 9, 9,
};

static uint8_t letter_digit(char c)
{
    if (c >= 'a' && c <= 'z') {
        return kLetterToDigit[c - 'a'];
    }
    return 0;
}

/* Validate a digit sequence: non-NULL, length in 1..MAX, every digit 2..9. */
static bool digits_valid(uint8_t const *digits, size_t n)
{
    if (!digits || n == 0 || n > FF_T9PRED_MAX_DIGITS) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (digits[i] < 2 || digits[i] > 9) {
            return false;
        }
    }
    return true;
}

/* Does dictionary word `w` (NUL-terminated) match digit sequence `digits`
 * (length `n`) as a prefix? True iff strlen(w) >= n and the first n letters of
 * w map to exactly digits[0..n). */
static bool word_matches(char const *w, uint8_t const *digits, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (w[i] == '\0') {
            return false; /* word shorter than the sequence */
        }
        if (letter_digit(w[i]) != digits[i]) {
            return false;
        }
    }
    return true;
}

/* Advance `p` past the current NUL-terminated word to the start of the next.
 * `p` must point at a word start within the blob. */
static char const *next_word(char const *p)
{
    while (*p != '\0') {
        p++;
    }
    return p + 1; /* step over the terminator */
}

size_t ff_t9pred_match(uint8_t const *digits, size_t n,
                       char const **out, size_t max_out)
{
    if (!out || max_out == 0 || !digits_valid(digits, n)) {
        return 0;
    }

    size_t written = 0;
    char const *p = ff_t9dict_blob;
    /* Walk in frequency order: the first `max_out` prefix-matches we meet are,
     * by construction of the blob, the most frequent ones. Stop early once the
     * output is full. */
    for (unsigned i = 0; i < ff_t9dict_count && written < max_out; i++) {
        if (word_matches(p, digits, n)) {
            out[written++] = p;
        }
        p = next_word(p);
    }
    return written;
}

size_t ff_t9pred_count(uint8_t const *digits, size_t n)
{
    if (!digits_valid(digits, n)) {
        return 0;
    }
    size_t total = 0;
    char const *p = ff_t9dict_blob;
    for (unsigned i = 0; i < ff_t9dict_count; i++) {
        if (word_matches(p, digits, n)) {
            total++;
        }
        p = next_word(p);
    }
    return total;
}

/* Return the `idx`-th (0-based) matching word in frequency order, or NULL if
 * there are `idx` or fewer matches. Bounded forward walk. */
static char const *nth_match(uint8_t const *digits, size_t n, size_t idx)
{
    size_t seen = 0;
    char const *p = ff_t9dict_blob;
    for (unsigned i = 0; i < ff_t9dict_count; i++) {
        if (word_matches(p, digits, n)) {
            if (seen == idx) {
                return p;
            }
            seen++;
        }
        p = next_word(p);
    }
    return NULL;
}

size_t ff_t9pred_match_str(char const *digits,
                           char const **out, size_t max_out)
{
    if (!digits) {
        return 0;
    }
    uint8_t seq[FF_T9PRED_MAX_DIGITS];
    size_t n = 0;
    for (; digits[n] != '\0'; n++) {
        if (n >= FF_T9PRED_MAX_DIGITS) {
            return 0; /* too long to match anything */
        }
        char c = digits[n];
        if (c < '2' || c > '9') {
            return 0; /* invalid key char -> whole query invalid */
        }
        seq[n] = (uint8_t)(c - '0');
    }
    return ff_t9pred_match(seq, n, out, max_out);
}

/* ------------------------------------------------------------------ */
/* Session                                                             */
/* ------------------------------------------------------------------ */

void ff_t9pred_session_reset(ff_t9pred_session_t *s)
{
    if (!s) {
        return;
    }
    s->n = 0;
    s->sel = 0;
    for (size_t i = 0; i < FF_T9PRED_MAX_DIGITS; i++) {
        s->digits[i] = 0;
    }
}

bool ff_t9pred_session_key(ff_t9pred_session_t *s, uint8_t key)
{
    if (!s || key < 2 || key > 9 || s->n >= FF_T9PRED_MAX_DIGITS) {
        return false;
    }
    s->digits[s->n++] = key;
    s->sel = 0; /* a new digit changes the candidate set; re-anchor to top */
    return true;
}

bool ff_t9pred_session_backspace(ff_t9pred_session_t *s)
{
    if (!s || s->n == 0) {
        return false;
    }
    s->n--;
    s->digits[s->n] = 0;
    s->sel = 0;
    return true;
}

void ff_t9pred_session_cycle(ff_t9pred_session_t *s)
{
    if (!s) {
        return;
    }
    size_t total = ff_t9pred_count(s->digits, s->n);
    if (total < 2) {
        return; /* nothing to cycle through (0 or 1 candidate) */
    }
    s->sel = (uint16_t)((s->sel + 1u) % total);
}

char const *ff_t9pred_session_current(ff_t9pred_session_t const *s)
{
    if (!s || s->n == 0) {
        return NULL;
    }
    return nth_match(s->digits, s->n, s->sel);
}

size_t ff_t9pred_session_candidates(ff_t9pred_session_t const *s,
                                    char const **out, size_t max_out)
{
    if (!s) {
        return 0;
    }
    return ff_t9pred_match(s->digits, s->n, out, max_out);
}
