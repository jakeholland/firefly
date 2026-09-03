/**
 * test_festpack.c — Unity tests for fp_parse(), criteria-numbered per
 * docs/specs/S05-festpack.md.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "fp_pack.h"

#ifndef FP_FIXTURE_DIR
#define FP_FIXTURE_DIR "./"
#endif

#define FIXTURE_BUF_SZ (256u * 1024u)

void setUp(void) {}
void tearDown(void) {}

/* S26 slice (a) - shared file-scope jsmn scratch: fp_parse no longer owns
 * a static token arena (fp_pack.h), so every test in this file supplies
 * one. Unity runs tests sequentially in one thread, so sharing this
 * across test functions is safe - each call fully consumes and
 * re-tokenizes it. */
static jsmntok_t s_toks[FP_MAX_TOKENS];

/* Reads a fixture file fully into `buf` (must be <= bufsz bytes) and
 * returns its length. Fails the test loudly if the file is missing —
 * a missing fixture is a test-setup bug, not a parser behavior to
 * verify. */
static size_t load_fixture(char const *name, char *buf, size_t bufsz)
{
    char path[512];
    snprintf(path, sizeof(path), "%s%s", FP_FIXTURE_DIR, name);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    size_t n = fread(buf, 1, bufsz, f);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, path);
    fclose(f);
    return n;
}

/* ======================================================================
 * AC1 — parses the real vendored Lost Lands 2026 fixture.
 * ==================================================================== */

static void S05_AC1_lost_lands_has_7_stages_exact_names_and_colors(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("lost-lands-2026.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);
    TEST_ASSERT_EQUAL_UINT8(7, pack.n_stages);

    static const struct {
        char const *id, *name;
        uint32_t color;
    } expect[7] = {
        {"prehistoric", "Prehistoric Stage", 0xffc66bu}, {"wompy-woods", "Wompy Woods", 0x4fd8c4u},
        {"subsidia", "Subsidia Stage", 0xff5ca8u},        {"forest", "Forest Stage", 0x9be07bu},
        {"crater", "The Crater", 0xb08cffu},               {"raptor-alley", "Raptor Alley", 0x5aa7e6u},
        {"grove", "The Grove", 0xe0c47bu},
    };
    for (int i = 0; i < 7; i++) {
        TEST_ASSERT_EQUAL_STRING(expect[i].id, pack.stages[i].id);
        TEST_ASSERT_EQUAL_STRING(expect[i].name, pack.stages[i].name);
        TEST_ASSERT_EQUAL_UINT32(expect[i].color, pack.stages[i].color_rgb);
    }
}

static void S05_AC1_lost_lands_has_at_least_27_sets_all_times_null(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("lost-lands-2026.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT16(27, pack.n_sets);
    TEST_ASSERT_EQUAL_UINT16(27, pack.n_sets); /* exact count of the real fixture, today */

    for (uint16_t i = 0; i < pack.n_sets; i++) {
        TEST_ASSERT_EQUAL_INT16(-1, pack.sets[i].start_min);
        TEST_ASSERT_EQUAL_INT16(-1, pack.sets[i].end_min);
    }

    /* Spot checks. */
    TEST_ASSERT_EQUAL_STRING("Caspa", pack.sets[0].artist);
    TEST_ASSERT_EQUAL_INT8(-1, pack.sets[0].stage_idx); /* stage: null */
    TEST_ASSERT_EQUAL_UINT16(259, pack.sets[0].day_doy); /* 2026-09-16 */

    TEST_ASSERT_EQUAL_STRING("Excision", pack.sets[4].artist);
    TEST_ASSERT_EQUAL_INT8(0, pack.sets[4].stage_idx); /* "prehistoric" == stages[0] */
    TEST_ASSERT_EQUAL_UINT16(261, pack.sets[4].day_doy); /* 2026-09-18 */
}

static void S05_AC1_lost_lands_festival_meta_and_utc_offset_default(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("lost-lands-2026.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);

    TEST_ASSERT_EQUAL_STRING("Lost Lands", pack.name);
    TEST_ASSERT_EQUAL_UINT16(2026, pack.year);
    TEST_ASSERT_EQUAL_UINT16(261, pack.start_doy); /* 2026-09-18 */
    TEST_ASSERT_EQUAL_UINT16(263, pack.end_doy);   /* 2026-09-20 */
    TEST_ASSERT_TRUE(pack.origin_known); /* venue.lat/lon both present, non-null */
    TEST_ASSERT_TRUE(pack.origin_approx);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 39.936f, (float)pack.origin.lat);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, -82.414f, (float)pack.origin.lon);

    /* The real Lost Lands 2026 pack has no utc_offset_min field — see the
     * S05 PR body. Must default to -240 (EDT) rather than error, and the
     * "this was assumed, not read" flag must be set. */
    TEST_ASSERT_EQUAL_INT16(-240, pack.utc_offset_min);
    TEST_ASSERT_TRUE(pack.utc_offset_assumed);

    TEST_ASSERT_EQUAL_UINT8(9, pack.n_features);
    TEST_ASSERT_EQUAL_UINT8(2, pack.n_landmarks);
    for (uint8_t i = 0; i < pack.n_features; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, pack.features[i].n_pts); /* every polygon is null */
    }
    for (uint8_t i = 0; i < pack.n_landmarks; i++) {
        TEST_ASSERT_FALSE(pack.landmarks[i].has_pos); /* every lat/lon is null */
    }
}

