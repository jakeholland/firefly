/**
 * test_session_log.c — S25 latch-hold amendment (2026-09-16): the NVS
 * heartbeat + clean_shutdown flag (ff_session_log.h/.c).
 *
 * Covers:
 *  - load with no prior record (empty store, corrupt/foreign blob) ->
 *    false, zeroed record.
 *  - heartbeat() always clears clean_shutdown; mark_clean() sets it.
 *  - save/load round trip (mock store, one write per save).
 *  - heartbeat_due cadence, including the uint32_t ms wraparound case.
 *  - format_last_time: absent record -> false/empty; clean prior ->
 *    false/empty; unclean prior -> the canonical line, with and without
 *    a known battery reading.
 */
#include <string.h>

#include "unity.h"

#include "ff_session_log.h"
#include "ff_store.h"

#include "support/mock_store.h"

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------------------
 * load — no prior record.
 * ------------------------------------------------------------------- */

static void test_load_empty_store_returns_false_and_zeroed(void)
{
    mock_store_io_t io;
    mock_store_reset(&io);
    ff_store_t st = mock_store_vtable(&io);

    ff_session_log_t rec;
    memset(&rec, 0xAA, sizeof(rec)); /* poison, so a real zero is provable */
    bool const ok = ff_session_log_load(&rec, &st);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_UINT32(0, rec.uptime_s);
    TEST_ASSERT_EQUAL_UINT16(0, rec.batt_mv);
    TEST_ASSERT_EQUAL_UINT8(0, rec.link);
    TEST_ASSERT_EQUAL_UINT8(0, rec.face);
    TEST_ASSERT_FALSE(rec.clean_shutdown);
}

