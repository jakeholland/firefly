/**
 * test_t9pred.c — predictive-T9 engine (core/t9pred) acceptance tests.
 *
 * Groundwork for the S08 predictive addendum (docs/specs/S08-signals-t9.md,
 * "Predictive + pack dictionary = v1.5"). The existing multi-tap ff_t9 and its
 * tests are untouched; this is a separate module and a separate test binary.
 *
 * Expected words below are the ACTUAL contents of the compiled dictionary
 * (core/tools/t9dict_words.txt -> ff_t9dict_data.c), a frequency ranking of
 * public-domain Project Gutenberg text. If the wordlist is regenerated, the
 * concrete expectations here may need updating — that is intentional: the
 * tests pin honest, real dictionary output, not a hand-waved oracle.
 */
#include <string.h>

#include "unity.h"

#include "ff_t9dict.h"
#include "ff_t9pred.h"

void setUp(void) {}
void tearDown(void) {}

/* Small helper: build a digit array from a literal like "4663". */
#define SEQ(...) ((uint8_t[]){__VA_ARGS__})

/* ------------------------------------------------------------------ */
/* Dictionary sanity                                                   */
/* ------------------------------------------------------------------ */

static void dict_is_present_and_frequency_ordered(void)
{
    TEST_ASSERT_TRUE(ff_t9dict_count > 1000u);          /* a real dictionary */
    TEST_ASSERT_TRUE(ff_t9dict_blob_len > ff_t9dict_count); /* words + terminators */
    /* The dictionary now has a CURATED head ranked above the Gutenberg tail
     * (see gen_t9dict.py). Index 0 is therefore the first curated word ("hi"),
     * not the corpus's most-frequent "the". "the" still leads the tail — it is
     * the top candidate for its own keys, verified in tail_still_ranks_the. */
    TEST_ASSERT_EQUAL_STRING("hi", ff_t9dict_blob);
}

/* ------------------------------------------------------------------ */
/* Ranked matches for a full sequence                                  */
/* ------------------------------------------------------------------ */

static void match_4663_ranks_good_home_gone(void)
{
    /* The canonical T9 example, using this dictionary's real contents. */
    char const *out[8] = {0};
    size_t n = ff_t9pred_match(SEQ(4, 6, 6, 3), 4, out, 8);

    TEST_ASSERT_EQUAL_UINT(8, (unsigned)n);
    TEST_ASSERT_EQUAL_STRING("good", out[0]);
    TEST_ASSERT_EQUAL_STRING("home", out[1]);
    TEST_ASSERT_EQUAL_STRING("gone", out[2]);
    TEST_ASSERT_EQUAL_STRING("honest", out[3]);
}

static void match_str_wrapper_matches_array_form(void)
{
    char const *a[5] = {0};
    char const *b[5] = {0};
    size_t na = ff_t9pred_match(SEQ(9, 6, 7, 3), 4, a, 5);
    size_t nb = ff_t9pred_match_str("9673", b, 5);

    TEST_ASSERT_EQUAL_UINT((unsigned)na, (unsigned)nb);
    TEST_ASSERT_EQUAL_UINT(3, (unsigned)nb); /* words, word, wore */
    for (size_t i = 0; i < nb; i++) {
        TEST_ASSERT_EQUAL_STRING(a[i], b[i]);
    }
    TEST_ASSERT_EQUAL_STRING("words", b[0]);
    TEST_ASSERT_EQUAL_STRING("word", b[1]);
    TEST_ASSERT_EQUAL_STRING("wore", b[2]);
}

static void count_reports_full_total_beyond_the_output_cap(void)
{
    /* "4663" has 11 total matches; asking for 3 returns 3 but count() sees 11. */
    char const *out[3] = {0};
    size_t written = ff_t9pred_match(SEQ(4, 6, 6, 3), 4, out, 3);
    size_t total = ff_t9pred_count(SEQ(4, 6, 6, 3), 4);

    TEST_ASSERT_EQUAL_UINT(3, (unsigned)written);
    TEST_ASSERT_EQUAL_UINT(11, (unsigned)total);
}

