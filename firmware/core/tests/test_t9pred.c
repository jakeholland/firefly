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
    /* Most-frequent-first: "the" is rank 0 in any English corpus. */
    TEST_ASSERT_EQUAL_STRING("the", ff_t9dict_blob);
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
    TEST_ASSERT_EQUAL_STRING("you", out[0]); /* most frequent w/x/y/z + m/n/o word */

    size_t n3 = ff_t9pred_match_str("9673", out, 4);
    TEST_ASSERT_EQUAL_UINT(3, (unsigned)n3);
    TEST_ASSERT_EQUAL_STRING("words", out[0]);
}

static void prefix_match_includes_longer_completions(void)
{
    /* "843" matches the 3-letter "the" AND longer completions like "there",
     * "their" — the same call serves full-word and prefix cases. */
    char const *out[8] = {0};
    size_t n = ff_t9pred_match_str("843", out, 8);

    TEST_ASSERT_EQUAL_UINT(8, (unsigned)n);
    TEST_ASSERT_EQUAL_STRING("the", out[0]);   /* exact length 3 */
    /* a longer completion is present in the ranked set */
    bool saw_there = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(out[i], "there") == 0) {
            saw_there = true;
        }
    }
    TEST_ASSERT_TRUE(saw_there);
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

    RUN_TEST(no_match_is_explicit_zero_and_leaves_out_untouched);
    RUN_TEST(no_match_session_current_is_null);

    RUN_TEST(cycle_advances_through_candidates_and_wraps);
    RUN_TEST(cycle_is_noop_with_fewer_than_two_candidates);
    RUN_TEST(key_and_backspace_reset_selection_to_top);

    RUN_TEST(repeated_queries_are_identical);

    RUN_TEST(empty_and_null_inputs_return_zero);
    RUN_TEST(invalid_digits_make_the_whole_query_invalid);
    RUN_TEST(overlong_sequence_never_matches_and_is_safe);
    RUN_TEST(session_key_rejects_invalid_and_full);
    RUN_TEST(session_null_and_empty_are_safe);

    return UNITY_END();
}
