/**
 * test_store_file.c — sim target's trivial file-backed ff_store_t
 * (spec S11 slice a: "a trivial file-backed store for the sim ...
 * with its own test").
 *
 * Covers the store_file.c contract directly (missing key, round trip,
 * overwrite, buffer-too-small, multiple keys in one file) plus an
 * end-to-end check that ff_settings_load/save round-trip correctly
 * through this real backing store (not just the in-memory mock used by
 * core/tests/test_settings.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"

#include "ff_settings.h"
#include "ff_store.h"
#include "store_file.h"

static char g_path[512];
static char g_tmp_path[600];

static void ff_remove_test_files(void)
{
    remove(g_path);
    remove(g_tmp_path);
}

void setUp(void)
{
    char const *tmp_dir = getenv("TMPDIR");
    if (tmp_dir == NULL || tmp_dir[0] == '\0') {
        tmp_dir = "/tmp";
    }
    snprintf(g_path, sizeof(g_path), "%s/ff_store_file_test_%d.kv", tmp_dir, (int)getpid());
    snprintf(g_tmp_path, sizeof(g_tmp_path), "%s.tmp", g_path);
    ff_remove_test_files();
}

void tearDown(void)
{
    ff_remove_test_files();
}

static void get_on_missing_file_returns_negative(void)
{
    ff_store_file_t fsio;
    ff_store_t st = ff_store_file_init(&fsio, g_path);

    char buf[32];
    TEST_ASSERT_LESS_THAN_INT(0, st.get(st.io, "anykey", buf, sizeof(buf)));
}

static void get_on_missing_key_returns_negative(void)
{
    ff_store_file_t fsio;
    ff_store_t st = ff_store_file_init(&fsio, g_path);

    TEST_ASSERT_TRUE(st.set(st.io, "present", "x", 1) >= 0);

    char buf[32];
    TEST_ASSERT_LESS_THAN_INT(0, st.get(st.io, "absent", buf, sizeof(buf)));
}

static void set_then_get_round_trips_value(void)
{
    ff_store_file_t fsio;
    ff_store_t st = ff_store_file_init(&fsio, g_path);

    const char *value = "hello, festival";
    size_t value_len = strlen(value);

    int set_rc = st.set(st.io, "greeting", value, value_len);
    TEST_ASSERT_EQUAL_INT((int)value_len, set_rc);

    char buf[64];
    int get_rc = st.get(st.io, "greeting", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT((int)value_len, get_rc);
    TEST_ASSERT_EQUAL_MEMORY(value, buf, value_len);
}

static void set_overwrites_existing_key_without_duplication(void)
{
    ff_store_file_t fsio;
    ff_store_t st = ff_store_file_init(&fsio, g_path);

    TEST_ASSERT_TRUE(st.set(st.io, "k", "first", 5) >= 0);
    TEST_ASSERT_TRUE(st.set(st.io, "k", "second-value", 12) >= 0);

    char buf[32];
    int rc = st.get(st.io, "k", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(12, rc);
    TEST_ASSERT_EQUAL_MEMORY("second-value", buf, 12);
}

static void multiple_keys_coexist_in_one_file(void)
{
    ff_store_file_t fsio;
    ff_store_t st = ff_store_file_init(&fsio, g_path);

    TEST_ASSERT_TRUE(st.set(st.io, "alpha", "AAA", 3) >= 0);
    TEST_ASSERT_TRUE(st.set(st.io, "beta", "BBBB", 4) >= 0);
    TEST_ASSERT_TRUE(st.set(st.io, "gamma", "C", 1) >= 0);

    char buf[16];
    TEST_ASSERT_EQUAL_INT(3, st.get(st.io, "alpha", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY("AAA", buf, 3);
    TEST_ASSERT_EQUAL_INT(4, st.get(st.io, "beta", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY("BBBB", buf, 4);
    TEST_ASSERT_EQUAL_INT(1, st.get(st.io, "gamma", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY("C", buf, 1);

    /* Overwriting the middle key leaves the others intact. */
    TEST_ASSERT_TRUE(st.set(st.io, "beta", "Z", 1) >= 0);
    TEST_ASSERT_EQUAL_INT(3, st.get(st.io, "alpha", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY("AAA", buf, 3);
    TEST_ASSERT_EQUAL_INT(1, st.get(st.io, "beta", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY("Z", buf, 1);
    TEST_ASSERT_EQUAL_INT(1, st.get(st.io, "gamma", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY("C", buf, 1);
}

static void get_with_undersized_buffer_returns_negative(void)
{
    ff_store_file_t fsio;
    ff_store_t st = ff_store_file_init(&fsio, g_path);

    TEST_ASSERT_TRUE(st.set(st.io, "k", "0123456789", 10) >= 0);

    char small_buf[4];
    TEST_ASSERT_LESS_THAN_INT(0, st.get(st.io, "k", small_buf, sizeof(small_buf)));
}

/* ---------------------------------------------------------------------
 * End-to-end: ff_settings_t through the real file-backed store (not the
 * in-memory mock). Reinforces S11_AC2/AC6 against the actual sim seam.
 * ------------------------------------------------------------------- */

static void settings_round_trip_through_real_file_store(void)
{
    ff_store_file_t fsio;
    ff_store_t st = ff_store_file_init(&fsio, g_path);

    ff_settings_t out;
    memset(&out, 0, sizeof(out));
    out.imperial = false;
    out.share_mode = FF_SHARE_GHOST;
    out.haptics = false;
    out.night_glow = true;
    out.water_min = 120;
    out.quiet_from_min = 0;
    out.quiet_to_min = 480;
    strncpy(out.my_name, "Riley", sizeof(out.my_name) - 1);
    out.cal_valid = true;
    for (size_t i = 0; i < FF_SETTINGS_CAL_BLOB_LEN; i++) {
        out.compass_cal_blob[i] = (uint8_t)(255 - i);
    }

    ff_settings_save(&out, &st);

    ff_settings_t in;
    memset(&in, 0xAA, sizeof(in));
    ff_settings_load(&in, &st);

    TEST_ASSERT_EQUAL_MEMORY(&out, &in, sizeof(ff_settings_t));
}

static void settings_load_from_fresh_file_yields_defaults(void)
{
    ff_store_file_t fsio;
    ff_store_t st = ff_store_file_init(&fsio, g_path);

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st); /* no file on disk yet */

    TEST_ASSERT_TRUE(s.imperial);
    TEST_ASSERT_EQUAL_UINT16(90, s.water_min);
    TEST_ASSERT_EQUAL_UINT16(240, s.quiet_from_min);
    TEST_ASSERT_EQUAL_UINT16(600, s.quiet_to_min);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(get_on_missing_file_returns_negative);
    RUN_TEST(get_on_missing_key_returns_negative);
    RUN_TEST(set_then_get_round_trips_value);
    RUN_TEST(set_overwrites_existing_key_without_duplication);
    RUN_TEST(multiple_keys_coexist_in_one_file);
    RUN_TEST(get_with_undersized_buffer_returns_negative);

    RUN_TEST(settings_round_trip_through_real_file_store);
    RUN_TEST(settings_load_from_fresh_file_yields_defaults);

    return UNITY_END();
}