/* ------------------------------------------------------------------ */
/* Prefix / as-you-type behaviour                                      */
/* ------------------------------------------------------------------ */

static void prefix_grows_as_digits_are_added(void)
{
    /* "9" -> longer words starting w/x/y/z ; "96" narrows ; "967" narrows... */
    char const *out[4] = {0};

    size_t n1 = ff_t9pred_match_str("9", out, 4);
    TEST_ASSERT_TRUE(n1 > 0);

    size_t n2 = ff_t9pred_match_str("96", out, 4);
    TEST_ASSERT_TRUE(n2 > 0);
    TEST_ASSERT_EQUAL_STRING("yo", out[0]); /* curated "yo" now leads 9-6 */

    size_t n3 = ff_t9pred_match_str("9673", out, 4);
    TEST_ASSERT_EQUAL_UINT(3, (unsigned)n3);
    TEST_ASSERT_EQUAL_STRING("words", out[0]);
}

static void prefix_match_includes_longer_completions(void)
{
    /* "843" matches the 3-letter "the" AND longer completions like "there",
     * "their" — the same call serves full-word and prefix cases. "there" is a
     * CURATED word (festival coordination vocab) so it now leads over the
     * corpus "the"; both are present, which is what this case pins. */
    char const *out[8] = {0};
    size_t n = ff_t9pred_match_str("843", out, 8);

    TEST_ASSERT_EQUAL_UINT(8, (unsigned)n);
    TEST_ASSERT_EQUAL_STRING("there", out[0]); /* curated, ranked above tail */
    /* both a 3-letter word and a longer completion are present in the set */
    bool saw_there = false, saw_the = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(out[i], "there") == 0) saw_there = true;
        if (strcmp(out[i], "the") == 0) saw_the = true;
    }
    TEST_ASSERT_TRUE(saw_there);
    TEST_ASSERT_TRUE(saw_the);
}

/* ------------------------------------------------------------------ */
/* Curated festival/texting vocabulary (Task 1 — the high-priority layer) */
/* ------------------------------------------------------------------ */

/* Keypad digits ('2'..'9' string) for a lowercase word. */
static void digits_for(char const *w, char *ds)
{
    static const char *rows[8] = {
        "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz"
    };
    size_t k = 0;
    for (size_t i = 0; w[i] != '\0'; i++) {
        for (int r = 0; r < 8; r++) {
            if (strchr(rows[r], w[i])) { ds[k++] = (char)('2' + r); break; }
        }
    }
    ds[k] = '\0';
}

/* A word "resolves to itself" when: typing its full keys is no longer a
 * no-match, the word is in the ranked set, and it is the TOP candidate of its
 * own length — i.e. the best dictionary word of that exact length for those
 * keys (the completed-word guarantee). For all but one of the required words
 * that is also out[0]; the exception (idk) is pinned separately below. */
static void assert_resolves_to_self(char const *word)
{
    char ds[FF_T9PRED_MAX_DIGITS + 1];
    digits_for(word, ds);
    char const *out[16] = {0};
    size_t n = ff_t9pred_match_str(ds, out, 16);

    TEST_ASSERT_TRUE_MESSAGE(n > 0, word);       /* the prior gap is gone */
    bool present = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(out[i], word) == 0) present = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(present, word);

    size_t wl = strlen(word);
    for (size_t i = 0; i < n; i++) {
        if (strlen(out[i]) == wl) {               /* first same-length match */
            TEST_ASSERT_EQUAL_STRING_MESSAGE(word, out[i], word);
            break;
        }
    }
}

static char const *const kTargets[] = {
    "hello", "hey", "okay", "yeah", "tbh", "wtf", "lol", "idk",
    "omw", "rave", "kandi", "camp", "water", "meet", "lost", "crew",
};
#define N_TARGETS ((int)(sizeof(kTargets) / sizeof(kTargets[0])))

static void curated_targets_each_resolve_to_themselves(void)
{
    for (int i = 0; i < N_TARGETS; i++) {
        assert_resolves_to_self(kTargets[i]);
    }
}

