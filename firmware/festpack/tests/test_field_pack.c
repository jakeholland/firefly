/**
 * test_field_pack.c — S05 field festpack: the real Lost Lands 2026 pack
 * embedded for the ESP32-S3 field build
 * (firmware/assets/field/lost-lands-2026.festpack.json) parses cleanly to
 * the v0.1 schema and carries the facts the field-test load path depends
 * on. This is a verbatim copy of fest-almanac's real pack (not fixture
 * data authored in this repo) — see firmware/assets/field/README.md.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fp_pack.h"

#ifndef FF_FIELD_PACK_PATH
#error "FF_FIELD_PACK_PATH must be defined (path to lost-lands-2026.festpack.json)"
#endif

#define BUF_SZ (256u * 1024u)

void setUp(void) {}
void tearDown(void) {}

/* S26 slice (a) — file-scope jsmn scratch, shared across this file's
 * tests the same way test_demo_pack.c does (Unity runs sequentially in
 * one thread; each call fully consumes and re-tokenizes it). */
static jsmntok_t s_toks[FP_MAX_TOKENS];

static size_t load(char *buf, size_t bufsz)
{
    FILE *f = fopen(FF_FIELD_PACK_PATH, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, FF_FIELD_PACK_PATH);
    size_t n = fread(buf, 1, bufsz, f);
    TEST_ASSERT_TRUE(n > 0);
    fclose(f);
    return n;
}

/* S05 AC1-alike: the real field pack parses, within every FP_MAX_* cap,
 * and carries the identity/origin/dates the field-load path
 * (CONFIG_FF_FIELD_PACK) depends on. */
static void test_S05_field_pack_parses(void)
{
    static char buf[BUF_SZ];
    size_t len = load(buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);

    TEST_ASSERT_EQUAL_STRING("Lost Lands", pack.name);
    TEST_ASSERT_EQUAL_UINT16(2026, pack.year);

    /* Real venue, approximate (fest-almanac marks Legend Valley
     * "approximate": true). */
    TEST_ASSERT_TRUE(pack.origin_known);
    TEST_ASSERT_TRUE(pack.origin_approx);
    TEST_ASSERT_EQUAL_DOUBLE(39.9387, pack.origin.lat);
    TEST_ASSERT_EQUAL_DOUBLE(-82.4027, pack.origin.lon);

    /* No utc_offset_min extension field in this pack yet -> the
     * documented default, explicitly flagged ASSUMED (never silently
     * "known" — S05's own honesty amendment). */
    TEST_ASSERT_EQUAL_INT16(-240, pack.utc_offset_min);
    TEST_ASSERT_TRUE(pack.utc_offset_assumed);

    /* Sep 18-20 2026 -> day-of-year 261..263 (non-leap year). */
    TEST_ASSERT_EQUAL_UINT16(261, pack.start_doy);
    TEST_ASSERT_EQUAL_UINT16(263, pack.end_doy);

    /* Real Lost Lands lineup/map, well within caps. */
    TEST_ASSERT_EQUAL_UINT8(7, pack.n_stages);
    TEST_ASSERT_TRUE(pack.n_sets >= 27);
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(FP_MAX_STAGES, pack.n_stages);
    TEST_ASSERT_LESS_OR_EQUAL_UINT16(FP_MAX_SETS, pack.n_sets);
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(FP_MAX_FEATURES, pack.n_features);
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(FP_MAX_LANDMARKS, pack.n_landmarks);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_S05_field_pack_parses);
    return UNITY_END();
}
