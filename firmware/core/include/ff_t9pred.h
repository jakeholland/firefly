/**
 * ff_t9pred.h — core/t9pred: the PREDICTIVE T9 text-entry engine.
 *
 * Spec: docs/specs/S08-signals-t9.md reserves predictive T9 ("Predictive +
 * pack dictionary = v1.5 spec addendum; API reserves `ff_t9_suggest()`").
 * This is the pure-core groundwork for that addendum: a dictionary + ranking
 * engine, unit-testable with no UI, no device, no I/O.
 *
 * ## Relationship to the existing multi-tap engine (`ff_t9`)
 * `ff_t9` (multi-tap: press a key N times per letter) is UNTOUCHED and still
 * what the composer uses. This module lives ALONGSIDE it. Wiring predictive
 * T9 into the compose UI/UX is a later maintainer-eyes slice and is NOT done
 * here — see the "Integration sketch" note at the bottom of this header.
 *
 * ## What "predictive T9" means here
 * Multi-tap makes you press 6-6-6 for 'o'. Predictive T9 lets you press each
 * key ONCE per letter — 4-6-6-3 — and ranks dictionary words whose letters
 * fall on those keys ("4663" -> good, home, gone, ...). The digit->letters
 * map is the standard phone keypad:
 *   2=abc 3=def 4=ghi 5=jkl 6=mno 7=pqrs 8=tuv 9=wxyz
 * Keys 0 and 1 are NOT letter keys (0=space, 1=punctuation in `ff_t9`) and are
 * rejected by this engine's inputs.
 *
 * ## Matching model — prefix / as-you-type
 * A query is a sequence of digit keys. A dictionary word MATCHES the sequence
 * when it is at least as long as the sequence and its first N letters map to
 * exactly those N digits. So the same call serves both the "full sequence"
 * (words whose length equals the sequence — the completed word) and "prefix"
 * (longer words the sequence could still become) — the caller simply calls it
 * again after each keypress with the growing sequence. Results are always
 * frequency-best-first; exact-length words, being common, naturally tend to
 * outrank longer completions.
 *
 * ## Honest no-match
 * When NO dictionary word matches, the engine reports zero candidates /
 * `NULL` — an explicit, testable state. It NEVER fabricates a word. The
 * composer is expected to fall back to multi-tap / literal entry in that case
 * (that fallback is the later UX slice's concern, not this module's).
 *
 * ## Purity / footprint
 * Pure C11, no I/O, no LVGL, ZERO heap allocation. The stateless query
 * functions touch only their arguments and the const dictionary. The optional
 * session struct (`ff_t9pred_session_t`) is a plain fixed-size value safe on
 * the stack or in a static; zero-initialise or call `ff_t9pred_session_reset`
 * before use. Queries are an O(dictionary) scan of a ~3.2k-word rodata blob
 * (see ff_t9dict.h) — microseconds on-device; no index structure to keep.
 */
#ifndef FF_T9PRED_H
#define FF_T9PRED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Max digits in one query / one word. Matches gen_t9dict.py's MAX_WORD_LEN;
 *  no dictionary word is longer, so a longer query can never match. */
#define FF_T9PRED_MAX_DIGITS 24

/* ------------------------------------------------------------------ */
/* Stateless queries                                                   */
/* ------------------------------------------------------------------ */

/**
 * ff_t9pred_match — candidates for a digit sequence, frequency-best-first.
 *
 * `digits` is `n` keys, each in the range 2..9 (letter keys). Writes up to
 * `max_out` candidate word pointers into `out` (pointers into the const
 * dictionary blob; valid for the life of the program, never freed) in
 * descending frequency order. Returns the number written.
 *
 * Returns 0 (and leaves `out` untouched) when there is no match, or on invalid
 * input: `digits`/`out` NULL, `n` == 0, `n` > FF_T9PRED_MAX_DIGITS, `max_out`
 * == 0, or any digit outside 2..9. A 0 return from valid input is the honest
 * "no dictionary word matches" state — not an error.
 *
 * Deterministic: identical inputs always yield identical output.
 */
size_t ff_t9pred_match(uint8_t const *digits, size_t n,
                       char const **out, size_t max_out);