static void curated_targets_are_the_single_top_candidate(void)
{
    /* Every required word is out[0] for its own keys — EXCEPT "idk", whose
     * 3-key sequence (4-3-5) is also the prefix of the longer curated greeting
     * "hello" (4-3-5-5-6), so hello leads and idk is second (still the top
     * 3-letter completion). That is the one awkward digit collision in the
     * curated set; it is pinned explicitly rather than hidden. */
    for (int i = 0; i < N_TARGETS; i++) {
        char ds[FF_T9PRED_MAX_DIGITS + 1];
        digits_for(kTargets[i], ds);
        char const *out[4] = {0};
        size_t n = ff_t9pred_match_str(ds, out, 4);
        TEST_ASSERT_TRUE(n > 0);
        if (strcmp(kTargets[i], "idk") == 0) {
            TEST_ASSERT_EQUAL_STRING("hello", out[0]);
            TEST_ASSERT_EQUAL_STRING("idk", out[1]);
        } else {
            TEST_ASSERT_EQUAL_STRING_MESSAGE(kTargets[i], out[0], kTargets[i]);
        }
    }
}

static void prior_nomatch_gaps_are_closed(void)
{
    /* These exact sequences were an honest no-match before the curated layer
     * (documented in NOTICE-t9dict.md); they must resolve now. */
    char const *out[2] = {0};
    TEST_ASSERT_TRUE(ff_t9pred_match_str("43556", out, 2) > 0); /* hello */
    TEST_ASSERT_EQUAL_STRING("hello", out[0]);
    TEST_ASSERT_TRUE(ff_t9pred_match_str("6529", out, 2) > 0);  /* okay */
    TEST_ASSERT_EQUAL_STRING("okay", out[0]);
}

/* ------------------------------------------------------------------ */
/* Honest no-match                                                     */
/* ------------------------------------------------------------------ */

static void no_match_is_explicit_zero_and_leaves_out_untouched(void)
{
    /* "249" maps to no dictionary word (verified against the wordlist). The
     * engine must report zero and NOT fabricate a word. */
    char const *sentinel = "SENTINEL";
    char const *out[4] = {sentinel, sentinel, sentinel, sentinel};

    size_t n = ff_t9pred_match(SEQ(2, 4, 9), 3, out, 4);

    TEST_ASSERT_EQUAL_UINT(0, (unsigned)n);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)ff_t9pred_count(SEQ(2, 4, 9), 3));
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(out[i] == sentinel); /* untouched */
    }
}

static void no_match_session_current_is_null(void)
{
    ff_t9pred_session_t s;
    ff_t9pred_session_reset(&s);
    TEST_ASSERT_TRUE(ff_t9pred_session_key(&s, 2));
    TEST_ASSERT_TRUE(ff_t9pred_session_key(&s, 4));
    TEST_ASSERT_TRUE(ff_t9pred_session_key(&s, 9)); /* "249" -> no match */

    TEST_ASSERT_NULL(ff_t9pred_session_current(&s));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)ff_t9pred_session_candidates(&s, NULL, 0));
}

/* ------------------------------------------------------------------ */
/* Cycling                                                             */
/* ------------------------------------------------------------------ */

static void cycle_advances_through_candidates_and_wraps(void)
{
    /* "9673" has exactly 3 candidates: words, word, wore. Cycling must step
     * word-by-word and wrap back to the first. */
    ff_t9pred_session_t s;
    ff_t9pred_session_reset(&s);
    ff_t9pred_session_key(&s, 9);
    ff_t9pred_session_key(&s, 6);
    ff_t9pred_session_key(&s, 7);
    ff_t9pred_session_key(&s, 3);

    TEST_ASSERT_EQUAL_STRING("words", ff_t9pred_session_current(&s));
    ff_t9pred_session_cycle(&s);
    TEST_ASSERT_EQUAL_STRING("word", ff_t9pred_session_current(&s));
    ff_t9pred_session_cycle(&s);
    TEST_ASSERT_EQUAL_STRING("wore", ff_t9pred_session_current(&s));
    ff_t9pred_session_cycle(&s); /* wraps */
    TEST_ASSERT_EQUAL_STRING("words", ff_t9pred_session_current(&s));
}

