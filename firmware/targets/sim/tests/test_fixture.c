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
    TEST_ASSERT_EQUAL_INT(FF_APP_FLARE_IDLE, s.flare.state);
    TEST_ASSERT_EQUAL_INT32(-1, s.flare.expires_in_ms);
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
    TEST_ASSERT_EQUAL_INT(FF_APP_FLARE_IDLE, s.flare.state);
    TEST_ASSERT_EQUAL_INT32(-1, s.flare.expires_in_ms);
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

static void flare_section_parses_every_field(void)
{
    ff_app_state_t s;
    char const *json = "{\"flare\": {\"state\": \"received\", \"from_name\": \"MAX\", \"expires_in_ms\": 4200}}";
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_json(json, strlen(json), &s));

    TEST_ASSERT_EQUAL_INT(FF_APP_FLARE_RECEIVED, s.flare.state);
    TEST_ASSERT_EQUAL_STRING("MAX", s.flare.from_name);
    TEST_ASSERT_EQUAL_INT32(4200, s.flare.expires_in_ms);
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

/* ---------------------------------------------------------------------
 * S13c — ff_fixture_dump_json: dump -> reload round-trips, per this
 * slice's ctl socket doc comment ("dumps round-trip as fixtures").
 * ------------------------------------------------------------------- */

static void dump_then_reload_round_trips_committed_fixture(void)
{
    ff_app_state_t original;
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_file(fixture_path("radar_live.json"), &original));

    char json[FF_FIXTURE_DUMP_MAX];
    int n = ff_fixture_dump_json(&original, json, sizeof(json));
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)strlen(json), (uint32_t)n);

    ff_app_state_t reloaded;
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_json(json, (size_t)n, &reloaded));

    /* Both structs came from memset(0)-then-parse, so padding bytes are
     * deterministically zero on both sides — a whole-struct compare is
     * the strongest statement of "round-trips exactly" available and
     * catches any field this test forgot to name individually. */
    TEST_ASSERT_EQUAL_MEMORY(&original, &reloaded, sizeof(original));
}

/* Mutation check: if fw_json_str ever stopped escaping `"`/`\`, this test
 * would fail loudly (either ff_fixture_load_json would reject the
 * corrupted JSON outright, or — worse, silently — the reloaded string
 * would come back truncated/shifted at the unescaped character) rather
 * than the escaping bug going unnoticed. */
static void dump_escapes_quotes_and_backslashes_in_names(void)
{
    ff_app_state_t original;
    memset(&original, 0, sizeof(original));
    original.radar.mode = RADAR_LIVE;
    (void)snprintf(original.radar.name, sizeof(original.radar.name), "\"Q\\R\"");
    original.signals.n_items = 1;
    original.signals.items[0].kind = FF_APP_FEED_TEXT;
    (void)snprintf(original.signals.items[0].from_name, sizeof(original.signals.items[0].from_name), "A\"B");
    (void)snprintf(original.signals.items[0].text, sizeof(original.signals.items[0].text),
                    "line1\nline2\ttab\\slash");

    char json[FF_FIXTURE_DUMP_MAX];
    int n = ff_fixture_dump_json(&original, json, sizeof(json));
    TEST_ASSERT_GREATER_THAN_INT(0, n);

    ff_app_state_t reloaded;
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_json(json, (size_t)n, &reloaded));

    TEST_ASSERT_EQUAL_STRING("\"Q\\R\"", reloaded.radar.name);
    TEST_ASSERT_EQUAL_STRING("A\"B", reloaded.signals.items[0].from_name);
    TEST_ASSERT_EQUAL_STRING("line1\nline2\ttab\\slash", reloaded.signals.items[0].text);
}

static void dump_maximally_populated_state_fits_budget(void)
{
    ff_app_state_t s;
    memset(&s, 0, sizeof(s));
    (void)snprintf(s.fixture_name, sizeof(s.fixture_name), "%s", "0123456789012345678901234567890");
    s.active_face = FF_APP_FACE_SIGNALS;

    s.radar.mode = RADAR_CLOSE;
    s.radar.n_dots = FF_CREW_MAX;
    for (uint8_t i = 0; i < FF_CREW_MAX; i++) {
        s.radar.dots[i].ring_deg = 359.5f;
        s.radar.dots[i].initial = 'A' + (char)i;
        s.radar.dots[i].color_idx = i;
        s.radar.dots[i].stale = (i % 2) == 0;
    }

    s.now.n_rows = FF_APP_NOW_MAX_ROWS;
    for (uint8_t i = 0; i < FF_APP_NOW_MAX_ROWS; i++) {
        (void)snprintf(s.now.rows[i].artist, sizeof(s.now.rows[i].artist), "%s", "Artist Name Thirty One Chars!!");
        (void)snprintf(s.now.rows[i].stage_name, sizeof(s.now.rows[i].stage_name), "%s",
                        "Stage Name Twenty Seven Ch"); /* fits FF_APP_STAGE_LEN (28) - 1 for the NUL */
        s.now.rows[i].stage_color_rgb = 0x00FFC66B;
        s.now.rows[i].mins_left = 999;
        s.now.rows[i].pct_done = 100;
    }
    s.now.next.valid = true;
    (void)snprintf(s.now.next.artist, sizeof(s.now.next.artist), "%s", "Artist Name Thirty One Chars!!");

    s.signals.n_items = FF_APP_SIGNALS_MAX_ITEMS;
    for (uint8_t i = 0; i < FF_APP_SIGNALS_MAX_ITEMS; i++) {
        s.signals.items[i].kind = FF_APP_FEED_STATUS;
        /* 63 chars: FF_APP_TEXT_LEN (64) - 1 for the NUL. */
        (void)snprintf(s.signals.items[i].text, sizeof(s.signals.items[i].text), "%s",
                        "012345678901234567890123456789012345678901234567890123456789012");
        s.signals.items[i].unread = true;
    }

    char json[FF_FIXTURE_DUMP_MAX];
    int n = ff_fixture_dump_json(&s, json, sizeof(json));
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_LESS_OR_EQUAL_INT((int)FF_FIXTURE_DUMP_MAX, n);

    ff_app_state_t reloaded;
    TEST_ASSERT_EQUAL_INT(FF_FIXTURE_OK, ff_fixture_load_json(json, (size_t)n, &reloaded));
    TEST_ASSERT_EQUAL_MEMORY(&s, &reloaded, sizeof(s));
}

static void dump_buffer_too_small_returns_negative(void)
{
    ff_app_state_t s;
    memset(&s, 0, sizeof(s));
    s.radar.mode = RADAR_LIVE;

    char tiny[4];
    TEST_ASSERT_EQUAL_INT(-1, ff_fixture_dump_json(&s, tiny, sizeof(tiny)));
}

static void dump_null_args_return_negative(void)
{
    ff_app_state_t s;
    memset(&s, 0, sizeof(s));
    char buf[64];
    TEST_ASSERT_EQUAL_INT(-1, ff_fixture_dump_json(NULL, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(-1, ff_fixture_dump_json(&s, NULL, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(-1, ff_fixture_dump_json(&s, buf, 0));
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

    RUN_TEST(dump_then_reload_round_trips_committed_fixture);
    RUN_TEST(dump_escapes_quotes_and_backslashes_in_names);
    RUN_TEST(dump_maximally_populated_state_fits_budget);
    RUN_TEST(dump_buffer_too_small_returns_negative);
    RUN_TEST(dump_null_args_return_negative);

    return UNITY_END();
}
