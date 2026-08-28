/**
 * test_t9words.c — festpack -> predictive-T9 bridge (fp_t9words) + the pure
 * engine's supplementary-word API (ff_t9pred_match_ex / session extra).
 *
 * The bridge (firmware/festpack/fp_t9words.c) turns a loaded fp_pack_t's
 * artist/stage/landmark names into the caller-supplied "extra" word array that
 * core/t9pred ranks above its static dictionary — WITHOUT core/t9pred ever
 * seeing fp_pack_t (see fp_t9words.h's placement note). This test builds a
 * small synthetic pack with real-style artist names (Lost Lands / Bass Canyon
 * fixtures: Excision, Sullivan King, NGHTMRE, Caspa) and drives both halves.
 */
#include <string.h>

#include "unity.h"

#include "fp_pack.h"
#include "fp_t9words.h"
#include "ff_t9pred.h"

void setUp(void) {}
void tearDown(void) {}

/* Map a lowercase/uppercase word to its keypad digit sequence, so tests can
 * say "digits for this word" without hand-encoding. Mirrors the engine map. */
static size_t seq_of(char const *w, uint8_t *out)
{
    static const char *rows[8] = {
        "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz"
    };
    size_t n = 0;
    for (size_t i = 0; w[i] != '\0'; i++) {
        char c = w[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        for (int r = 0; r < 8; r++) {
            if (strchr(rows[r], c)) {
                out[n++] = (uint8_t)(r + 2);
                break;
            }
        }
    }
    return n;
}

/* Build a synthetic pack: a few sets (artists), a stage, a landmark. */
static void make_pack(fp_pack_t *p)
{
    memset(p, 0, sizeof(*p));
    strcpy(p->sets[0].artist, "Excision");
    strcpy(p->sets[1].artist, "Sullivan King");
    strcpy(p->sets[2].artist, "NGHTMRE");
    strcpy(p->sets[3].artist, "Caspa");
    p->n_sets = 4;
    strcpy(p->stages[0].name, "Wompy Woods");
    p->n_stages = 1;
    strcpy(p->landmarks[0].name, "Medical Tent");
    p->n_landmarks = 1;
}

/* Is `needle` among the first `n` candidates in `out`? */
static bool has_candidate(char const **out, size_t n, char const *needle)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(out[i], needle) == 0) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Bridge: collect                                                     */
/* ------------------------------------------------------------------ */

static void collect_gathers_artists_stages_landmarks_in_order(void)
{
    fp_pack_t p;
    make_pack(&p);
    char const *w[16] = {0};
    int n = fp_t9words_collect(&p, w, 16);

    /* 4 artists + 1 stage + 1 landmark = 6, artists first. */
    TEST_ASSERT_EQUAL_INT(6, n);
    TEST_ASSERT_EQUAL_STRING("Excision", w[0]);
    TEST_ASSERT_EQUAL_STRING("Sullivan King", w[1]);
    TEST_ASSERT_EQUAL_STRING("NGHTMRE", w[2]);
    TEST_ASSERT_EQUAL_STRING("Caspa", w[3]);
    TEST_ASSERT_EQUAL_STRING("Wompy Woods", w[4]);
    TEST_ASSERT_EQUAL_STRING("Medical Tent", w[5]);
}

static void collect_skips_empty_and_dedups_case_insensitively(void)
{
    fp_pack_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.sets[0].artist, "Excision");
    strcpy(p.sets[1].artist, "");          /* empty -> skipped */
    strcpy(p.sets[2].artist, "EXCISION");  /* case-dup -> skipped */
    strcpy(p.sets[3].artist, "Caspa");
    p.n_sets = 4;
    char const *w[16] = {0};
    int n = fp_t9words_collect(&p, w, 16);

    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING("Excision", w[0]);
    TEST_ASSERT_EQUAL_STRING("Caspa", w[1]);
}

static void collect_respects_max_and_null_pack(void)
{
    fp_pack_t p;
    make_pack(&p);
    char const *w[2] = {0};
    TEST_ASSERT_EQUAL_INT(2, fp_t9words_collect(&p, w, 2)); /* capped */
    TEST_ASSERT_EQUAL_STRING("Excision", w[0]);
    TEST_ASSERT_EQUAL_STRING("Sullivan King", w[1]);

    TEST_ASSERT_EQUAL_INT(0, fp_t9words_collect(NULL, w, 2));
    TEST_ASSERT_EQUAL_INT(0, fp_t9words_collect(&p, NULL, 2));
    TEST_ASSERT_EQUAL_INT(0, fp_t9words_collect(&p, w, 0));
}

