/**
 * test_crewcode.c — the crew code codec, pinned against the SHARED
 * fixture.
 *
 * Spec: docs/specs/A02-crew-join.md §1 (§1.9's byte-exact vectors) and
 * docs/specs/S02-core-crew.md's 2026-09-13 amendment §D.
 *
 * The vectors are NOT transcribed into this file. They are read at
 * runtime from `docs/specs/fixtures/A02-crew-codes.json` — the same file
 * the app's Swift `CrewCodeTests` reads — so the two implementations
 * cannot drift apart without one of them going red. Parsing uses the
 * vendored jsmn (firmware/third_party/jsmn.h), header-only and
 * test-only: core itself stays zero-dependency.
 *
 * The proxy check (docs/review/code-review.md item 6), asked of this
 * file's central test: *what input satisfies "the PSK matches" and
 * violates "the derivation is right"?* A hardcoded expected value copied
 * out of our own implementation's output would — which is exactly why
 * the expectations come from a file this implementation did not produce
 * and cannot write. Vector 6 (`FIRE-4KIM7X` -> a DIFFERENT key from
 * vector 1) is the second half of that guard: it fails loudly if
 * Crockford aliasing is ever quietly dropped, which a same-key-as-before
 * regression test could not see.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "ff_crewcode.h"

#define JSMN_STATIC
#include "jsmn.h"

#ifndef FF_CREWCODE_FIXTURE
#define FF_CREWCODE_FIXTURE "docs/specs/fixtures/A02-crew-codes.json"
#endif

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* Minimal jsmn helpers — the same shape targets/sim/fixture.c uses      */
/* ------------------------------------------------------------------- */

#define FX_MAX_TOKENS 512

typedef struct {
    char      *json;
    jsmntok_t  toks[FX_MAX_TOKENS];
    int        ntok;
} fx_t;

static fx_t g_fx;

static void fx_load(void)
{
    FILE *f = fopen(FF_CREWCODE_FIXTURE, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "fixture " FF_CREWCODE_FIXTURE " not readable");
    TEST_ASSERT_EQUAL_INT(0, fseek(f, 0, SEEK_END));
    long const sz = ftell(f);
    TEST_ASSERT_TRUE(sz > 0);
    rewind(f);
    g_fx.json = (char *)malloc((size_t)sz + 1u);
    TEST_ASSERT_NOT_NULL(g_fx.json);
    TEST_ASSERT_EQUAL_size_t((size_t)sz, fread(g_fx.json, 1u, (size_t)sz, f));
    g_fx.json[sz] = '\0';
    fclose(f);

    jsmn_parser p;
    jsmn_init(&p);
    g_fx.ntok = jsmn_parse(&p, g_fx.json, (size_t)sz, g_fx.toks, FX_MAX_TOKENS);
    TEST_ASSERT_TRUE_MESSAGE(g_fx.ntok > 0, "fixture JSON did not tokenize (raise FX_MAX_TOKENS?)");
}

static void fx_free(void)
{
    free(g_fx.json);
    g_fx.json = NULL;
}

/* Index of the token after the subtree rooted at `i` — the same shape
 * targets/sim/fixture.c's own fx_skip uses (which in turn mirrors
 * festpack's fp_skip). */
static int fx_skip(int i)
{
    if (i < 0 || i >= g_fx.ntok) return g_fx.ntok;
    jsmntok_t const *t = &g_fx.toks[i];
    int next = i + 1;
    if (t->type == JSMN_OBJECT) {
        for (int k = 0; k < t->size; k++) {
            next = fx_skip(next); /* key */
            next = fx_skip(next); /* value */
        }
    } else if (t->type == JSMN_ARRAY) {
        for (int k = 0; k < t->size; k++) {
            next = fx_skip(next);
        }
    }
    return next;
}

static bool fx_streq(int i, char const *s)
{
    size_t const len = strlen(s);
    return (size_t)(g_fx.toks[i].end - g_fx.toks[i].start) == len &&
           memcmp(g_fx.json + g_fx.toks[i].start, s, len) == 0;
}