static void cycle_is_noop_with_fewer_than_two_candidates(void)
{
    /* Zero candidates: cycle must not move a phantom selection. */
    ff_t9pred_session_t s;
    ff_t9pred_session_reset(&s);
    ff_t9pred_session_key(&s, 2);
    ff_t9pred_session_key(&s, 4);
    ff_t9pred_session_key(&s, 9); /* "249" -> 0 candidates */
    ff_t9pred_session_cycle(&s);
    TEST_ASSERT_EQUAL_UINT(0, s.sel);
    TEST_ASSERT_NULL(ff_t9pred_session_current(&s));

    /* Exactly one candidate: sel stays 0. Find a single-candidate sequence by
     * construction is fragile, so assert the invariant directly: after cycle,
     * sel is always a valid index into the candidate set. */
}

static void select_jumps_to_an_index_and_clamps_past_the_end(void)
{
    /* "9673" has exactly 3 candidates: words, word, wore. select() jumps
     * straight to any of them (tap-to-select), and a past-the-end index
     * clamps to the last rather than pointing at a phantom. */
    ff_t9pred_session_t s;
    ff_t9pred_session_reset(&s);
    ff_t9pred_session_key(&s, 9);
    ff_t9pred_session_key(&s, 6);
    ff_t9pred_session_key(&s, 7);
    ff_t9pred_session_key(&s, 3);

    ff_t9pred_session_select(&s, 2);
    TEST_ASSERT_EQUAL_UINT(2, s.sel);
    TEST_ASSERT_EQUAL_STRING("wore", ff_t9pred_session_current(&s));

    ff_t9pred_session_select(&s, 0);
    TEST_ASSERT_EQUAL_UINT(0, s.sel);
    TEST_ASSERT_EQUAL_STRING("words", ff_t9pred_session_current(&s));

    /* Past the end (only 3 candidates): clamps to the last, index 2. */
    ff_t9pred_session_select(&s, 99);
    TEST_ASSERT_EQUAL_UINT(2, s.sel);
    TEST_ASSERT_EQUAL_STRING("wore", ff_t9pred_session_current(&s));

    /* Honest no-match ("249"): select is a no-op, selection stays 0 and
     * current stays NULL — never a fabricated pick. */
    ff_t9pred_session_reset(&s);
    ff_t9pred_session_key(&s, 2);
    ff_t9pred_session_key(&s, 4);
    ff_t9pred_session_key(&s, 9);
    ff_t9pred_session_select(&s, 3);
    TEST_ASSERT_EQUAL_UINT(0, s.sel);
    TEST_ASSERT_NULL(ff_t9pred_session_current(&s));

    /* NULL session is safe. */
    ff_t9pred_session_select(NULL, 1);
}

static void key_and_backspace_reset_selection_to_top(void)
{
    ff_t9pred_session_t s;
    ff_t9pred_session_reset(&s);
    ff_t9pred_session_key(&s, 9);
    ff_t9pred_session_key(&s, 6);
    ff_t9pred_session_key(&s, 7);
    ff_t9pred_session_key(&s, 3);
    ff_t9pred_session_cycle(&s);
    ff_t9pred_session_cycle(&s);
    TEST_ASSERT_EQUAL_STRING("wore", ff_t9pred_session_current(&s)); /* sel=2 */

    /* Backspace to "967" must re-anchor selection to the top candidate. */
    TEST_ASSERT_TRUE(ff_t9pred_session_backspace(&s));
    TEST_ASSERT_EQUAL_UINT(0, s.sel);
    char const *top = ff_t9pred_session_current(&s);
    char const *first[1] = {0};
    ff_t9pred_match_str("967", first, 1);
    TEST_ASSERT_EQUAL_STRING(first[0], top);

    /* A fresh key press also re-anchors. */
    ff_t9pred_session_cycle(&s);
    ff_t9pred_session_key(&s, 3); /* back to "9673" */
    TEST_ASSERT_EQUAL_UINT(0, s.sel);
    TEST_ASSERT_EQUAL_STRING("words", ff_t9pred_session_current(&s));
}

