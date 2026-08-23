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
#include "ff_crew.h" /* FF_CREW_MAX — radar.dots[] cap */

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

    TEST_ASSERT_EQUAL_INT(RADAR_LIVE, s.radar.mode);
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
    TEST_ASSERT_FALSE(s.flare.sending);
    TEST_ASSERT_EQUAL_INT32(-1, s.flare.send_expires_in_ms);
    TEST_ASSERT_FALSE(s.flare.takeover_active);
    TEST_ASSERT_EQUAL_INT32(-1, s.flare.takeover_expires_in_ms);
    TEST_ASSERT_FALSE(s.flare.locked);
    TEST_ASSERT_EQUAL_INT32(-1, s.flare.locked_expires_in_ms);
}

static void radar_stale_parses_exact_values(void)
{
    ff_app_state_t s;
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_file(fixture_path("radar_stale.json"), &s));

    TEST_ASSERT_EQUAL_INT(RADAR_STALE, s.radar.mode);
    TEST_ASSERT_EQUAL_STRING("320 m", s.radar.dist_str); /* last known distance, honestly stale */
    TEST_ASSERT_EQUAL_STRING("4 MIN", s.radar.age_str);
    TEST_ASSERT_TRUE(s.radar.arrow_valid); /* STALE still draws a (dashed) arrow per S06 */
}

static void radar_close_parses_exact_values(void)
{
    ff_app_state_t s;
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_file(fixture_path("radar_close.json"), &s));

    TEST_ASSERT_EQUAL_INT(RADAR_CLOSE, s.radar.mode);
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
    TEST_ASSERT_EQUAL_INT(RADAR_NOSEL, s.radar.mode); /* documented default */
    TEST_ASSERT_EQUAL_UINT8(0, s.radar.n_dots);
    TEST_ASSERT_FALSE(s.radar.mesh_ok);
    TEST_ASSERT_FALSE(s.flare.sending);
    TEST_ASSERT_EQUAL_INT32(-1, s.flare.send_expires_in_ms);
    TEST_ASSERT_FALSE(s.flare.takeover_active);
    TEST_ASSERT_EQUAL_INT32(-1, s.flare.takeover_expires_in_ms);
    TEST_ASSERT_FALSE(s.flare.locked);
    TEST_ASSERT_EQUAL_INT32(-1, s.flare.locked_expires_in_ms);
}

static void unknown_keys_are_tolerated(void)
{
    ff_app_state_t s;
    char const *json = "{\"fixture\": \"x\", \"totally_unknown_key\": {\"a\": [1,2,3]}, "
                        "\"radar\": {\"mode\": \"live\", \"also_unknown\": true}}";
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_json(json, strlen(json), &s));
    TEST_ASSERT_EQUAL_INT(RADAR_LIVE, s.radar.mode);
}

/* ---------------------------------------------------------------------
 * stage_color_rgb (PR #12 review finding #1): the documented "#rrggbb"
 * string form used to route through fx_num(), which only handles
 * JSMN_PRIMITIVE tokens — a JSON string token was silently rejected,
 * yielding 0x000000 with FF_FIXTURE_OK and no error signal anywhere.
 * `now_stage_color_rgb_hex_string_parses` below is the reviewer's exact
 * repro, now asserting the correct 0xffc66b.
 * ------------------------------------------------------------------- */

static void now_stage_color_rgb_hex_string_parses(void)
{
    ff_app_state_t s;
    char const *json = "{\"now\": {\"rows\": [{\"stage_color_rgb\": \"#ffc66b\"}]}}";
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_json(json, strlen(json), &s));
    TEST_ASSERT_EQUAL_UINT8(1, s.now.n_rows);
    TEST_ASSERT_EQUAL_HEX32(0xffc66bu, s.now.rows[0].stage_color_rgb);
}

static void now_stage_color_rgb_numeric_form_parses(void)
{
    ff_app_state_t s;
    /* 16762475 decimal == 0xFFC66B, same color as the string-form test
     * above — the README documents both forms as accepted. */
    char const *json = "{\"now\": {\"rows\": [{\"stage_color_rgb\": 16762475}]}}";
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_json(json, strlen(json), &s));
    TEST_ASSERT_EQUAL_HEX32(0xffc66bu, s.now.rows[0].stage_color_rgb);
}

static void now_stage_color_rgb_malformed_hex_falls_back_to_zero(void)
{
    ff_app_state_t s;
    char const *json = "{\"now\": {\"rows\": [{\"stage_color_rgb\": \"#zzzzzz\"}]}}";
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_json(json, strlen(json), &s));
    TEST_ASSERT_EQUAL_HEX32(0u, s.now.rows[0].stage_color_rgb);
}

