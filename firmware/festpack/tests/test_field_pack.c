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

    /* 2026-09-09: the refreshed fest-almanac pack now states
     * "utc_offset_min": -240 explicitly, so this is a KNOWN offset, not
     * the assumed default it used to fall back to (S05's own honesty
     * amendment — the flag must distinguish the two). */
    TEST_ASSERT_EQUAL_INT16(-240, pack.utc_offset_min);
    TEST_ASSERT_FALSE(pack.utc_offset_assumed);

    /* Sep 18-20 2026 -> day-of-year 261..263 (non-leap year). */
    TEST_ASSERT_EQUAL_UINT16(261, pack.start_doy);
    TEST_ASSERT_EQUAL_UINT16(263, pack.end_doy);

    /* Real Lost Lands lineup/map, well within caps. 2026-09-09: real set
     * times landed (222 sets, was 27 all-null) — see
     * docs/specs/S05-festpack.md's dated amendment. */
    TEST_ASSERT_EQUAL_UINT8(7, pack.n_stages);
    TEST_ASSERT_EQUAL_UINT16(222, pack.n_sets);
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(FP_MAX_STAGES, pack.n_stages);
    TEST_ASSERT_LESS_OR_EQUAL_UINT16(FP_MAX_SETS, pack.n_sets);
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(FP_MAX_FEATURES, pack.n_features);
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(FP_MAX_LANDMARKS, pack.n_landmarks);
}

/* 2026-09-09 (S05-festpack.md amendment): the real field pack encodes an
 * after-midnight set as a plain calendar `day`/`start` pair plus an
 * explicit `night`; the parser folds that onto the festival night's
 * day_doy at start_min >= 1440. See test_festpack.c's equivalent case
 * (which runs against the tests/fixtures/ copy) for the full rationale;
 * this is the same assertion against the actual EMBEDDED asset, which is
 * a byte-identical copy of fest-almanac's pack. */
static void test_S05_field_pack_after_midnight_set_folds_onto_festival_night(void)
{
    static char buf[BUF_SZ];
    size_t len = load(buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);

    fp_set_t const *resistance = NULL, *sippy = NULL, *excision_fri = NULL;
    for (uint16_t i = 0; i < pack.n_sets; i++) {
        if (strcmp(pack.sets[i].artist, "The Resistance") == 0) resistance = &pack.sets[i];
        if (strcmp(pack.sets[i].artist, "Sippy") == 0) sippy = &pack.sets[i];
        /* Two sets are billed "Excision"; the Friday one on Prehistoric
         * is the 2-hour set with the explicit end. */
        if (strcmp(pack.sets[i].artist, "Excision") == 0 && pack.sets[i].end_min >= 0) {
            excision_fri = &pack.sets[i];
        }
    }
    TEST_ASSERT_NOT_NULL(resistance);
    TEST_ASSERT_NOT_NULL(sippy);
    TEST_ASSERT_NOT_NULL(excision_fri);

    TEST_ASSERT_EQUAL_UINT16(resistance->day_doy, sippy->day_doy); /* same Friday night, not the
                                                                      next calendar day */
    TEST_ASSERT_EQUAL_INT16(24 * 60 + 15, sippy->start_min);       /* 00:15 folded to 1455 */
    TEST_ASSERT_TRUE(sippy->start_min > resistance->start_min);

    /* `end_day` is the field that makes Excision's end unambiguous:
     * 00:10 on 2026-09-19, i.e. 1450 measured from Friday night's
     * midnight — strictly after its own 22:10 start, no fold needed. */
    TEST_ASSERT_EQUAL_UINT16(resistance->day_doy, excision_fri->day_doy);
    TEST_ASSERT_EQUAL_INT16(22 * 60 + 10, excision_fri->start_min);
    TEST_ASSERT_EQUAL_INT16(24 * 60 + 10, excision_fri->end_min);
    TEST_ASSERT_TRUE(excision_fri->end_min > excision_fri->start_min);
}

/* The embedded asset is a byte-identical copy of fest-almanac's pack
 * (firmware/assets/field/README.md's "never hand-edit" rule), so its
 * shape is a fact about that upstream pack, asserted here so a careless
 * refresh cannot quietly change the schedule out from under the
 * schedule-engine tests: 222 sets, of which exactly 55 are billed under
 * the PREVIOUS calendar day's festival night (`night` != `day`, folded
 * to start_min >= 1440). */
static void test_S05_field_pack_has_222_sets_55_after_midnight(void)
{
    static char buf[BUF_SZ];
    size_t len = load(buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);

    TEST_ASSERT_EQUAL_UINT16(222, pack.n_sets);

    uint16_t folded = 0, timed = 0;
    for (uint16_t i = 0; i < pack.n_sets; i++) {
        if (pack.sets[i].start_min >= 0) timed++;
        if (pack.sets[i].start_min >= 1440) folded++;
    }
    TEST_ASSERT_EQUAL_UINT16(222, timed);  /* every set has a published start */
    TEST_ASSERT_EQUAL_UINT16(55, folded);  /* the after-midnight ones */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_S05_field_pack_parses);
    RUN_TEST(test_S05_field_pack_after_midnight_set_folds_onto_festival_night);
    RUN_TEST(test_S05_field_pack_has_222_sets_55_after_midnight);
    return UNITY_END();
}