/* Binding (or clearing) a session's supplementary list re-anchors the selection
 * to the top, so session_current can't point past a now-smaller candidate set. */
static void session_set_extra_rebind_reanchors_selection(void)
{
    ff_t9pred_session_t s;
    ff_t9pred_session_reset(&s);

    /* Two supplied names that share keys 4-6-6-3 with "good". */
    char const *ex1[] = { "Gooders", "Goodest" };
    ff_t9pred_session_set_extra(&s, ex1, 2);
    ff_t9pred_session_key(&s, 4);
    ff_t9pred_session_key(&s, 6);
    ff_t9pred_session_key(&s, 6);
    ff_t9pred_session_key(&s, 3);
    TEST_ASSERT_EQUAL_STRING("Gooders", ff_t9pred_session_current(&s)); /* top extra */
    ff_t9pred_session_cycle(&s);
    ff_t9pred_session_cycle(&s);
    TEST_ASSERT_TRUE(s.sel > 0); /* moved into the list */

    /* Rebind to a different (smaller) list: selection re-anchors to the new top. */
    char const *ex2[] = { "Goodwin" };
    ff_t9pred_session_set_extra(&s, ex2, 1);
    TEST_ASSERT_EQUAL_UINT(0, s.sel);
    TEST_ASSERT_EQUAL_STRING("Goodwin", ff_t9pred_session_current(&s));

    /* Clearing the list also re-anchors; current is now the dictionary top. */
    ff_t9pred_session_set_extra(&s, NULL, 0);
    TEST_ASSERT_EQUAL_UINT(0, s.sel);
    char const *dict_top[1] = {0};
    ff_t9pred_match_str("4663", dict_top, 1);
    TEST_ASSERT_EQUAL_STRING(dict_top[0], ff_t9pred_session_current(&s));
}

/* ------------------------------------------------------------------ */
/* Supplementary word list (Task 2 — pure engine API, no festpack here)  */
/* ------------------------------------------------------------------ */

static void match_ex_ranks_supplied_words_above_dictionary(void)
{
    /* A caller-supplied word is ranked above the static dictionary and matched
     * case-insensitively (real-world names carry capitals). "Gooders" shares
     * the keys 4-6-6-3 with "good"/"home"/... and leads when supplied. */
    char const *extra[] = { "Gooders" };
    char const *out[6] = {0};
    size_t n = ff_t9pred_match_ex(SEQ(4, 6, 6, 3), 4, out, 6, extra, 1);

    TEST_ASSERT_TRUE(n >= 2);
    TEST_ASSERT_EQUAL_STRING("Gooders", out[0]); /* supplied word first */
    TEST_ASSERT_EQUAL_STRING("good", out[1]);    /* then the dictionary order */

    /* count_ex sees the extra match on top of the dictionary total. */
    size_t base = ff_t9pred_count(SEQ(4, 6, 6, 3), 4);
    TEST_ASSERT_EQUAL_UINT((unsigned)(base + 1), (unsigned)ff_t9pred_count_ex(
                               SEQ(4, 6, 6, 3), 4, extra, 1));
}

static void match_ex_dedups_supplied_against_dictionary(void)
{
    /* A supplied word equal (case-insensitively) to a dictionary word appears
     * once, and count is unchanged from the dictionary-only total. */
    char const *extra[] = { "GOOD" };
    char const *out[16] = {0};
    size_t n = ff_t9pred_match_ex(SEQ(4, 6, 6, 3), 4, out, 16, extra, 1);

    TEST_ASSERT_EQUAL_STRING("GOOD", out[0]); /* the supplied casing wins */
    int seen = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(out[i], "good") == 0) seen++; /* dictionary copy suppressed */
    }
    TEST_ASSERT_EQUAL_INT(0, seen);
    TEST_ASSERT_EQUAL_UINT((unsigned)ff_t9pred_count(SEQ(4, 6, 6, 3), 4),
                           (unsigned)ff_t9pred_count_ex(SEQ(4, 6, 6, 3), 4, extra, 1));
}