/* ---------------------------------------------------------------------
 * Section coverage (PR #12 review finding #2): one happy-path fixture
 * per section, asserted field-by-field. Previously only `radar` had any
 * coverage at all — exactly how finding #1 went unnoticed.
 * ------------------------------------------------------------------- */

static void now_section_parses_every_field(void)
{
    ff_app_state_t s;
    char const *json = "{\"now\": {"
                        "  \"rows\": [{\"artist\": \"GRiZ\", \"stage_name\": \"Bass Camp\", "
                        "               \"stage_color_rgb\": \"#ffc66b\", \"mins_left\": 12, \"pct_done\": 60}],"
                        "  \"next\": {\"artist\": \"Subtronics\", \"stage_name\": \"Grand Illusion\", "
                        "             \"mins_until\": 45},"
                        "  \"tbd\": false"
                        "}}";
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_json(json, strlen(json), &s));

    TEST_ASSERT_EQUAL_UINT8(1, s.now.n_rows);
    TEST_ASSERT_EQUAL_STRING("GRiZ", s.now.rows[0].artist);
    TEST_ASSERT_EQUAL_STRING("Bass Camp", s.now.rows[0].stage_name);
    TEST_ASSERT_EQUAL_HEX32(0xffc66bu, s.now.rows[0].stage_color_rgb);
    TEST_ASSERT_EQUAL_INT(12, s.now.rows[0].mins_left);
    TEST_ASSERT_EQUAL_UINT8(60, s.now.rows[0].pct_done);

    TEST_ASSERT_TRUE(s.now.next.valid);
    TEST_ASSERT_EQUAL_STRING("Subtronics", s.now.next.artist);
    TEST_ASSERT_EQUAL_STRING("Grand Illusion", s.now.next.stage_name);
    TEST_ASSERT_EQUAL_INT(45, s.now.next.mins_until);

    TEST_ASSERT_FALSE(s.now.tbd);
}

static void signals_section_parses_every_field(void)
{
    ff_app_state_t s;
    char const *json = "{\"signals\": {"
                        "  \"items\": [{\"kind\": \"pulse\", \"from_name\": \"RILEY\", \"text\": \"omw\", "
                        "               \"age_str\": \"2 MIN\", \"unread\": true}],"
                        "  \"unread_count\": 1"
                        "}}";
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_json(json, strlen(json), &s));

    TEST_ASSERT_EQUAL_UINT8(1, s.signals.n_items);
    TEST_ASSERT_EQUAL_INT(FF_APP_FEED_PULSE, s.signals.items[0].kind);
    TEST_ASSERT_EQUAL_STRING("RILEY", s.signals.items[0].from_name);
    TEST_ASSERT_EQUAL_STRING("omw", s.signals.items[0].text);
    TEST_ASSERT_EQUAL_STRING("2 MIN", s.signals.items[0].age_str);
    TEST_ASSERT_TRUE(s.signals.items[0].unread);
    TEST_ASSERT_EQUAL_UINT8(1, s.signals.unread_count);
}

/* [api] S10 slice b — ff_app_flare_t's three independent groups (see
 * ff_app_state.h's updated doc comment). This is the reviewer-repro shape
 * itself: a fixture with `sending`, `takeover_active`, AND `locked` ALL
 * true at once, for DIFFERENT crew members, is exactly the legitimate
 * simultaneous state PR #15's Ruling 1/2 exist to allow (single-slot
 * `state` enum couldn't have represented this fixture at all). */
static void flare_section_parses_every_field(void)
{
    ff_app_state_t s;
    char const *json = "{\"flare\": {"
                        "  \"sending\": true, \"send_expires_in_ms\": 120000,"
                        "  \"takeover_active\": true, \"takeover_from_name\": \"KEV\","
                        "  \"takeover_bearing_valid\": true, \"takeover_bearing_deg\": 90.0,"
                        "  \"takeover_dist_str\": \"40 m\","
                        "  \"takeover_expires_in_ms\": 4200,"
                        "  \"locked\": true, \"locked_from_name\": \"DANA\", \"locked_expires_in_ms\": 9000"
                        "}}";
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_json(json, strlen(json), &s));

    TEST_ASSERT_TRUE(s.flare.sending);
    TEST_ASSERT_EQUAL_INT32(120000, s.flare.send_expires_in_ms);

    TEST_ASSERT_TRUE(s.flare.takeover_active);
    TEST_ASSERT_EQUAL_STRING("KEV", s.flare.takeover_from_name);
    TEST_ASSERT_TRUE(s.flare.takeover_bearing_valid);
    TEST_ASSERT_EQUAL_FLOAT(90.0f, s.flare.takeover_bearing_deg);
    TEST_ASSERT_EQUAL_STRING("40 m", s.flare.takeover_dist_str);
    TEST_ASSERT_EQUAL_INT32(4200, s.flare.takeover_expires_in_ms);

    TEST_ASSERT_TRUE(s.flare.locked);
    TEST_ASSERT_EQUAL_STRING("DANA", s.flare.locked_from_name);
    TEST_ASSERT_EQUAL_INT32(9000, s.flare.locked_expires_in_ms);
}