/**
 * ff_t9pred_match_str — convenience wrapper taking the sequence as an ASCII
 * digit string, e.g. "4663". Any character outside '2'..'9' makes the whole
 * query invalid (returns 0), consistent with `ff_t9pred_match`. An empty
 * string returns 0.
 */
size_t ff_t9pred_match_str(char const *digits,
                           char const **out, size_t max_out);

/**
 * ff_t9pred_count — total number of dictionary words matching the sequence
 * (may exceed what a capped `ff_t9pred_match` returned). Lets a caller show a
 * "3 of 12" cycle indicator. Same validity rules as `ff_t9pred_match`; returns
 * 0 on no-match or invalid input.
 */
size_t ff_t9pred_count(uint8_t const *digits, size_t n);

/* ------------------------------------------------------------------ */
/* Supplementary word list — caller-supplied, ranked ABOVE the dict    */
/* ------------------------------------------------------------------ */
/*
 * The engine stays PURE: it knows nothing about festpacks, artists, or any
 * app concept. It only accepts an OPTIONAL array of extra NUL-terminated words
 * the CALLER supplies, and ranks any that match the digit sequence ABOVE its
 * own static dictionary. The caller owns the memory (the pointers and the
 * bytes they point at must outlive the call); the engine copies nothing.
 *
 * This is how the composer will fold festival vocabulary (artist / stage /
 * landmark names) in without core/t9pred depending on the festpack layer — the
 * festpack->words bridge lives OUTSIDE core (see firmware/festpack/
 * fp_t9words.h), collects the names, and hands them here as plain strings.
 *
 * Matching of extra words is the same prefix rule as the dictionary, and is
 * CASE-INSENSITIVE for ASCII letters, so real-world names ("Excision",
 * "NGHTMRE") match their lowercase digit sequence. A non-letter byte (space,
 * '&', digit, punctuation) is not a keypad letter, so it simply cannot be
 * matched: a query can predict a multi-word name only up to its first
 * non-letter ("Sullivan King" is a candidate for the keys of "sullivan").
 *
 * De-dup: extra words are de-duplicated against each other (case-insensitively,
 * first occurrence wins) and a dictionary word equal to a supplied extra word
 * is suppressed (the extra copy, at its higher rank, represents it once).
 */

/**
 * ff_t9pred_match_ex — like ff_t9pred_match, but ranks the `n_extra` words in
 * `extra` (each NUL-terminated) ABOVE the static dictionary. `extra` may be
 * NULL / `n_extra` <= 0, in which case this is byte-for-byte `ff_t9pred_match`.
 * Extra matches are emitted in supplied order, then dictionary matches in
 * frequency order, both subject to `max_out`. Returns the number written.
 */
size_t ff_t9pred_match_ex(uint8_t const *digits, size_t n,
                          char const **out, size_t max_out,
                          char const *const *extra, int n_extra);

/**
 * ff_t9pred_count_ex — total matches counting the supplementary list, with the
 * same de-dup semantics as `ff_t9pred_match_ex`. `extra` NULL / `n_extra` <= 0
 * degenerates to `ff_t9pred_count`.
 */
size_t ff_t9pred_count_ex(uint8_t const *digits, size_t n,
                          char const *const *extra, int n_extra);

/* ------------------------------------------------------------------ */
/* Optional stateful session — as-you-type entry + candidate cycling   */
/* ------------------------------------------------------------------ */

/**
 * A running predictive-entry session: the digit sequence typed so far plus the
 * currently-selected candidate (for "cycle to next word"). Plain value type,
 * no heap. Zero-initialise or call `ff_t9pred_session_reset`.
 */
typedef struct {
    uint8_t  digits[FF_T9PRED_MAX_DIGITS];
    uint8_t  n;    /* number of valid digits (0..FF_T9PRED_MAX_DIGITS) */
    uint16_t sel;  /* selected candidate index among current matches   */

    /* Optional supplementary word list (see ff_t9pred_match_ex). NULL / 0 when
     * none, in which case the session behaves exactly as the dictionary-only
     * engine. Caller-owned: the pointer and the words it references must
     * outlive the session. Bound via ff_t9pred_session_set_extra. */
    char const *const *extra;
    int                n_extra;
} ff_t9pred_session_t;