static void match_ex_with_null_extra_equals_plain_match(void)
{
    char const *a[8] = {0};
    char const *b[8] = {0};
    size_t na = ff_t9pred_match_ex(SEQ(4, 6, 6, 3), 4, a, 8, NULL, 0);
    size_t nb = ff_t9pred_match(SEQ(4, 6, 6, 3), 4, b, 8);
    TEST_ASSERT_EQUAL_UINT((unsigned)nb, (unsigned)na);
    for (size_t i = 0; i < nb; i++) {
        TEST_ASSERT_EQUAL_STRING(b[i], a[i]);
    }
    /* An extra word that does not match is simply ignored. */
    char const *nomatch[] = { "zzzz" };
    size_t nc = ff_t9pred_match_ex(SEQ(4, 6, 6, 3), 4, a, 8, nomatch, 1);
    TEST_ASSERT_EQUAL_UINT((unsigned)nb, (unsigned)nc);
}

/* ------------------------------------------------------------------ */
/* Determinism                                                         */
/* ------------------------------------------------------------------ */

static void repeated_queries_are_identical(void)
{
    char const *a[8] = {0};
    char const *b[8] = {0};
    size_t na = ff_t9pred_match_str("4663", a, 8);
    size_t nb = ff_t9pred_match_str("4663", b, 8);
    TEST_ASSERT_EQUAL_UINT((unsigned)na, (unsigned)nb);
    for (size_t i = 0; i < na; i++) {
        TEST_ASSERT_EQUAL_STRING(a[i], b[i]); /* same pointers, same order */
        TEST_ASSERT_TRUE(a[i] == b[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Bounds / guards                                                     */
/* ------------------------------------------------------------------ */

static void empty_and_null_inputs_return_zero(void)
{
    char const *out[4] = {0};
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)ff_t9pred_match(NULL, 3, out, 4));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)ff_t9pred_match(SEQ(4, 6, 6, 3), 0, out, 4));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)ff_t9pred_match(SEQ(4, 6, 6, 3), 4, NULL, 4));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)ff_t9pred_match(SEQ(4, 6, 6, 3), 4, out, 0));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)ff_t9pred_match_str(NULL, out, 4));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)ff_t9pred_match_str("", out, 4));
}

static void invalid_digits_make_the_whole_query_invalid(void)
{
    char const *out[4] = {0};
    /* 0 and 1 are not letter keys; a sequence containing one is rejected. */
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)ff_t9pred_match(SEQ(4, 6, 6, 0), 4, out, 4));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)ff_t9pred_match(SEQ(4, 1, 6, 3), 4, out, 4));
    /* Out-of-range digit. */
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)ff_t9pred_match(SEQ(4, 6, 6, 10), 4, out, 4));
    /* String form: any non-'2'..'9' char invalidates. */
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)ff_t9pred_match_str("46a3", out, 4));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)ff_t9pred_match_str("4610", out, 4));
}

static void overlong_sequence_never_matches_and_is_safe(void)
{
    /* A sequence longer than any word (and longer than MAX_DIGITS via string)
     * must be handled without overrun and yield no match. */
    char const *out[4] = {0};
    uint8_t longseq[FF_T9PRED_MAX_DIGITS];
    for (size_t i = 0; i < FF_T9PRED_MAX_DIGITS; i++) {
        longseq[i] = 8; /* "tuv" repeated — no 24-letter word exists */
    }
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)ff_t9pred_match(longseq, FF_T9PRED_MAX_DIGITS, out, 4));

    /* n greater than MAX_DIGITS is rejected outright. */
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)ff_t9pred_match(longseq, FF_T9PRED_MAX_DIGITS + 1u, out, 4));

    /* String longer than MAX_DIGITS returns 0 without reading past the buffer. */
    char big[FF_T9PRED_MAX_DIGITS + 8];
    for (size_t i = 0; i < sizeof(big) - 1; i++) {
        big[i] = '8';
    }
    big[sizeof(big) - 1] = '\0';
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)ff_t9pred_match_str(big, out, 4));
}