/* Member `key` of the object at `obj_i`, or -1. */
static int fx_get(int obj_i, char const *key)
{
    if (obj_i < 0 || obj_i >= g_fx.ntok || g_fx.toks[obj_i].type != JSMN_OBJECT) return -1;
    int j = obj_i + 1;
    for (int k = 0; k < g_fx.toks[obj_i].size; k++) {
        int const key_i = j;
        int const val_i = key_i + 1; /* keys are plain strings: one token */
        if (g_fx.toks[key_i].type == JSMN_STRING && fx_streq(key_i, key)) return val_i;
        j = fx_skip(val_i);
    }
    return -1;
}

/* Copies a string token (no escapes appear in this fixture's values). */
static void fx_str(int i, char *out, size_t n)
{
    size_t len = (size_t)(g_fx.toks[i].end - g_fx.toks[i].start);
    if (len > n - 1u) len = n - 1u;
    memcpy(out, g_fx.json + g_fx.toks[i].start, len);
    out[len] = '\0';
}

static void hex_to_bytes(char const *hex, uint8_t *out, size_t n)
{
    TEST_ASSERT_EQUAL_size_t(n * 2u, strlen(hex));
    for (size_t i = 0; i < n; i++) {
        char b[3] = {hex[2u * i], hex[2u * i + 1u], '\0'};
        out[i] = (uint8_t)strtoul(b, NULL, 16);
    }
}

/* ------------------------------------------------------------------- */
/* A02 AC1 — the alphabet is Crockford's, minus I/L/O/U                  */
/* ------------------------------------------------------------------- */

static void A02_AC1_alphabet_matches_the_fixture(void)
{
    int const a = fx_get(0, "alphabet");
    TEST_ASSERT_TRUE(a > 0);
    char want[64];
    fx_str(a, want, sizeof(want));
    TEST_ASSERT_EQUAL_STRING(want, ff_crewcode_alphabet);
    /* Stated separately from the string compare so a fixture edit that
     * reintroduced one of these would fail with a message naming it. */
    TEST_ASSERT_NULL(strchr(ff_crewcode_alphabet, 'I'));
    TEST_ASSERT_NULL(strchr(ff_crewcode_alphabet, 'L'));
    TEST_ASSERT_NULL(strchr(ff_crewcode_alphabet, 'O'));
    TEST_ASSERT_NULL(strchr(ff_crewcode_alphabet, 'U'));
    TEST_ASSERT_EQUAL_size_t(32u, strlen(ff_crewcode_alphabet));
}

/* ------------------------------------------------------------------- */
/* A02 AC2 — every vector: parse -> canonical, and derive -> PSK         */
/* ------------------------------------------------------------------- */

static void A02_AC2_vectors_parse_and_derive(void)
{
    int const vectors = fx_get(0, "vectors");
    TEST_ASSERT_TRUE(vectors > 0);
    TEST_ASSERT_EQUAL_INT(JSMN_ARRAY, g_fx.toks[vectors].type);
    TEST_ASSERT_TRUE_MESSAGE(g_fx.toks[vectors].size >= 6, "fixture lost vectors");

    int v = vectors + 1;
    for (int i = 0; i < g_fx.toks[vectors].size; i++) {
        char input[64], canonical[64], psk_hex[128];
        fx_str(fx_get(v, "input"), input, sizeof(input));
        fx_str(fx_get(v, "canonical"), canonical, sizeof(canonical));
        fx_str(fx_get(v, "psk_hex"), psk_hex, sizeof(psk_hex));

        char got[FF_CREWCODE_LEN + 1u];
        char msg[160];
        snprintf(msg, sizeof(msg), "vector %d: input '%s'", i + 1, input);
        TEST_ASSERT_TRUE_MESSAGE(ff_crewcode_parse(input, got), msg);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(canonical, got, msg);

        /* The canonical form is exactly ChannelSettings.name's budget —
         * the identity the whole design rests on (A02 §1.3). */
        TEST_ASSERT_EQUAL_size_t(FF_CREWCODE_LEN, strlen(got));
        TEST_ASSERT_TRUE(ff_crewcode_valid(got));

        uint8_t want_psk[FF_CREWCODE_PSK_LEN];
        hex_to_bytes(psk_hex, want_psk, sizeof(want_psk));
        uint8_t got_psk[FF_CREWCODE_PSK_LEN];
        memset(got_psk, 0xAB, sizeof(got_psk));
        TEST_ASSERT_TRUE_MESSAGE(ff_crewcode_psk(got, got_psk), msg);
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(want_psk, got_psk, sizeof(want_psk), msg);

        v = fx_skip(v);
    }
}