static void collect_handles_max_length_name(void)
{
    /* fp_copy_str truncates+NUL-terminates every name to its buffer; a name
     * that exactly fills artist[32] (31 chars + NUL) must still be a safe,
     * bounded C string the bridge and engine can walk. */
    fp_pack_t p;
    memset(&p, 0, sizeof(p));
    /* 31 'a's — one short of the 32-byte buffer, leaving room for the NUL. */
    memset(p.sets[0].artist, 'a', 31);
    p.sets[0].artist[31] = '\0';
    p.n_sets = 1;
    char const *w[4] = {0};
    int n = fp_t9words_collect(&p, w, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_UINT(31, (unsigned)strlen(w[0]));
}

/* ------------------------------------------------------------------ */
/* Engine: match_ex ranks extra above the dictionary                   */
/* ------------------------------------------------------------------ */

static void typing_an_artist_returns_that_artist(void)
{
    fp_pack_t p;
    make_pack(&p);
    char const *extra[16] = {0};
    int ne = fp_t9words_collect(&p, extra, 16);

    /* Type the full keys for "excision" -> the artist is a candidate. */
    uint8_t seq[FF_T9PRED_MAX_DIGITS];
    size_t n = seq_of("excision", seq);
    char const *out[8] = {0};
    size_t got = ff_t9pred_match_ex(seq, n, out, 8, extra, ne);
    TEST_ASSERT_TRUE(got >= 1);
    TEST_ASSERT_EQUAL_STRING("Excision", out[0]); /* nothing else matches */

    /* A multi-word artist is predicted up to its first non-letter: the keys of
     * "sullivan" surface "Sullivan King" (the space stops further keys). */
    n = seq_of("sullivan", seq);
    got = ff_t9pred_match_ex(seq, n, out, 8, extra, ne);
    TEST_ASSERT_TRUE(has_candidate(out, got, "Sullivan King"));
}

static void extra_outranks_a_colliding_dictionary_word(void)
{
    /* "Caspa" shares its first three keys (2-2-7) with common dictionary words
     * like "care"/"cart". With the pack supplied, the artist ranks first; with
     * no pack, a dictionary word leads and the engine is unchanged. */
    fp_pack_t p;
    make_pack(&p);
    char const *extra[16] = {0};
    int ne = fp_t9words_collect(&p, extra, 16);

    uint8_t seq[FF_T9PRED_MAX_DIGITS];
    size_t n = seq_of("cas", seq); /* 2-2-7 */

    char const *with[8] = {0};
    size_t nw = ff_t9pred_match_ex(seq, n, with, 8, extra, ne);
    TEST_ASSERT_TRUE(nw >= 2);
    TEST_ASSERT_EQUAL_STRING("Caspa", with[0]); /* extra ranked above dict */

    char const *without[8] = {0};
    size_t no = ff_t9pred_match(seq, n, without, 8);
    TEST_ASSERT_TRUE(no >= 1);
    TEST_ASSERT_TRUE(strcmp(without[0], "Caspa") != 0); /* a dict word leads */

    /* The dictionary matches still follow the artist, in the same order the
     * plain engine produced them (extra is purely additive on top). */
    TEST_ASSERT_EQUAL_STRING(without[0], with[1]);
}

static void empty_or_absent_pack_leaves_engine_unchanged(void)
{
    /* No pack -> collect yields nothing -> match_ex == match. */
    fp_pack_t p;
    memset(&p, 0, sizeof(p));
    char const *extra[8] = {0};
    int ne = fp_t9words_collect(&p, extra, 8);
    TEST_ASSERT_EQUAL_INT(0, ne);

    uint8_t seq[FF_T9PRED_MAX_DIGITS];
    size_t n = seq_of("good", seq);
    char const *a[8] = {0};
    char const *b[8] = {0};
    size_t na = ff_t9pred_match_ex(seq, n, a, 8, extra, ne);
    size_t nb = ff_t9pred_match(seq, n, b, 8);
    TEST_ASSERT_EQUAL_UINT((unsigned)nb, (unsigned)na);
    for (size_t i = 0; i < nb; i++) {
        TEST_ASSERT_EQUAL_STRING(b[i], a[i]);
    }
    /* NULL extra list behaves identically. */
    size_t nc = ff_t9pred_match_ex(seq, n, a, 8, NULL, 0);
    TEST_ASSERT_EQUAL_UINT((unsigned)nb, (unsigned)nc);
}

static void extra_deduped_against_dictionary_word(void)
{
    /* An artist that IS a dictionary word ("water") must appear once, not
     * twice: the extra copy (higher rank) suppresses the dictionary one, and
     * count_ex equals the dictionary-only count. */
    fp_pack_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.sets[0].artist, "water");
    p.n_sets = 1;
    char const *extra[4] = {0};
    int ne = fp_t9words_collect(&p, extra, 4);
    TEST_ASSERT_EQUAL_INT(1, ne);

    uint8_t seq[FF_T9PRED_MAX_DIGITS];
    size_t n = seq_of("water", seq);
    TEST_ASSERT_EQUAL_UINT((unsigned)ff_t9pred_count(seq, n),
                           (unsigned)ff_t9pred_count_ex(seq, n, extra, ne));

    char const *out[8] = {0};
    size_t got = ff_t9pred_match_ex(seq, n, out, 8, extra, ne);
    int seen = 0;
    for (size_t i = 0; i < got; i++) {
        if (strcmp(out[i], "water") == 0) seen++;
    }
    TEST_ASSERT_EQUAL_INT(1, seen);
}