/* PR #20 code review (LOW finding): `takeover_bearing_valid` defaults to
 * false ("unknown") both when the whole `flare` section is absent AND
 * when the takeover group is present but this one key is omitted —
 * providing `takeover_bearing_deg` without the validity flag must NOT be
 * read as "valid" (a fixture author forgetting the flag should get an
 * honest "unknown" render, not a silently-fabricated compass point). */
static void flare_takeover_bearing_valid_defaults_false(void)
{
    ff_app_state_t s;
    char const *json = "{\"flare\": {\"takeover_active\": true, \"takeover_bearing_deg\": 90.0}}";
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_json(json, strlen(json), &s));

    TEST_ASSERT_FALSE(s.flare.takeover_bearing_valid);
    TEST_ASSERT_EQUAL_FLOAT(90.0f, s.flare.takeover_bearing_deg); /* parsed regardless — just not honestly usable */
}

/* Each group's own "n/a" sentinel is independent — omitting only the
 * takeover group must NOT disturb the (present) sending/locked groups'
 * real values, and must still default takeover_expires_in_ms to -1 as if
 * the whole "flare" section were absent (fx_parse_flare's doc comment). */
static void flare_omitted_group_defaults_independently(void)
{
    ff_app_state_t s;
    char const *json = "{\"flare\": {\"sending\": true, \"send_expires_in_ms\": 5000, "
                        "\"locked\": true, \"locked_expires_in_ms\": 9000}}";
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_json(json, strlen(json), &s));

    TEST_ASSERT_TRUE(s.flare.sending);
    TEST_ASSERT_EQUAL_INT32(5000, s.flare.send_expires_in_ms);

    TEST_ASSERT_FALSE(s.flare.takeover_active);
    TEST_ASSERT_EQUAL_INT32(-1, s.flare.takeover_expires_in_ms);

    TEST_ASSERT_TRUE(s.flare.locked);
    TEST_ASSERT_EQUAL_INT32(9000, s.flare.locked_expires_in_ms);
}

static void settings_section_parses_every_field(void)
{
    ff_app_state_t s;
    char const *json = "{\"settings\": {"
                        "  \"imperial\": false, \"share_mode\": \"ghost\", \"haptics\": false, "
                        "  \"night_glow\": false, \"water_min\": 120, \"quiet_from_min\": 0, "
                        "  \"quiet_to_min\": 480, \"my_name\": \"DANA\""
                        "}}";
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_json(json, strlen(json), &s));

    TEST_ASSERT_FALSE(s.settings.imperial);
    TEST_ASSERT_EQUAL_UINT8(2, s.settings.share_mode); /* FF_SHARE_GHOST */
    TEST_ASSERT_FALSE(s.settings.haptics);
    TEST_ASSERT_FALSE(s.settings.night_glow);
    TEST_ASSERT_EQUAL_UINT16(120, s.settings.water_min);
    TEST_ASSERT_EQUAL_UINT16(0, s.settings.quiet_from_min);
    TEST_ASSERT_EQUAL_UINT16(480, s.settings.quiet_to_min);
    TEST_ASSERT_EQUAL_STRING("DANA", s.settings.my_name);
}

/* ---------------------------------------------------------------------
 * Fail-loud on oversized arrays (PR #12 review finding #3, orchestrator
 * ruling on deviation #6): a section array beyond its documented cap
 * must reject the whole load with FF_FIXTURE_ERR_TOO_BIG, not silently
 * truncate. These are also the mutation-test guard the review asked
 * for: each capped parser's over-cap check runs BEFORE any array
 * writes, so if that check is ever deleted, the function stops
 * returning FF_FIXTURE_ERR_TOO_BIG for these fixtures and the assertion
 * below fails — no ASan required to notice the regression.
 * ------------------------------------------------------------------- */

/* Builds {"<section_key>": {"<array_key>": [{},{},...,{}] }} with `n`
 * empty-object entries — field content doesn't matter for a cap test,
 * only the count. */