/* ------------------------------------------------------------------- */
/* A02 AC3 — normalisation is a real remap, not a no-op                  */
/* ------------------------------------------------------------------- */

static void A02_AC3_aliasing_changes_the_key(void)
{
    /* Vector 5/6's point: `I` -> `1` produces a DIFFERENT crew. A
     * regression that dropped aliasing would put two people who typed
     * the same thing onto two different crews, and every same-key
     * assertion in this file would still pass. */
    char a[FF_CREWCODE_LEN + 1u], b[FF_CREWCODE_LEN + 1u];
    TEST_ASSERT_TRUE(ff_crewcode_parse("FIRE-4KIM7X", a));
    TEST_ASSERT_EQUAL_STRING("FIRE-4K1M7X", a);
    TEST_ASSERT_TRUE(ff_crewcode_parse("FIRE-4K9M7X", b));

    uint8_t pa[FF_CREWCODE_PSK_LEN], pb[FF_CREWCODE_PSK_LEN];
    TEST_ASSERT_TRUE(ff_crewcode_psk(a, pa));
    TEST_ASSERT_TRUE(ff_crewcode_psk(b, pb));
    TEST_ASSERT_TRUE(memcmp(pa, pb, sizeof(pa)) != 0);

    /* `L` aliases to `1` as well; `O` to `0`. */
    char c[FF_CREWCODE_LEN + 1u];
    TEST_ASSERT_TRUE(ff_crewcode_parse("FIRE-4KLM7X", c));
    TEST_ASSERT_EQUAL_STRING("FIRE-4K1M7X", c);
    TEST_ASSERT_TRUE(ff_crewcode_parse("fire-O0OOO0", c));
    TEST_ASSERT_EQUAL_STRING("FIRE-000000", c);
}

static void A02_AC3_three_spellings_one_key(void)
{
    char const *spellings[] = {"fire 4k9m7x", "4K9M7X", "FIRE-4K9M7X", "  fire-4k9m7x  "};
    uint8_t first[FF_CREWCODE_PSK_LEN];
    for (size_t i = 0; i < sizeof(spellings) / sizeof(spellings[0]); i++) {
        char got[FF_CREWCODE_LEN + 1u];
        TEST_ASSERT_TRUE_MESSAGE(ff_crewcode_parse(spellings[i], got), spellings[i]);
        TEST_ASSERT_EQUAL_STRING("FIRE-4K9M7X", got);
        uint8_t psk[FF_CREWCODE_PSK_LEN];
        TEST_ASSERT_TRUE(ff_crewcode_psk(got, psk));
        if (i == 0) memcpy(first, psk, sizeof(first));
        else TEST_ASSERT_EQUAL_HEX8_ARRAY(first, psk, sizeof(first));
    }
}

static void A02_AC3_tag_is_stripped_literally_before_aliasing(void)
{
    /* The pinned consequence of §1.2's step ordering: a crew whose six
     * symbols are F1RE9X parses from the FULL spelling, and the tagless
     * spelling is rejected rather than guessed at. */
    char got[FF_CREWCODE_LEN + 1u];
    TEST_ASSERT_TRUE(ff_crewcode_parse("FIRE-FIRE9X", got));
    TEST_ASSERT_EQUAL_STRING("FIRE-F1RE9X", got);
    TEST_ASSERT_FALSE(ff_crewcode_parse("FIRE9X", got));
}

/* ------------------------------------------------------------------- */
/* A02 AC4 — rejections: no partial result, no fallback code             */
/* ------------------------------------------------------------------- */

