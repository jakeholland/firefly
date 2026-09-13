/**
 * test_demo_pack.c — S20 demo mode: the authored Firefly Fields festpack
 * (firmware/assets/demo/firefly-fields.festpack.json) parses cleanly to
 * the v0.1 schema and carries the counts the demo world expects.
 *
 * This is DEMO/fixture data living in the firefly repo (not fest-almanac).
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fp_pack.h"

#ifndef FF_DEMO_PACK_PATH
#error "FF_DEMO_PACK_PATH must be defined (path to firefly-fields.festpack.json)"
#endif

#define BUF_SZ (256u * 1024u)

void setUp(void) {}
void tearDown(void) {}

/* S26 slice (a) - shared file-scope jsmn scratch: fp_parse no longer owns
 * a static token arena (fp_pack.h), so every test in this file supplies
 * one. Unity runs tests sequentially in one thread, so sharing this
 * across test functions is safe - each call fully consumes and
 * re-tokenizes it. */
static jsmntok_t s_toks[FP_MAX_TOKENS];

static size_t load(char *buf, size_t bufsz)
{
    FILE *f = fopen(FF_DEMO_PACK_PATH, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, FF_DEMO_PACK_PATH);
    size_t n = fread(buf, 1, bufsz, f);
    TEST_ASSERT_TRUE(n > 0);
    fclose(f);
    return n;
}

/* S20 — parses OK, within every FP_MAX_* cap. */
static void test_S20_demo_pack_parses(void)
{
    static char buf[BUF_SZ];
    size_t len = load(buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);

    /* Festival identity + a KNOWN, non-assumed offset (America/Los_Angeles,
     * PDT) so the pack's offset wins the wall-clock resolution. */
    TEST_ASSERT_EQUAL_STRING("Firefly Fields", pack.name);
    TEST_ASSERT_EQUAL_UINT16(2026, pack.year);
    TEST_ASSERT_TRUE(pack.origin_known);
    TEST_ASSERT_TRUE(pack.origin_approx);
    TEST_ASSERT_EQUAL_INT16(-420, pack.utc_offset_min);
    TEST_ASSERT_FALSE(pack.utc_offset_assumed);

    /* Stages / acts / map, all fictional. */
    TEST_ASSERT_EQUAL_UINT8(5, pack.n_stages);
    TEST_ASSERT_EQUAL_UINT16(25, pack.n_sets);
    TEST_ASSERT_EQUAL_UINT8(16, pack.n_features);
    TEST_ASSERT_EQUAL_UINT8(2, pack.n_landmarks);

    /* Within the caps (a demo pack must never blow a budget). */
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(FP_MAX_STAGES, pack.n_stages);
    TEST_ASSERT_LESS_OR_EQUAL_UINT16(FP_MAX_SETS, pack.n_sets);
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(FP_MAX_FEATURES, pack.n_features);
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(FP_MAX_LANDMARKS, pack.n_landmarks);
}

/* S20 — the Saturday headliner FIREFLY is starred and timed; a starred
 * Sunrise Grove set exists too (so Now can show a countdown). */
static void test_S20_firefly_starred_and_timed(void)
{
    static char buf[BUF_SZ];
    size_t len = load(buf, sizeof(buf));
    fp_pack_t pack;
    TEST_ASSERT_EQUAL_INT(FP_OK, fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS));

    int firefly = -1;
    int n_starred = 0;
    for (uint16_t i = 0; i < pack.n_sets; i++) {
        if (strcmp(pack.sets[i].artist, "FIREFLY") == 0) firefly = (int)i;
        if (pack.sets[i].starred) n_starred++;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, firefly);
    TEST_ASSERT_TRUE(pack.sets[firefly].starred);
    TEST_ASSERT_GREATER_OR_EQUAL_INT16(0, pack.sets[firefly].start_min); /* timed, not TBD */
    TEST_ASSERT_EQUAL_INT(2, n_starred); /* FIREFLY + one Sunrise Grove set */
}

/* 2026-09-11 review fixup: this pack is real, shipped evidence for the
 * fp_parse_set_daytime midnight-fold bug — four authored sets publish a
 * clock-past-midnight "end" (e.g. LOST + FOUND's "23:30" -> "01:00")
 * with no `end_day`, the exact shape the fix targets (see
 * test_festpack.c's S05_review_published_end_before_start_without_end_day_folds_past_midnight
 * for the isolated case). Assert it against the actual shipped asset so
 * a future change to this fixture can't silently regress it. */
static void test_S20_midnight_crossing_sets_fold_without_end_day(void)
{
    static char buf[BUF_SZ];
    size_t len = load(buf, sizeof(buf));
    fp_pack_t pack;
    TEST_ASSERT_EQUAL_INT(FP_OK, fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS));

    static char const *const midnight_crossers[] = {
        "LOST + FOUND", "VOLTAGE", "PHOTON", "TWILIGHT FUNCTION",
    };
    int found = 0;
    for (uint16_t i = 0; i < pack.n_sets; i++) {
        fp_set_t const *s = &pack.sets[i];
        for (size_t k = 0; k < sizeof(midnight_crossers) / sizeof(midnight_crossers[0]); k++) {
            if (strcmp(s->artist, midnight_crossers[k]) != 0) continue;
            found++;
            TEST_ASSERT_GREATER_OR_EQUAL_INT16(0, s->start_min);
            TEST_ASSERT_GREATER_OR_EQUAL_INT16(0, s->end_min);
            TEST_ASSERT_TRUE_MESSAGE(s->end_min > s->start_min, s->artist);
            TEST_ASSERT_TRUE_MESSAGE(s->end_min >= 1440, s->artist); /* actually past midnight */
        }
    }
    TEST_ASSERT_EQUAL_INT(4, found);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_S20_demo_pack_parses);
    RUN_TEST(test_S20_firefly_starred_and_timed);
    RUN_TEST(test_S20_midnight_crossing_sets_fold_without_end_day);
    return UNITY_END();
}