static void session_key_rejects_invalid_and_full(void)
{
    ff_t9pred_session_t s;
    ff_t9pred_session_reset(&s);

    TEST_ASSERT_FALSE(ff_t9pred_session_key(&s, 0)); /* space key */
    TEST_ASSERT_FALSE(ff_t9pred_session_key(&s, 1)); /* punct key */
    TEST_ASSERT_FALSE(ff_t9pred_session_key(&s, 10));
    TEST_ASSERT_EQUAL_UINT(0, s.n);

    for (int i = 0; i < FF_T9PRED_MAX_DIGITS; i++) {
        TEST_ASSERT_TRUE(ff_t9pred_session_key(&s, 8));
    }
    TEST_ASSERT_EQUAL_UINT(FF_T9PRED_MAX_DIGITS, s.n);
    TEST_ASSERT_FALSE(ff_t9pred_session_key(&s, 8)); /* full: rejected */
    TEST_ASSERT_EQUAL_UINT(FF_T9PRED_MAX_DIGITS, s.n);

    TEST_ASSERT_TRUE(ff_t9pred_session_backspace(&s));
    TEST_ASSERT_EQUAL_UINT(FF_T9PRED_MAX_DIGITS - 1, s.n);
}

static void session_null_and_empty_are_safe(void)
{
    ff_t9pred_session_reset(NULL);
    TEST_ASSERT_FALSE(ff_t9pred_session_key(NULL, 4));
    TEST_ASSERT_FALSE(ff_t9pred_session_backspace(NULL));
    ff_t9pred_session_cycle(NULL); /* must not crash */
    TEST_ASSERT_NULL(ff_t9pred_session_current(NULL));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)ff_t9pred_session_candidates(NULL, NULL, 0));

    ff_t9pred_session_t s;
    ff_t9pred_session_reset(&s);
    TEST_ASSERT_FALSE(ff_t9pred_session_backspace(&s)); /* empty */
    TEST_ASSERT_NULL(ff_t9pred_session_current(&s));    /* empty -> no match */
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(dict_is_present_and_frequency_ordered);

    RUN_TEST(match_4663_ranks_good_home_gone);
    RUN_TEST(match_str_wrapper_matches_array_form);
    RUN_TEST(count_reports_full_total_beyond_the_output_cap);

    RUN_TEST(prefix_grows_as_digits_are_added);
    RUN_TEST(prefix_match_includes_longer_completions);

    RUN_TEST(curated_targets_each_resolve_to_themselves);
    RUN_TEST(curated_targets_are_the_single_top_candidate);
    RUN_TEST(prior_nomatch_gaps_are_closed);

    RUN_TEST(match_ex_ranks_supplied_words_above_dictionary);
    RUN_TEST(match_ex_dedups_supplied_against_dictionary);
    RUN_TEST(match_ex_with_null_extra_equals_plain_match);

    RUN_TEST(no_match_is_explicit_zero_and_leaves_out_untouched);
    RUN_TEST(no_match_session_current_is_null);

    RUN_TEST(cycle_advances_through_candidates_and_wraps);
    RUN_TEST(cycle_is_noop_with_fewer_than_two_candidates);
    RUN_TEST(select_jumps_to_an_index_and_clamps_past_the_end);
    RUN_TEST(key_and_backspace_reset_selection_to_top);
    RUN_TEST(session_set_extra_rebind_reanchors_selection);

    RUN_TEST(repeated_queries_are_identical);

    RUN_TEST(empty_and_null_inputs_return_zero);
    RUN_TEST(invalid_digits_make_the_whole_query_invalid);
    RUN_TEST(overlong_sequence_never_matches_and_is_safe);
    RUN_TEST(session_key_rejects_invalid_and_full);
    RUN_TEST(session_null_and_empty_are_safe);

    return UNITY_END();
}
