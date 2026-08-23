/**
 * test_fixture.c — ff_app_state_t JSON fixture loader (S13/S14 slice b).
 *
 * Covers: the three committed radar fixtures parse to the exact values
 * documented in tests/fixtures/README.md; missing-file/malformed-JSON
 * error paths; missing-section defaults; and ff_fixture_stem()'s path
 * handling.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fixture.h"

#ifndef FF_FIXTURE_DIR
#define FF_FIXTURE_DIR "tests/fixtures/"
#endif

static char g_path[512];

static char const *fixture_path(char const *name)
{
    snprintf(g_path, sizeof(g_path), "%s%s", FF_FIXTURE_DIR, name);
    return g_path;
}

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------------------
 * The three committed radar fixtures — exact values, per
 * tests/fixtures/README.md's documented table.
 * ------------------------------------------------------------------- */

static void radar_live_parses_exact_values(void)
{
    ff_app_state_t s;
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_file(fixture_path("radar_live.json"), &s));

    TEST_ASSERT_EQUAL_STRING("radar_live", s.fixture_name);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, s.active_face);

    TEST_ASSERT_EQUAL_INT(FF_APP_RADAR_LIVE, s.radar.mode);
    TEST_ASSERT_EQUAL_FLOAT(42.0f, s.radar.arrow_deg);
    TEST_ASSERT_TRUE(s.radar.arrow_valid);
    TEST_ASSERT_EQUAL_STRING("DANA", s.radar.name);
    TEST_ASSERT_EQUAL_STRING("320 m", s.radar.dist_str);
    TEST_ASSERT_EQUAL_STRING("8 SEC", s.radar.age_str);
    TEST_ASSERT_EQUAL_INT(0, s.radar.trend);
    TEST_ASSERT_EQUAL_STRING("9:41", s.radar.clock_str);
    TEST_ASSERT_EQUAL_INT(78, s.radar.batt_pct);
    TEST_ASSERT_TRUE(s.radar.mesh_ok);

    TEST_ASSERT_EQUAL_UINT8(4, s.radar.n_dots);
    TEST_ASSERT_EQUAL_FLOAT(42.0f, s.radar.dots[0].ring_deg);
    TEST_ASSERT_EQUAL_CHAR('D', s.radar.dots[0].initial);
    TEST_ASSERT_EQUAL_UINT8(0, s.radar.dots[0].color_idx);
    TEST_ASSERT_FALSE(s.radar.dots[0].stale);
    TEST_ASSERT_TRUE(s.radar.dots[2].stale); /* "M" dot is stale in this fixture */

    /* Sections not present in the JSON stay zeroed, except the
     * documented flare "n/a" sentinel. */
    TEST_ASSERT_EQUAL_UINT8(0, s.now.n_rows);
    TEST_ASSERT_FALSE(s.now.next.valid);
    TEST_ASSERT_EQUAL_UINT8(0, s.signals.n_items);
    TEST_ASSERT_EQUAL_INT(FF_APP_FLARE_IDLE, s.flare.state);
    TEST_ASSERT_EQUAL_INT32(-1, s.flare.expires_in_ms);
}

static void radar_stale_parses_exact_values(void)
{
    ff_app_state_t s;
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_file(fixture_path("radar_stale.json"), &s));

    TEST_ASSERT_EQUAL_INT(FF_APP_RADAR_STALE, s.radar.mode);
    TEST_ASSERT_EQUAL_STRING("320 m", s.radar.dist_str); /* last known distance, honestly stale */
    TEST_ASSERT_EQUAL_STRING("4 MIN", s.radar.age_str);
    TEST_ASSERT_TRUE(s.radar.arrow_valid); /* STALE still draws a (dashed) arrow per S06 */
}

static void radar_close_parses_exact_values(void)
{
    ff_app_state_t s;
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_file(fixture_path("radar_close.json"), &s));

    TEST_ASSERT_EQUAL_INT(FF_APP_RADAR_CLOSE, s.radar.mode);
    TEST_ASSERT_EQUAL_STRING("15 m", s.radar.dist_str);
    TEST_ASSERT_EQUAL_STRING("3 SEC", s.radar.age_str);
    TEST_ASSERT_EQUAL_INT(1, s.radar.trend);
    TEST_ASSERT_FALSE(s.radar.arrow_valid); /* S06: "false in CLOSE/NOFIX/NOSEL" */
}