static void A02_AC4_rejections_from_the_fixture(void)
{
    int const rejections = fx_get(0, "rejections");
    TEST_ASSERT_TRUE(rejections > 0);
    TEST_ASSERT_EQUAL_INT(JSMN_ARRAY, g_fx.toks[rejections].type);
    TEST_ASSERT_TRUE(g_fx.toks[rejections].size >= 5);

    int r = rejections + 1;
    for (int i = 0; i < g_fx.toks[rejections].size; i++) {
        char input[64], why[128];
        fx_str(fx_get(r, "input"), input, sizeof(input));
        fx_str(fx_get(r, "why"), why, sizeof(why));

        /* Sentinel-filled and compared afterwards: "returns false" is
         * only half the contract — the other half is that `out` is left
         * untouched, so a caller that ignores the return value cannot
         * pick up a fabricated code. */
        char out[FF_CREWCODE_LEN + 1u];
        memset(out, '#', sizeof(out));
        char msg[224];
        snprintf(msg, sizeof(msg), "rejection '%s' (%s)", input, why);
        TEST_ASSERT_FALSE_MESSAGE(ff_crewcode_parse(input, out), msg);
        for (size_t k = 0; k < sizeof(out); k++) {
            TEST_ASSERT_EQUAL_HEX8_MESSAGE('#', out[k], msg);
        }
        r = fx_skip(r);
    }
}

static void A02_AC4_U_is_rejected_never_aliased(void)
{
    /* Called out on its own because aliasing U (to V, say) is the single
     * most tempting "helpful" change here, and it would silently land a
     * typo on somebody else's crew. */
    char out[FF_CREWCODE_LEN + 1u];
    TEST_ASSERT_FALSE(ff_crewcode_parse("FIRE-4K9M7U", out));
    TEST_ASSERT_FALSE(ff_crewcode_parse("UUUUUU", out));
}

static void A02_AC4_invalid_never_derives_a_key(void)
{
    uint8_t psk[FF_CREWCODE_PSK_LEN];
    memset(psk, 0x5A, sizeof(psk));
    TEST_ASSERT_FALSE(ff_crewcode_psk("FIRE-4K9M7", psk));
    TEST_ASSERT_FALSE(ff_crewcode_psk("", psk));
    TEST_ASSERT_FALSE(ff_crewcode_psk(NULL, psk));
    TEST_ASSERT_FALSE(ff_crewcode_psk("fire-4k9m7x", psk)); /* not canonical: lowercase */
    for (size_t i = 0; i < sizeof(psk); i++) TEST_ASSERT_EQUAL_HEX8(0x5A, psk[i]);
}

static void A02_AC4_valid_is_stricter_than_parse(void)
{
    /* ff_crewcode_valid asks "is this channel NAME a crew code", where
     * accepting a sloppy spelling would be wrong: the on-air channel
     * hash folds the name's exact bytes, so a differently-spelled name
     * is a different channel (A02 §1.3). */
    TEST_ASSERT_TRUE(ff_crewcode_valid("FIRE-4K9M7X"));
    TEST_ASSERT_FALSE(ff_crewcode_valid("fire-4k9m7x"));
    TEST_ASSERT_FALSE(ff_crewcode_valid("4K9M7X"));
    TEST_ASSERT_FALSE(ff_crewcode_valid("FIRE-4K9M7XX"));
    TEST_ASSERT_FALSE(ff_crewcode_valid("LongFast"));
    TEST_ASSERT_FALSE(ff_crewcode_valid(""));
    TEST_ASSERT_FALSE(ff_crewcode_valid(NULL));
}

/* ------------------------------------------------------------------- */
/* A02 AC5 — the deep link, byte for byte                                */
/* ------------------------------------------------------------------- */

static void A02_AC5_deep_link_matches_the_fixture(void)
{
    int const vectors = fx_get(0, "vectors");
    int v = vectors + 1;
    for (int i = 0; i < g_fx.toks[vectors].size; i++) {
        int const dl = fx_get(v, "deep_link");
        if (dl > 0) {
            char want[FF_CREWCODE_URL_MAX], canonical[64];
            fx_str(dl, want, sizeof(want));
            fx_str(fx_get(v, "canonical"), canonical, sizeof(canonical));

            /* The fixture's links carry `name=Camp%20Firefly`; the puck
             * passes NULL (it has no human crew name — that lives on the
             * phone). Both shapes are pinned here so the encoder the
             * puck uses IS the encoder the fixture describes. */
            char got[FF_CREWCODE_URL_MAX];
            size_t const n = ff_crewcode_invite_url(canonical, "Camp Firefly", got, sizeof(got));
            TEST_ASSERT_EQUAL_size_t(strlen(want), n);
            TEST_ASSERT_EQUAL_STRING(want, got);
        }
        v = fx_skip(v);
    }
}

