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
    fp_result_t r = fp_parse(buf, len, &pack);
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
    TEST_ASSERT_EQUAL_INT(FP_OK, fp_parse(buf, len, &pack));

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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_S20_demo_pack_parses);
    RUN_TEST(test_S20_firefly_starred_and_timed);
    return UNITY_END();
}