static void build_n_element_array_json(char *buf, size_t buf_sz, char const *section_key, char const *array_key,
                                        int n)
{
    size_t pos = 0;
    int written = snprintf(buf + pos, buf_sz - pos, "{\"%s\": {\"%s\": [", section_key, array_key);
    pos += (size_t)written;
    for (int i = 0; i < n && pos < buf_sz; i++) {
        written = snprintf(buf + pos, buf_sz - pos, "%s{}", i == 0 ? "" : ",");
        pos += (size_t)written;
    }
    snprintf(buf + pos, buf_sz - pos, "]}}");
}

static void radar_dots_over_cap_fails_loud(void)
{
    char json[512];
    build_n_element_array_json(json, sizeof(json), "radar", "dots", FF_CREW_MAX + 1);

    ff_app_state_t s;
    memset(&s, 0xAA, sizeof(s));
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_ERR_TOO_BIG, ff_fixture_load_json(json, strlen(json), &s));

    ff_app_state_t zero;
    memset(&zero, 0, sizeof(zero));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &s, sizeof(s));
}

static void radar_dots_at_cap_still_loads_ok(void)
{
    char json[512];
    build_n_element_array_json(json, sizeof(json), "radar", "dots", FF_CREW_MAX);

    ff_app_state_t s;
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_json(json, strlen(json), &s));
    TEST_ASSERT_EQUAL_UINT8(FF_CREW_MAX, s.radar.n_dots);
}

static void now_rows_over_cap_fails_loud(void)
{
    char json[512];
    build_n_element_array_json(json, sizeof(json), "now", "rows", FF_APP_NOW_MAX_ROWS + 1);

    ff_app_state_t s;
    memset(&s, 0xAA, sizeof(s));
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_ERR_TOO_BIG, ff_fixture_load_json(json, strlen(json), &s));

    ff_app_state_t zero;
    memset(&zero, 0, sizeof(zero));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &s, sizeof(s));
}

static void signals_items_over_cap_fails_loud(void)
{
    char json[1024];
    build_n_element_array_json(json, sizeof(json), "signals", "items", FF_APP_SIGNALS_MAX_ITEMS + 1);

    ff_app_state_t s;
    memset(&s, 0xAA, sizeof(s));
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_ERR_TOO_BIG, ff_fixture_load_json(json, strlen(json), &s));

    ff_app_state_t zero;
    memset(&zero, 0, sizeof(zero));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &s, sizeof(s));
}

static void oversized_array_zeroes_entire_state_including_other_sections(void)
{
    /* radar.dots is over cap; "settings" appears earlier in the raw
     * JSON text, but ff_fixture_load_json parses sections in a fixed
     * internal order (radar, then now/signals/flare/settings) — this
     * exercises that a failure anywhere still yields a *fully* zeroed
     * struct, not just the section that failed, regardless of where in
     * the document the failing section appears. */
    char dots[128] = "";
    for (int i = 0; i < FF_CREW_MAX + 1; i++) {
        strcat(dots, i == 0 ? "{}" : ",{}");
    }
    char json[512];
    snprintf(json, sizeof(json), "{\"settings\": {\"my_name\": \"SHOULD_NOT_SURVIVE\"}, \"radar\": {\"dots\": [%s]}}",
             dots);

    ff_app_state_t s;
    memset(&s, 0xAA, sizeof(s));
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_ERR_TOO_BIG, ff_fixture_load_json(json, strlen(json), &s));

    ff_app_state_t zero;
    memset(&zero, 0, sizeof(zero));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &s, sizeof(s));
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

    RUN_TEST(now_stage_color_rgb_hex_string_parses);
    RUN_TEST(now_stage_color_rgb_numeric_form_parses);
    RUN_TEST(now_stage_color_rgb_malformed_hex_falls_back_to_zero);

    RUN_TEST(now_section_parses_every_field);
    RUN_TEST(signals_section_parses_every_field);
    RUN_TEST(flare_section_parses_every_field);
    RUN_TEST(flare_omitted_group_defaults_independently);
    RUN_TEST(flare_takeover_bearing_valid_defaults_false);
    RUN_TEST(settings_section_parses_every_field);

    RUN_TEST(radar_dots_over_cap_fails_loud);
    RUN_TEST(radar_dots_at_cap_still_loads_ok);
    RUN_TEST(now_rows_over_cap_fails_loud);
    RUN_TEST(signals_items_over_cap_fails_loud);
    RUN_TEST(oversized_array_zeroes_entire_state_including_other_sections);

    RUN_TEST(stem_strips_dir_and_json_extension);
    RUN_TEST(stem_handles_bare_filename);
    RUN_TEST(stem_leaves_non_json_extension_alone);
    RUN_TEST(stem_handles_null_path);

    return UNITY_END();
}