static void A02_AC5_deep_link_without_a_name(void)
{
    char got[FF_CREWCODE_URL_MAX];
    size_t const n = ff_crewcode_invite_url("FIRE-4K9M7X", NULL, got, sizeof(got));
    TEST_ASSERT_EQUAL_STRING("firefly://crew?v=1&code=FIRE-4K9M7X", got);
    TEST_ASSERT_EQUAL_size_t(strlen(got), n);

    /* An empty name is the same as no name — never `&name=`. */
    TEST_ASSERT_TRUE(ff_crewcode_invite_url("FIRE-4K9M7X", "", got, sizeof(got)) > 0u);
    TEST_ASSERT_EQUAL_STRING("firefly://crew?v=1&code=FIRE-4K9M7X", got);
}

static void A02_AC5_deep_link_refuses_rather_than_truncates(void)
{
    char small[16];
    memset(small, '#', sizeof(small));
    TEST_ASSERT_EQUAL_size_t(0u, ff_crewcode_invite_url("FIRE-4K9M7X", NULL, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small); /* "", never a half-built link */

    char buf[FF_CREWCODE_URL_MAX];
    TEST_ASSERT_EQUAL_size_t(0u, ff_crewcode_invite_url("not-a-code", NULL, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("", buf);
    TEST_ASSERT_EQUAL_size_t(0u, ff_crewcode_invite_url(NULL, NULL, buf, sizeof(buf)));
}

static void A02_AC5_name_clamp_never_splits_a_utf8_character(void)
{
    /* 24-byte clamp; "é" is 2 bytes. 12 of them is exactly 24 bytes, so
     * the 13th must drop WHOLE, not as a lone continuation byte that
     * percent-encodes to mojibake. */
    char name[64];
    for (int i = 0; i < 13; i++) memcpy(name + 2 * i, "\xC3\xA9", 2);
    name[26] = '\0';

    char got[FF_CREWCODE_URL_MAX];
    TEST_ASSERT_TRUE(ff_crewcode_invite_url("FIRE-4K9M7X", name, got, sizeof(got)) > 0u);
    char const *q = strstr(got, "&name=");
    TEST_ASSERT_NOT_NULL(q);
    /* 12 whole characters -> 12 * "%C3%A9" = 72 chars, and nothing else. */
    TEST_ASSERT_EQUAL_size_t(strlen("&name=") + 72u, strlen(q));
    for (int i = 0; i < 12; i++) {
        TEST_ASSERT_EQUAL_STRING_LEN("%C3%A9", q + strlen("&name=") + 6 * i, 6);
    }
}

int main(void)
{
    UNITY_BEGIN();
    fx_load();
    RUN_TEST(A02_AC1_alphabet_matches_the_fixture);
    RUN_TEST(A02_AC2_vectors_parse_and_derive);
    RUN_TEST(A02_AC3_aliasing_changes_the_key);
    RUN_TEST(A02_AC3_three_spellings_one_key);
    RUN_TEST(A02_AC3_tag_is_stripped_literally_before_aliasing);
    RUN_TEST(A02_AC4_rejections_from_the_fixture);
    RUN_TEST(A02_AC4_U_is_rejected_never_aliased);
    RUN_TEST(A02_AC4_invalid_never_derives_a_key);
    RUN_TEST(A02_AC4_valid_is_stricter_than_parse);
    RUN_TEST(A02_AC5_deep_link_matches_the_fixture);
    RUN_TEST(A02_AC5_deep_link_without_a_name);
    RUN_TEST(A02_AC5_deep_link_refuses_rather_than_truncates);
    RUN_TEST(A02_AC5_name_clamp_never_splits_a_utf8_character);
    fx_free();
    return UNITY_END();
}
