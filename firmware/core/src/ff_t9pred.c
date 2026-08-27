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

/* Map a byte to its keypad digit, CASE-INSENSITIVELY. The static dictionary is
 * lowercase [a-z], but caller-supplied supplementary words (see match_ex) are
 * real-world strings that may carry capitals ("Excision"); folding case here
 * lets them match their lowercase digit sequence. Any non-letter byte (space,
 * '&', digit, punctuation) maps to 0 — not a keypad letter, so it cannot
 * match, which naturally bounds a multi-word name at its first non-letter. */
static uint8_t letter_digit(char c)
{
    if (c >= 'a' && c <= 'z') {
        return kLetterToDigit[c - 'a'];
    }
    if (c >= 'A' && c <= 'Z') {
        return kLetterToDigit[c - 'A'];
    }
    return 0;
}

/* ASCII lowercase, for case-insensitive word de-dup. */
static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive ASCII string equality. */
static bool ci_equal(char const *a, char const *b)
{
    size_t i = 0;
    for (; a[i] != '\0' && b[i] != '\0'; i++) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return a[i] == b[i]; /* both ended together */
}

/* Is `w` case-insensitively equal to any of the `n_extra` supplementary words?
 * Used to suppress a dictionary word already represented by a supplied word. */
static bool in_extra(char const *w, char const *const *extra, int n_extra)
{
    for (int i = 0; i < n_extra; i++) {
        if (extra[i] && ci_equal(w, extra[i])) {
            return true;
        }
    }
    return false;
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

size_t ff_t9pred_match_ex(uint8_t const *digits, size_t n,
                          char const **out, size_t max_out,
                          char const *const *extra, int n_extra)
{
    if (!out || max_out == 0 || !digits_valid(digits, n)) {
        return 0;
    }
    if (!extra || n_extra < 0) {
        n_extra = 0;
    }

    size_t written = 0;

    /* 1. Supplementary words first (higher rank), in supplied order, de-duped
     *    against each other case-insensitively (first occurrence wins). */
    for (int i = 0; i < n_extra && written < max_out; i++) {
        char const *w = extra[i];
        if (!w || w[0] == '\0' || !word_matches(w, digits, n)) {
            continue;
        }
        bool dup = false;
        for (size_t j = 0; j < written; j++) {
            if (ci_equal(out[j], w)) { dup = true; break; }
        }
        if (!dup) {
            out[written++] = w;
        }
    }

    /* 2. Then the static dictionary in frequency order, suppressing any word
     *    already represented by a supplied extra word. */
    char const *p = ff_t9dict_blob;
    for (unsigned i = 0; i < ff_t9dict_count && written < max_out; i++) {
        if (word_matches(p, digits, n) && !in_extra(p, extra, n_extra)) {
            out[written++] = p;
        }
        p = next_word(p);
    }
    return written;
}

size_t ff_t9pred_match(uint8_t const *digits, size_t n,
                       char const **out, size_t max_out)
{
    return ff_t9pred_match_ex(digits, n, out, max_out, NULL, 0);
}

size_t ff_t9pred_count_ex(uint8_t const *digits, size_t n,
                          char const *const *extra, int n_extra)
{
    if (!digits_valid(digits, n)) {
        return 0;
    }
    if (!extra || n_extra < 0) {
        n_extra = 0;
    }

    size_t total = 0;
    /* Unique supplementary matches. */
    for (int i = 0; i < n_extra; i++) {
        char const *w = extra[i];
        if (!w || w[0] == '\0' || !word_matches(w, digits, n)) {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (extra[j] && ci_equal(extra[j], w)) { dup = true; break; }
        }
        if (!dup) {
            total++;
        }
    }
    /* Dictionary matches not represented by a supplied word. */
    char const *p = ff_t9dict_blob;
    for (unsigned i = 0; i < ff_t9dict_count; i++) {
        if (word_matches(p, digits, n) && !in_extra(p, extra, n_extra)) {
            total++;
        }
        p = next_word(p);
    }
    return total;
}

size_t ff_t9pred_count(uint8_t const *digits, size_t n)
{
    return ff_t9pred_count_ex(digits, n, NULL, 0);
}

/* Return the `idx`-th (0-based) matching word, in the same order match_ex
 * emits (supplementary first, then dictionary), or NULL if there are `idx` or
 * fewer matches. Bounded forward walk. */
static char const *nth_match_ex(uint8_t const *digits, size_t n, size_t idx,
                                char const *const *extra, int n_extra)
{
    if (!extra || n_extra < 0) {
        n_extra = 0;
    }
    size_t seen = 0;
    /* Supplementary words first (unique). */
    for (int i = 0; i < n_extra; i++) {
        char const *w = extra[i];
        if (!w || w[0] == '\0' || !word_matches(w, digits, n)) {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (extra[j] && ci_equal(extra[j], w)) { dup = true; break; }
        }
        if (dup) {
            continue;
        }
        if (seen == idx) {
            return w;
        }
        seen++;
    }
    /* Then dictionary (suppressing extra dupes). */
    char const *p = ff_t9dict_blob;
    for (unsigned i = 0; i < ff_t9dict_count; i++) {
        if (word_matches(p, digits, n) && !in_extra(p, extra, n_extra)) {
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
    s->extra = NULL;
    s->n_extra = 0;
}

void ff_t9pred_session_set_extra(ff_t9pred_session_t *s,
                                 char const *const *extra, int n_extra)
{
    if (!s) {
        return;
    }
    if (!extra || n_extra < 0) {
        extra = NULL;
        n_extra = 0;
    }
    s->extra = extra;
    s->n_extra = n_extra;
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
    size_t total = ff_t9pred_count_ex(s->digits, s->n, s->extra, s->n_extra);
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
    return nth_match_ex(s->digits, s->n, s->sel, s->extra, s->n_extra);
}

size_t ff_t9pred_session_candidates(ff_t9pred_session_t const *s,
                                    char const **out, size_t max_out)
{
    if (!s) {
        return 0;
    }
    return ff_t9pred_match_ex(s->digits, s->n, out, max_out, s->extra, s->n_extra);
}