/* ---------------------------------------------------------------------
 * Error paths.
 * ------------------------------------------------------------------- */

static void missing_file_returns_io_error(void)
{
    ff_app_state_t s;
    memset(&s, 0xAA, sizeof(s));
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_ERR_IO, ff_fixture_load_file("tests/fixtures/does_not_exist.json", &s));

    /* Contract: on any non-OK return, *out is left zeroed, not
     * partially/garbage populated (matches fp_parse()'s contract). */
    ff_app_state_t zero;
    memset(&zero, 0, sizeof(zero));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &s, sizeof(s));
}

static void malformed_json_returns_json_error(void)
{
    ff_app_state_t s;
    memset(&s, 0xAA, sizeof(s));
    char const *bad = "{ this is not valid json";
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_ERR_JSON, ff_fixture_load_json(bad, strlen(bad), &s));

    ff_app_state_t zero;
    memset(&zero, 0, sizeof(zero));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &s, sizeof(s));
}

static void non_object_top_level_returns_json_error(void)
{
    ff_app_state_t s;
    char const *arr = "[1, 2, 3]";
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_ERR_JSON, ff_fixture_load_json(arr, strlen(arr), &s));
}

static void empty_input_returns_json_error(void)
{
    ff_app_state_t s;
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_ERR_JSON, ff_fixture_load_json("", 0, &s));
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_ERR_JSON, ff_fixture_load_json(NULL, 0, &s));
}

/* ---------------------------------------------------------------------
 * Missing-section / unknown-key tolerance.
 * ------------------------------------------------------------------- */

static void absent_sections_default_to_zero(void)
{
    ff_app_state_t s;
    char const *json = "{\"fixture\": \"minimal\"}";
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_json(json, strlen(json), &s));

    TEST_ASSERT_EQUAL_STRING("minimal", s.fixture_name);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, s.active_face); /* documented default */
    TEST_ASSERT_EQUAL_INT(FF_APP_RADAR_NOSEL, s.radar.mode); /* documented default */
    TEST_ASSERT_EQUAL_UINT8(0, s.radar.n_dots);
    TEST_ASSERT_FALSE(s.radar.mesh_ok);
    TEST_ASSERT_EQUAL_INT(FF_APP_FLARE_IDLE, s.flare.state);
    TEST_ASSERT_EQUAL_INT32(-1, s.flare.expires_in_ms);
}

static void unknown_keys_are_tolerated(void)
{
    ff_app_state_t s;
    char const *json = "{\"fixture\": \"x\", \"totally_unknown_key\": {\"a\": [1,2,3]}, "
                        "\"radar\": {\"mode\": \"live\", \"also_unknown\": true}}";
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_json(json, strlen(json), &s));
    TEST_ASSERT_EQUAL_INT(FF_APP_RADAR_LIVE, s.radar.mode);
}

/* ---------------------------------------------------------------------
 * ff_fixture_stem
 * ------------------------------------------------------------------- */

static void stem_strips_dir_and_json_extension(void)
{
    char out[64];
    ff_fixture_stem("tests/fixtures/radar_live.json", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("radar_live", out);
}

static void stem_handles_bare_filename(void)
{
    char out[64];
    ff_fixture_stem("radar_live.json", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("radar_live", out);
}

static void stem_leaves_non_json_extension_alone(void)
{
    char out[64];
    ff_fixture_stem("foo/bar.txt", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("bar.txt", out);
}

static void stem_handles_null_path(void)
{
    char out[64] = "unchanged-sentinel";
    ff_fixture_stem(NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(radar_live_parses_exact_values);
    RUN_TEST(radar_stale_parses_exact_values);
    RUN_TEST(radar_close_parses_exact_values);

    RUN_TEST(missing_file_returns_io_error);
    RUN_TEST(malformed_json_returns_json_error);
    RUN_TEST(non_object_top_level_returns_json_error);
    RUN_TEST(empty_input_returns_json_error);

    RUN_TEST(absent_sections_default_to_zero);
    RUN_TEST(unknown_keys_are_tolerated);

    RUN_TEST(stem_strips_dir_and_json_extension);
    RUN_TEST(stem_handles_bare_filename);
    RUN_TEST(stem_leaves_non_json_extension_alone);
    RUN_TEST(stem_handles_null_path);

    return UNITY_END();
}