static void test_load_corrupt_blob_returns_false(void)
{
    mock_store_io_t io;
    mock_store_reset(&io);
    ff_store_t st = mock_store_vtable(&io);

    /* Garbage bytes under the right key, wrong size/magic. */
    uint8_t garbage[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    st.set(st.io, "ff.sesslog", garbage, sizeof(garbage));

    ff_session_log_t rec;
    bool const ok = ff_session_log_load(&rec, &st);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_FALSE(rec.clean_shutdown);
}

/* ---------------------------------------------------------------------
 * heartbeat / mark_clean.
 * ------------------------------------------------------------------- */

static void test_heartbeat_sets_fields_and_clears_clean(void)
{
    ff_session_log_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.clean_shutdown = true; /* prove heartbeat() clears it */

    ff_session_log_heartbeat(&rec, 7980u, 3620u, FF_SESSION_LINK_CONNECTED, 3u);

    TEST_ASSERT_EQUAL_UINT32(7980u, rec.uptime_s);
    TEST_ASSERT_EQUAL_UINT16(3620u, rec.batt_mv);
    TEST_ASSERT_EQUAL_UINT8(FF_SESSION_LINK_CONNECTED, rec.link);
    TEST_ASSERT_EQUAL_UINT8(3u, rec.face);
    TEST_ASSERT_FALSE(rec.clean_shutdown);
}

static void test_mark_clean_sets_flag_only(void)
{
    ff_session_log_t rec;
    memset(&rec, 0, sizeof(rec));
    ff_session_log_heartbeat(&rec, 100u, 4000u, FF_SESSION_LINK_NONE, 1u);

    ff_session_log_mark_clean(&rec);

    TEST_ASSERT_TRUE(rec.clean_shutdown);
    TEST_ASSERT_EQUAL_UINT32(100u, rec.uptime_s); /* untouched */
    TEST_ASSERT_EQUAL_UINT16(4000u, rec.batt_mv);
}

/* ---------------------------------------------------------------------
 * save/load round trip.
 * ------------------------------------------------------------------- */

static void test_round_trip_exactly_one_write(void)
{
    mock_store_io_t io;
    mock_store_reset(&io);
    ff_store_t st = mock_store_vtable(&io);

    ff_session_log_t out;
    memset(&out, 0, sizeof(out));
    ff_session_log_heartbeat(&out, 7980u, 3620u, FF_SESSION_LINK_RECONNECTING, 2u);

    ff_session_log_save(&out, &st);
    TEST_ASSERT_EQUAL_INT(1, io.set_calls);

    ff_session_log_t in;
    bool const ok = ff_session_log_load(&in, &st);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(out.uptime_s, in.uptime_s);
    TEST_ASSERT_EQUAL_UINT16(out.batt_mv, in.batt_mv);
    TEST_ASSERT_EQUAL_UINT8(out.link, in.link);
    TEST_ASSERT_EQUAL_UINT8(out.face, in.face);
    TEST_ASSERT_EQUAL(out.clean_shutdown, in.clean_shutdown);
}

static void test_round_trip_preserves_clean_flag(void)
{
    mock_store_io_t io;
    mock_store_reset(&io);
    ff_store_t st = mock_store_vtable(&io);

    ff_session_log_t out;
    memset(&out, 0, sizeof(out));
    ff_session_log_heartbeat(&out, 500u, 3900u, FF_SESSION_LINK_CONNECTED, 0u);
    ff_session_log_mark_clean(&out);
    ff_session_log_save(&out, &st);

    ff_session_log_t in;
    bool const ok = ff_session_log_load(&in, &st);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(in.clean_shutdown);
}

/* ---------------------------------------------------------------------
 * heartbeat_due — cadence + wraparound.
 * ------------------------------------------------------------------- */

static void test_heartbeat_due_cadence(void)
{
    TEST_ASSERT_FALSE(ff_session_log_heartbeat_due(0u, FF_SESSION_LOG_HEARTBEAT_MS - 1u));
    TEST_ASSERT_TRUE(ff_session_log_heartbeat_due(0u, FF_SESSION_LOG_HEARTBEAT_MS));
    TEST_ASSERT_TRUE(ff_session_log_heartbeat_due(0u, FF_SESSION_LOG_HEARTBEAT_MS + 1000u));
}

static void test_heartbeat_due_wraps_safely(void)
{
    /* last_write_ms near UINT32_MAX, now_ms wrapped around to a small
     * value FF_SESSION_LOG_HEARTBEAT_MS later — unsigned subtraction
     * must still read the correct elapsed interval, not a huge one. */
    uint32_t const last = 0xFFFFFFFFu - 1000u;
    uint32_t const now = (uint32_t)(last + FF_SESSION_LOG_HEARTBEAT_MS); /* wraps */
    TEST_ASSERT_TRUE(ff_session_log_heartbeat_due(last, now));

    uint32_t const now_short = (uint32_t)(last + FF_SESSION_LOG_HEARTBEAT_MS - 1u);
    TEST_ASSERT_FALSE(ff_session_log_heartbeat_due(last, now_short));
}

/* ---------------------------------------------------------------------
 * format_last_time.
 * ------------------------------------------------------------------- */

static void test_format_last_time_no_prior_record(void)
{
    char buf[96];
    strcpy(buf, "unchanged");
    bool const shown = ff_session_log_format_last_time(buf, sizeof(buf), NULL, /*prev_valid=*/false);

    TEST_ASSERT_FALSE(shown);
    TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_format_last_time_clean_prior_session(void)
{
    ff_session_log_t prev;
    memset(&prev, 0, sizeof(prev));
    ff_session_log_heartbeat(&prev, 9999u, 4000u, FF_SESSION_LINK_CONNECTED, 1u);
    ff_session_log_mark_clean(&prev);

    char buf[96];
    bool const shown = ff_session_log_format_last_time(buf, sizeof(buf), &prev, /*prev_valid=*/true);

    TEST_ASSERT_FALSE(shown);
    TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_format_last_time_unclean_with_battery(void)
{
    ff_session_log_t prev;
    memset(&prev, 0, sizeof(prev));
    /* 2h13m = 2*3600 + 13*60 = 7980s; 3620 mV = 3.62 V — the exact
     * example docs/specs/S25-power-latch.md's Amendments cites. */
    ff_session_log_heartbeat(&prev, 7980u, 3620u, FF_SESSION_LINK_CONNECTED, 1u);

    char buf[96];
    bool const shown = ff_session_log_format_last_time(buf, sizeof(buf), &prev, /*prev_valid=*/true);

    TEST_ASSERT_TRUE(shown);
    TEST_ASSERT_EQUAL_STRING("Last time: stopped unexpectedly after 2h13m - battery 3.62 V", buf);
}

static void test_format_last_time_unclean_unknown_battery(void)
{
    ff_session_log_t prev;
    memset(&prev, 0, sizeof(prev));
    ff_session_log_heartbeat(&prev, 61u, 0u, FF_SESSION_LINK_NONE, 0u); /* 61s = 0h01m */

    char buf[96];
    bool const shown = ff_session_log_format_last_time(buf, sizeof(buf), &prev, /*prev_valid=*/true);

    TEST_ASSERT_TRUE(shown);
    TEST_ASSERT_EQUAL_STRING("Last time: stopped unexpectedly after 0h01m - battery unknown", buf);
}

static void test_format_last_time_null_buf_is_safe(void)
{
    ff_session_log_t prev;
    memset(&prev, 0, sizeof(prev));
    ff_session_log_heartbeat(&prev, 100u, 4000u, FF_SESSION_LINK_NONE, 0u);

    TEST_ASSERT_FALSE(ff_session_log_format_last_time(NULL, 96, &prev, true));
    char buf[96];
    TEST_ASSERT_FALSE(ff_session_log_format_last_time(buf, 0, &prev, true));
}

/* ---------------------------------------------------------------------
 * ff_reset_reason_name — never NULL, covers every enumerator.
 * ------------------------------------------------------------------- */

static void test_reset_reason_name_never_null(void)
{
    TEST_ASSERT_NOT_NULL(ff_reset_reason_name(FF_RESET_REASON_UNKNOWN));
    TEST_ASSERT_NOT_NULL(ff_reset_reason_name(FF_RESET_REASON_POWERON));
    TEST_ASSERT_NOT_NULL(ff_reset_reason_name(FF_RESET_REASON_SW));
    TEST_ASSERT_NOT_NULL(ff_reset_reason_name(FF_RESET_REASON_TASK_WDT));
    TEST_ASSERT_NOT_NULL(ff_reset_reason_name(FF_RESET_REASON_BROWNOUT));
    TEST_ASSERT_NOT_NULL(ff_reset_reason_name(FF_RESET_REASON_OTHER));
    TEST_ASSERT_EQUAL_STRING("task watchdog", ff_reset_reason_name(FF_RESET_REASON_TASK_WDT));
    TEST_ASSERT_EQUAL_STRING("brownout", ff_reset_reason_name(FF_RESET_REASON_BROWNOUT));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_load_empty_store_returns_false_and_zeroed);
    RUN_TEST(test_load_corrupt_blob_returns_false);
    RUN_TEST(test_heartbeat_sets_fields_and_clears_clean);
    RUN_TEST(test_mark_clean_sets_flag_only);
    RUN_TEST(test_round_trip_exactly_one_write);
    RUN_TEST(test_round_trip_preserves_clean_flag);
    RUN_TEST(test_heartbeat_due_cadence);
    RUN_TEST(test_heartbeat_due_wraps_safely);
    RUN_TEST(test_format_last_time_no_prior_record);
    RUN_TEST(test_format_last_time_clean_prior_session);
    RUN_TEST(test_format_last_time_unclean_with_battery);
    RUN_TEST(test_format_last_time_unclean_unknown_battery);
    RUN_TEST(test_format_last_time_null_buf_is_safe);
    RUN_TEST(test_reset_reason_name_never_null);
    return UNITY_END();
}