/* ======================================================================
 * AC2 — null handling + absent optional sections.
 * ==================================================================== */

static void S05_AC2_nulls_in_every_nullable_slot_parse(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("nulls.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);

    TEST_ASSERT_EQUAL_UINT8(1, pack.n_stages);
    TEST_ASSERT_EQUAL_UINT16(2, pack.n_sets);

    TEST_ASSERT_EQUAL_STRING("Nobody", pack.sets[0].artist);
    TEST_ASSERT_EQUAL_INT8(-1, pack.sets[0].stage_idx);
    TEST_ASSERT_EQUAL_INT16(-1, pack.sets[0].start_min);
    TEST_ASSERT_EQUAL_INT16(-1, pack.sets[0].end_min);
    TEST_ASSERT_EQUAL_STRING("", pack.sets[0].note);

    TEST_ASSERT_EQUAL_STRING("Somebody", pack.sets[1].artist);
    TEST_ASSERT_EQUAL_INT8(0, pack.sets[1].stage_idx);
    TEST_ASSERT_EQUAL_INT16(14 * 60 + 30, pack.sets[1].start_min);
    TEST_ASSERT_EQUAL_INT16(15 * 60 + 30, pack.sets[1].end_min);

    TEST_ASSERT_EQUAL_UINT8(1, pack.n_features);
    TEST_ASSERT_EQUAL_UINT8(FP_KIND_ENTRANCE, pack.features[0].kind);
    TEST_ASSERT_EQUAL_INT8(-1, pack.features[0].stage_idx);
    TEST_ASSERT_EQUAL_UINT8(0, pack.features[0].n_pts); /* polygon: null */

    TEST_ASSERT_EQUAL_UINT8(1, pack.n_landmarks);
    TEST_ASSERT_FALSE(pack.landmarks[0].has_pos); /* lat/lon: null */

    TEST_ASSERT_TRUE(pack.origin_known); /* venue.lat/lon present in this fixture */
    TEST_ASSERT_EQUAL_INT16(-300, pack.utc_offset_min); /* explicit, not defaulted */
    TEST_ASSERT_FALSE(pack.utc_offset_assumed);
}

static void S05_AC2_absent_optional_sections_parse(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("minimal.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r); /* no "map" key, no utc_offset_min, no approximate */

    TEST_ASSERT_EQUAL_UINT8(1, pack.n_stages);
    TEST_ASSERT_EQUAL_UINT16(1, pack.n_sets);
    TEST_ASSERT_EQUAL_UINT8(0, pack.n_features);
    TEST_ASSERT_EQUAL_UINT8(0, pack.n_landmarks);
    TEST_ASSERT_EQUAL_INT16(-240, pack.utc_offset_min); /* defaulted */
    TEST_ASSERT_TRUE(pack.utc_offset_assumed);          /* field absent */
    TEST_ASSERT_TRUE(pack.origin_known);                /* venue.lat/lon present */
    TEST_ASSERT_FALSE(pack.origin_approx);              /* defaulted (approximate absent) */
    TEST_ASSERT_EQUAL_UINT16(182, pack.start_doy);       /* 2027-07-01 */

    TEST_ASSERT_EQUAL_INT16(20 * 60, pack.sets[0].start_min);
    TEST_ASSERT_EQUAL_INT16(21 * 60, pack.sets[0].end_min);
    TEST_ASSERT_EQUAL_INT8(0, pack.sets[0].stage_idx);
}

/* ======================================================================
 * AC3 — wrong version / truncated / non-JSON: correct errors, no crash.
 * Plus a deterministic 10k-iteration fuzz smoke.
 * ==================================================================== */

static void S05_AC3_wrong_version_returns_err_version(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("wrong_version.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_ERR_VERSION, r);
    TEST_ASSERT_EQUAL_UINT8(0, pack.n_stages); /* out left zeroed on error */
}

static void S05_AC3_missing_version_returns_err_version(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("missing_version.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_ERR_VERSION, r);
}

static void S05_AC3_truncated_json_returns_err_json(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("truncated.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_ERR_JSON, r);
}

static void S05_AC3_non_json_returns_err_json(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("non_json.txt", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_ERR_JSON, r);
}

static void S05_AC3_null_and_empty_input_return_err_json_no_crash(void)
{
    fp_pack_t pack;
    TEST_ASSERT_EQUAL_INT(FP_ERR_JSON, fp_parse(NULL, 0, &pack, s_toks, FP_MAX_TOKENS));
    TEST_ASSERT_EQUAL_INT(FP_ERR_JSON, fp_parse("", 0, &pack, s_toks, FP_MAX_TOKENS));
    TEST_ASSERT_EQUAL_INT(FP_ERR_JSON, fp_parse("{", 1, &pack, s_toks, FP_MAX_TOKENS));
    TEST_ASSERT_EQUAL_INT(FP_ERR_JSON, fp_parse("not json", 8, &pack, s_toks, FP_MAX_TOKENS));
}

static uint32_t g_rng_state;

static uint32_t xorshift32(void)
{
    uint32_t x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng_state = x;
    return x;
}

/* Deterministic (fixed seed) fuzz smoke: half pure-random byte strings,
 * half small random mutations of a known-valid fixture. Requirement is
 * simply "no crash, valid result code" — S05 AC3. */
static void S05_AC3_fuzz_smoke_10000_iterations_no_crash(void)
{
    char base[FIXTURE_BUF_SZ];
    size_t base_len = load_fixture("minimal.festpack.json", base, sizeof(base));

    g_rng_state = 0xC0FFEE01u; /* fixed seed: deterministic across runs */
    char buf[4096];

    for (int iter = 0; iter < 10000; iter++) {
        size_t len;
        if ((iter & 1) == 0) {
            len = xorshift32() % sizeof(buf);
            for (size_t i = 0; i < len; i++) buf[i] = (char)(xorshift32() & 0xFFu);
        } else {
            len = base_len < sizeof(buf) ? base_len : sizeof(buf);
            memcpy(buf, base, len);
            int nmut = 1 + (int)(xorshift32() % 8);
            for (int m = 0; m < nmut; m++) {
                size_t pos = len ? (xorshift32() % len) : 0;
                buf[pos] = (char)(xorshift32() & 0xFFu);
            }
        }
        fp_pack_t pack;
        fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
        TEST_ASSERT_TRUE(r == FP_OK || r == FP_ERR_JSON || r == FP_ERR_VERSION || r == FP_ERR_TOO_BIG);
    }
}

static void S05_AC3_deeply_nested_input_returns_err_json_no_crash(void)
{
    /* 100 levels of array nesting, well past FP_MAX_JSON_DEPTH (16),
     * placed where fp_obj_get() must fp_skip() over it while looking for
     * "festival"/"stages"/"schedule". Must fail cleanly, not overflow
     * the call stack (see fp_skip_depth() in fp_pack.c). */
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("deep_nesting.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_ERR_JSON, r);
    TEST_ASSERT_EQUAL_UINT8(0, pack.n_stages); /* left zeroed */
}

/* ======================================================================
 * Polygon point format: schema mandates [[lat, lon], ...] tuple arrays,
 * not {"lat":,"lon":} objects. An object-shaped point must be a parse
 * error, never silently projected as wrong data.
 * ==================================================================== */

static void S05_review_object_format_polygon_point_returns_err_json(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("object_format_polygon.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_ERR_JSON, r);
}

/* ======================================================================
 * AC4 — overflow fixtures.
 * ==================================================================== */

static void S05_AC4_13_stages_returns_err_too_big(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("overflow_stages.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    memset(&pack, 0xAA, sizeof(pack)); /* poison, to prove fp_parse re-zeros on error */
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_ERR_TOO_BIG, r);
    TEST_ASSERT_EQUAL_UINT8(0, pack.n_stages);
    TEST_ASSERT_EQUAL_UINT16(0, pack.n_sets);
}

static void S05_AC4_257_sets_returns_err_too_big(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("overflow_sets.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_ERR_TOO_BIG, r);
}

static void S05_AC4_25_features_returns_err_too_big(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("overflow_features.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_ERR_TOO_BIG, r);
}

static void S05_AC4_13_landmarks_returns_err_too_big(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("overflow_landmarks.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_ERR_TOO_BIG, r);
}

static void S05_AC4_25_polygon_points_returns_err_too_big(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("overflow_polygon.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_ERR_TOO_BIG, r);
}

/* "Exactly at the cap" companions to the five overflow tests above — each
 * proves cap -> FP_OK (not just cap+1 -> FP_ERR_TOO_BIG), so an off-by-one
 * mutation (`>` -> `>=` in any fp_parse_* size check) fails the suite. */

static void S05_AC4_at_cap_12_stages_returns_ok(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("at_cap_stages.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);
    TEST_ASSERT_EQUAL_UINT8(FP_MAX_STAGES, pack.n_stages);
}

static void S05_AC4_at_cap_256_sets_returns_ok(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("at_cap_sets.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);
    TEST_ASSERT_EQUAL_UINT16(FP_MAX_SETS, pack.n_sets);
}

static void S05_AC4_at_cap_24_features_returns_ok(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("at_cap_features.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);
    TEST_ASSERT_EQUAL_UINT8(FP_MAX_FEATURES, pack.n_features);
}

static void S05_AC4_at_cap_12_landmarks_returns_ok(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("at_cap_landmarks.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);
    TEST_ASSERT_EQUAL_UINT8(FP_MAX_LANDMARKS, pack.n_landmarks);
}

static void S05_AC4_at_cap_24_polygon_points_returns_ok(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("at_cap_polygon.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);
    TEST_ASSERT_EQUAL_UINT8(1, pack.n_features);
    TEST_ASSERT_EQUAL_UINT8(FP_MAX_POLY_PTS, pack.features[0].n_pts);
}

/* ======================================================================
 * AC5 — feature polygon lat/lon -> east/north projection.
 * ==================================================================== */

static void S05_AC5_feature_polygon_projects_known_square_within_1m(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("square.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);
    TEST_ASSERT_EQUAL_UINT8(1, pack.n_features);
    TEST_ASSERT_EQUAL_UINT8(4, pack.features[0].n_pts);

    /* Expected values computed independently (see PR body for the delta
     * vs. the old local fp_project() stub) with ff_geo_project()'s actual
     * formula: north = dlat_rad * R, east = dlon_rad * R * cos(lat0_rad),
     * R = 6,371,000 m (mean earth radius) — a proper radian/great-circle
     * scale rather than fp_project()'s fixed 111320/110540 m/deg
     * constants. Values differ from the old stub by well under 1%. */
    static const float expect_en[4][2] = {
        {85.2600f, 111.1949f},   /* NE */
        {-85.2600f, 111.1949f},  /* NW */
        {-85.2600f, -111.1949f}, /* SW */
        {85.2600f, -111.1949f},  /* SE */
    };
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_FLOAT_WITHIN(1.0f, expect_en[i][0], pack.features[0].pts_en[i][0]);
        TEST_ASSERT_FLOAT_WITHIN(1.0f, expect_en[i][1], pack.features[0].pts_en[i][1]);
    }
}

/* ======================================================================
 * Honest-data: a null festival.venue.lat/lon must surface as
 * origin_known == false, never a silent (0,0) origin presented as real.
 * ==================================================================== */

static void S05_review_null_venue_position_sets_origin_known_false(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("null_venue.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);
    TEST_ASSERT_FALSE(pack.origin_known);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, (float)pack.origin.lat);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, (float)pack.origin.lon);
}

/* ======================================================================
 * Honest-data: wrong-TYPED numeric fields (present, but not the type the
 * schema promises — e.g. a quoted "43.7" where a JSON number is
 * required) must never look "verified"/"known"/"explicit" while actually
 * holding a default. See docs/specs/S05-festpack.md's Amendments entry
 * for the per-field policy this proves.
 * ==================================================================== */

/* Festival origin: venue.lat/lon has no per-field "unknown" concept
 * distinct from origin_known itself, so a wrong-typed lat/lon is folded
 * into that SAME honest-unknown slot the null-venue case already uses —
 * origin_known stays false, origin stays {0,0}, and the rest of the pack
 * (which has nothing to do with the venue) still parses. */
static void S05_review_string_origin_lat_sets_origin_known_false(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("wrong_type_origin.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);
    TEST_ASSERT_FALSE(pack.origin_known);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, (float)pack.origin.lat);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, (float)pack.origin.lon);
    /* Nothing else about the pack is affected. */
    TEST_ASSERT_EQUAL_UINT8(1, pack.n_stages);
    TEST_ASSERT_EQUAL_UINT16(1, pack.n_sets);
    TEST_ASSERT_EQUAL_STRING("Solo Act", pack.sets[0].artist);
}

/* Landmark position: has_pos is exactly the "known" flag has_pos exists
 * for — a wrong-typed lat leaves it false, same as a null/absent
 * position, while the landmark's other fields (id/name) still parse. */
static void S05_review_string_landmark_lat_sets_has_pos_false(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("wrong_type_landmark.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);
    TEST_ASSERT_EQUAL_UINT8(1, pack.n_landmarks);
    TEST_ASSERT_FALSE(pack.landmarks[0].has_pos);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, pack.landmarks[0].east_m);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, pack.landmarks[0].north_m);
    TEST_ASSERT_EQUAL_STRING("lm1", pack.landmarks[0].id);
    TEST_ASSERT_EQUAL_STRING("Suspect Landmark", pack.landmarks[0].name);
}

/* utc_offset_min: a wrong-typed value must NOT flip utc_offset_assumed to
 * false (that flag is ff_shell.c's S18 wall-clock-trust signal — a bad
 * offset outranking the user's manual setting would be a real regression,
 * not a cosmetic one). It falls through to the same default path as an
 * absent field. */
static void S05_review_string_utc_offset_min_stays_assumed(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("wrong_type_utc_offset.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);
    TEST_ASSERT_EQUAL_INT16(-240, pack.utc_offset_min);
    TEST_ASSERT_TRUE(pack.utc_offset_assumed);
}

/* A quoted integer where a plain (unflagged) numeric field is expected —
 * `year` has no downstream "known" flag, so the existing lenient fp_num()
 * default is the correct, unchanged behavior: parse still succeeds, year
 * reads as the documented default (0) rather than crashing or erroring
 * the whole pack. This is the deliberate contrast with the flagged sites
 * above: fp_num() (lenient) vs. fp_num_checked() (strict) is a per-field
 * choice, not a blanket one — see fp_num()'s comment in fp_pack.c. */
static void S05_review_quoted_integer_year_defaults_without_error(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("wrong_type_year.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_OK, r);
    TEST_ASSERT_EQUAL_UINT16(0, pack.year);
    TEST_ASSERT_EQUAL_UINT8(1, pack.n_stages); /* rest of the pack parses fine */
}

/* A boolean literal where a polygon point's number is expected. Before
 * this fix, fp_parse_polygon()'s type check accepted any JSMN_PRIMITIVE
 * (true/false included) and fp_num() would then silently substitute 0.0
 * on strtod failure — a `[true, -84.5]` point would have quietly become
 * (0,0) input to ff_geo_project() and shipped as if it were real
 * geometry. fp_num_checked() closes that: a non-numeric primitive fails
 * the whole pack (FP_ERR_JSON), consistent with the object-format-point
 * rejection already covered above. */
static void S05_review_boolean_polygon_point_returns_err_json(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("boolean_polygon_point.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, len, &pack, s_toks, FP_MAX_TOKENS);
    TEST_ASSERT_EQUAL_INT(FP_ERR_JSON, r);
}

/* ======================================================================
 * AC6 — struct size budget (also enforced at compile time in fp_pack.h).
 * ==================================================================== */

static void S05_AC6_pack_struct_fits_48kb_budget(void)
{
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(48u * 1024u, (uint32_t)sizeof(fp_pack_t));
}

/* ======================================================================
 * S26 slice (a) — AC2: a too-small caller-supplied `ntoks` returns the
 * existing FP_ERR_TOO_BIG error, exactly like an oversized document
 * running out of arena — never an overrun. docs/specs/S26-device-
 * lifecycle.md.
 * ==================================================================== */

/* The real fixture below tokenizes into far more than 2 jsmntok_t
 * elements. `tiny` is malloc'd to EXACTLY 2 elements — no slack — so if
 * fp_parse ever wrote past what `ntoks` promised it could use, that
 * write lands past the allocation and address-sanitizer (or a debug
 * malloc) catches it immediately rather than silently landing in
 * adjacent heap memory. This is the actual proxy check for "never
 * overrun": a buffer sized loosely could pass while still overrunning
 * its *stated* capacity, so the buffer here is sized to prove the real
 * property. */
static void S26_AC2_too_small_ntoks_returns_err_too_big_no_overrun(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("lost-lands-2026.festpack.json", buf, sizeof(buf));

    jsmntok_t *tiny = malloc(2 * sizeof(jsmntok_t));
    TEST_ASSERT_NOT_NULL(tiny);

    fp_pack_t pack;
    memset(&pack, 0xAA, sizeof(pack)); /* poison, to prove fp_parse re-zeros on error */
    fp_result_t r = fp_parse(buf, len, &pack, tiny, 2);
    TEST_ASSERT_EQUAL_INT(FP_ERR_TOO_BIG, r);
    TEST_ASSERT_EQUAL_UINT8(0, pack.n_stages);
    TEST_ASSERT_EQUAL_UINT16(0, pack.n_sets);

    free(tiny);
}

/* NULL toks / ntoks<=0 are the same "no usable scratch" case as running
 * out mid-parse — FP_ERR_TOO_BIG, not a NULL-deref crash. */
static void S26_AC2_null_or_nonpositive_toks_returns_err_too_big_no_crash(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("lost-lands-2026.festpack.json", buf, sizeof(buf));
    fp_pack_t pack;

    TEST_ASSERT_EQUAL_INT(FP_ERR_TOO_BIG, fp_parse(buf, len, &pack, NULL, FP_MAX_TOKENS));
    TEST_ASSERT_EQUAL_INT(FP_ERR_TOO_BIG, fp_parse(buf, len, &pack, s_toks, 0));
    TEST_ASSERT_EQUAL_INT(FP_ERR_TOO_BIG, fp_parse(buf, len, &pack, s_toks, -1));
}

/* AC1 — the static array is gone (this is a compile-time/structural fact
 * checked by review + `nm`/`idf.py size`, not a runtime assertion), but
 * the flip side IS runtime-checkable: fp_parse is now reentrant-capable
 * because distinct calls can use distinct buffers with no shared state.
 * Parsing the same fixture through two different caller-owned buffers
 * back to back, with no crash and identical results, is the behavioral
 * proof that nothing static survives between calls. */
static void S26_AC1_distinct_caller_buffers_parse_independently(void)
{
    char buf[FIXTURE_BUF_SZ];
    size_t len = load_fixture("lost-lands-2026.festpack.json", buf, sizeof(buf));

    static jsmntok_t toks_a[FP_MAX_TOKENS];
    static jsmntok_t toks_b[FP_MAX_TOKENS];
    fp_pack_t pack_a, pack_b;

    TEST_ASSERT_EQUAL_INT(FP_OK, fp_parse(buf, len, &pack_a, toks_a, FP_MAX_TOKENS));
    TEST_ASSERT_EQUAL_INT(FP_OK, fp_parse(buf, len, &pack_b, toks_b, FP_MAX_TOKENS));
    TEST_ASSERT_EQUAL_UINT8(pack_a.n_stages, pack_b.n_stages);
    TEST_ASSERT_EQUAL_UINT16(pack_a.n_sets, pack_b.n_sets);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S05_AC1_lost_lands_has_7_stages_exact_names_and_colors);
    RUN_TEST(S05_AC1_lost_lands_has_at_least_27_sets_all_times_null);
    RUN_TEST(S05_AC1_lost_lands_festival_meta_and_utc_offset_default);

    RUN_TEST(S05_AC2_nulls_in_every_nullable_slot_parse);
    RUN_TEST(S05_AC2_absent_optional_sections_parse);

    RUN_TEST(S05_AC3_wrong_version_returns_err_version);
    RUN_TEST(S05_AC3_missing_version_returns_err_version);
    RUN_TEST(S05_AC3_truncated_json_returns_err_json);
    RUN_TEST(S05_AC3_non_json_returns_err_json);
    RUN_TEST(S05_AC3_null_and_empty_input_return_err_json_no_crash);
    RUN_TEST(S05_AC3_fuzz_smoke_10000_iterations_no_crash);
    RUN_TEST(S05_AC3_deeply_nested_input_returns_err_json_no_crash);

    RUN_TEST(S05_review_object_format_polygon_point_returns_err_json);

    RUN_TEST(S05_AC4_13_stages_returns_err_too_big);
    RUN_TEST(S05_AC4_257_sets_returns_err_too_big);
    RUN_TEST(S05_AC4_25_features_returns_err_too_big);
    RUN_TEST(S05_AC4_13_landmarks_returns_err_too_big);
    RUN_TEST(S05_AC4_25_polygon_points_returns_err_too_big);
    RUN_TEST(S05_AC4_at_cap_12_stages_returns_ok);
    RUN_TEST(S05_AC4_at_cap_256_sets_returns_ok);
    RUN_TEST(S05_AC4_at_cap_24_features_returns_ok);
    RUN_TEST(S05_AC4_at_cap_12_landmarks_returns_ok);
    RUN_TEST(S05_AC4_at_cap_24_polygon_points_returns_ok);

    RUN_TEST(S05_AC5_feature_polygon_projects_known_square_within_1m);

    RUN_TEST(S05_review_null_venue_position_sets_origin_known_false);

    RUN_TEST(S05_review_string_origin_lat_sets_origin_known_false);
    RUN_TEST(S05_review_string_landmark_lat_sets_has_pos_false);
    RUN_TEST(S05_review_string_utc_offset_min_stays_assumed);
    RUN_TEST(S05_review_quoted_integer_year_defaults_without_error);
    RUN_TEST(S05_review_boolean_polygon_point_returns_err_json);

    RUN_TEST(S05_AC6_pack_struct_fits_48kb_budget);

    RUN_TEST(S26_AC2_too_small_ntoks_returns_err_too_big_no_overrun);
    RUN_TEST(S26_AC2_null_or_nonpositive_toks_returns_err_too_big_no_crash);
    RUN_TEST(S26_AC1_distinct_caller_buffers_parse_independently);

    return UNITY_END();
}