/* ------------------------------------------------------------------ */
/* Session variant carrying the extra list                             */
/* ------------------------------------------------------------------ */

static void session_with_extra_predicts_and_cycles(void)
{
    fp_pack_t p;
    make_pack(&p);
    char const *extra[16] = {0};
    int ne = fp_t9words_collect(&p, extra, 16);

    ff_t9pred_session_t s;
    ff_t9pred_session_reset(&s);
    ff_t9pred_session_set_extra(&s, extra, ne);

    uint8_t seq[FF_T9PRED_MAX_DIGITS];
    size_t n = seq_of("cas", seq);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_TRUE(ff_t9pred_session_key(&s, seq[i]));
    }
    /* Artist ranks first as the current candidate. */
    TEST_ASSERT_EQUAL_STRING("Caspa", ff_t9pred_session_current(&s));
    /* Cycling steps off the artist onto the dictionary tail and wraps back. */
    size_t total = ff_t9pred_count_ex(seq, n, extra, ne);
    TEST_ASSERT_TRUE(total >= 2);
    ff_t9pred_session_cycle(&s);
    TEST_ASSERT_TRUE(strcmp(ff_t9pred_session_current(&s), "Caspa") != 0);
    for (size_t i = 1; i < total; i++) {
        ff_t9pred_session_cycle(&s); /* complete the loop */
    }
    TEST_ASSERT_EQUAL_STRING("Caspa", ff_t9pred_session_current(&s)); /* wrapped */
}

static void session_reset_unbinds_extra(void)
{
    fp_pack_t p;
    make_pack(&p);
    char const *extra[16] = {0};
    int ne = fp_t9words_collect(&p, extra, 16);

    ff_t9pred_session_t s;
    ff_t9pred_session_reset(&s);
    ff_t9pred_session_set_extra(&s, extra, ne);
    uint8_t seq[FF_T9PRED_MAX_DIGITS];
    size_t n = seq_of("cas", seq);
    for (size_t i = 0; i < n; i++) ff_t9pred_session_key(&s, seq[i]);
    TEST_ASSERT_EQUAL_STRING("Caspa", ff_t9pred_session_current(&s));

    /* reset() clears the binding: same keys now hit only the dictionary. */
    ff_t9pred_session_reset(&s);
    for (size_t i = 0; i < n; i++) ff_t9pred_session_key(&s, seq[i]);
    char const *cur = ff_t9pred_session_current(&s);
    if (cur) {
        TEST_ASSERT_TRUE(strcmp(cur, "Caspa") != 0);
    }
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(collect_gathers_artists_stages_landmarks_in_order);
    RUN_TEST(collect_skips_empty_and_dedups_case_insensitively);
    RUN_TEST(collect_respects_max_and_null_pack);
    RUN_TEST(collect_handles_max_length_name);

    RUN_TEST(typing_an_artist_returns_that_artist);
    RUN_TEST(extra_outranks_a_colliding_dictionary_word);
    RUN_TEST(empty_or_absent_pack_leaves_engine_unchanged);
    RUN_TEST(extra_deduped_against_dictionary_word);

    RUN_TEST(session_with_extra_predicts_and_cycles);
    RUN_TEST(session_reset_unbinds_extra);

    return UNITY_END();
}