/** Clear a session to empty: no digits, selection 0, AND no supplementary list
 *  (extra unbound). Re-bind with ff_t9pred_session_set_extra after a reset if a
 *  supplementary list should persist across it. */
void ff_t9pred_session_reset(ff_t9pred_session_t *s);

/**
 * Bind (or clear, with `extra` NULL / `n_extra` <= 0) the session's optional
 * supplementary word list. The list is ranked above the static dictionary for
 * all subsequent session queries (current / candidates / cycle). No-op if `s`
 * is NULL. Does not change the typed digits, but re-anchors the selection to
 * the top candidate (rebinding changes the candidate set, so a stale selection
 * index could otherwise point past a now-smaller list until the next keypress).
 */
void ff_t9pred_session_set_extra(ff_t9pred_session_t *s,
                                 char const *const *extra, int n_extra);

/**
 * Append one letter key (2..9) to the session and reset the selection to the
 * top (most frequent) candidate. Returns false (session unchanged) if `s` is
 * NULL, `key` is not 2..9, or the sequence is already FF_T9PRED_MAX_DIGITS
 * long.
 */
bool ff_t9pred_session_key(ff_t9pred_session_t *s, uint8_t key);

/**
 * Remove the last digit and reset the selection to the top candidate. Returns
 * false (unchanged) if `s` is NULL or the sequence is already empty.
 */
bool ff_t9pred_session_backspace(ff_t9pred_session_t *s);

/**
 * Cycle to the next candidate for the current sequence (standard T9: same
 * digits, next-likelier word), wrapping from the last candidate back to the
 * first. A well-defined no-op when there are fewer than two candidates (0 or
 * 1: nothing to cycle) and when `s` is NULL.
 */
void ff_t9pred_session_cycle(ff_t9pred_session_t *s);

/**
 * The currently-selected candidate word for the session, or NULL when there
 * is no match (honest no-match) or `s` is NULL / empty. Pointer into the const
 * dictionary OR into the caller's supplementary list (whichever the selection
 * lands on); valid as long as that backing storage lives.
 */
char const *ff_t9pred_session_current(ff_t9pred_session_t const *s);

/**
 * Fill `out` with up to `max_out` candidates for the session's current
 * sequence, best-first (same as calling `ff_t9pred_match_ex` with the
 * session's digits and its bound supplementary list). Returns the number
 * written; 0 on no-match / empty / NULL.
 */
size_t ff_t9pred_session_candidates(ff_t9pred_session_t const *s,
                                    char const **out, size_t max_out);

/* ------------------------------------------------------------------ */
/* Integration sketch (NOT implemented here — later UX slice)          */
/* ------------------------------------------------------------------ */
/*
 * A future compose slice would swap multi-tap for predictive like so, with
 * NO change to this module:
 *
 *   ff_t9pred_session_t ps;  ff_t9_t t;   // t = the existing committed text
 *   ff_t9pred_session_reset(&ps);
 *
 *   on letter key k (2..9):
 *       if (!ff_t9pred_session_key(&ps, k)) { ...full/invalid... }
 *       char const *w = ff_t9pred_session_current(&ps);
 *       if (w) show_inline_prediction(w);        // live best guess
 *       else   fall_back_to_multitap(k);         // honest no-match path
 *
 *   on "next candidate" key:  ff_t9pred_session_cycle(&ps);
 *                             show_inline_prediction(ff_t9pred_session_current(&ps));
 *   on backspace:             ff_t9pred_session_backspace(&ps);
 *   on accept (space/select): ff_t9_insert_text(&t, ff_t9pred_session_current(&ps));
 *                             ff_t9_space(&t);  ff_t9pred_session_reset(&ps);
 *
 * The composer keeps owning the committed buffer (ff_t9); this engine only
 * ranks words. Deciding the exact keys, inline-vs-list UX, and the multi-tap
 * fallback trigger is the later maintainer-eyes decision, deliberately out of
 * scope here.
 */

#ifdef __cplusplus
}
#endif

#endif /* FF_T9PRED_H */
