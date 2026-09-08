/**
 * test_dbgcmd.c — bench/debug console line-command parser tests
 * (CONFIG_FF_DEBUG_CONSOLE). See ff_dbgcmd.h.
 *
 * Every command parses (one test per kind) · arguments are bounded (the
 * SEND/DM text-length and node-hex-digit-count gates) · garbage is
 * rejected (unknown command, bad hex, missing args, extra args on a
 * zero-arg command) · no buffer overreads (the 300-char-line test feeds
 * a buffer with an explicit length and no trailing NUL anywhere in it).
 */
#include <string.h>

#include "unity.h"

#include "ff_dbgcmd.h"

void setUp(void) {}
void tearDown(void) {}

static ff_dbgcmd_status_t parse_str(char const *s, ff_dbgcmd_t *out)
{
    return ff_dbgcmd_parse(s, strlen(s), out);
}

/* ------------------------------------------------------------------- */
/* every command parses                                                  */
/* ------------------------------------------------------------------- */

static void dbgcmd_help_parses(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("help", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_HELP, cmd.kind);
}

static void dbgcmd_me_parses(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("me", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_ME, cmd.kind);
}

static void dbgcmd_roster_parses(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("roster", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_ROSTER, cmd.kind);
}

static void dbgcmd_heard_parses(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("heard", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_HEARD, cmd.kind);
}

static void dbgcmd_wall_parses(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("wall", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_WALL, cmd.kind);
}

static void dbgcmd_i2c_parses(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("i2c", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_I2C, cmd.kind);
}

/* DIAGNOSTICS — `diag` is zero-arg, same shape as `wall`/`i2c` above. */
static void dbgcmd_diag_parses(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("diag", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_DIAG, cmd.kind);
}

static void dbgcmd_diag_with_extra_arg_rejected(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, parse_str("diag now", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_NONE, cmd.kind);
}

static void dbgcmd_send_parses_with_text(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("send hello crew", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_SEND, cmd.kind);
    TEST_ASSERT_EQUAL_STRING("hello crew", cmd.u.text);
}

static void dbgcmd_dm_parses_with_hex_and_text(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("dm a1b2c3d4 omw", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_DM, cmd.kind);
    TEST_ASSERT_EQUAL_UINT32(0xa1b2c3d4u, cmd.u.dm.dest_node);
    TEST_ASSERT_EQUAL_STRING("omw", cmd.u.dm.text);
}

static void dbgcmd_dm_accepts_bang_prefix(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("dm !a1b2c3d4 omw", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_DM, cmd.kind);
    TEST_ASSERT_EQUAL_UINT32(0xa1b2c3d4u, cmd.u.dm.dest_node);
}

static void dbgcmd_dm_accepts_0x_prefix(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("dm 0xFF text here", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_DM, cmd.kind);
    TEST_ASSERT_EQUAL_UINT32(0xFFu, cmd.u.dm.dest_node);
    TEST_ASSERT_EQUAL_STRING("text here", cmd.u.dm.text);
}

static void dbgcmd_flare_parses(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("flare", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_FLARE, cmd.kind);
}

static void dbgcmd_flare_cancel_parses(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("flare cancel", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_FLARE_CANCEL, cmd.kind);
}

/* S12 step 3 — "cal" and its four sub-verbs, same bare-verb-plus-sub-verb
 * shape as "flare"/"flare cancel" just above. */
static void dbgcmd_cal_parses(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("cal", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_CAL, cmd.kind);
}

static void dbgcmd_cal_start_parses(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("cal start", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_CAL_START, cmd.kind);
}

static void dbgcmd_cal_finish_parses(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("cal finish", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_CAL_FINISH, cmd.kind);
}

static void dbgcmd_cal_cancel_parses(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("cal cancel", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_CAL_CANCEL, cmd.kind);
}

static void dbgcmd_cal_clear_parses(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("cal clear", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_CAL_CLEAR, cmd.kind);
}

static void dbgcmd_cal_with_bad_arg_rejected(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, parse_str("cal xyz", &cmd));
}

/* NAME in Settings — "name" (bare, status) and "name <text>" (set). */
static void dbgcmd_name_bare_parses(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("name", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_NAME, cmd.kind);
}

static void dbgcmd_name_set_parses_with_text(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("name Jake", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_NAME_SET, cmd.kind);
    TEST_ASSERT_EQUAL_STRING("Jake", cmd.u.text);
}

/* A name with a space is a legal name (S12's own "charset A-Z0-9 space"
 * rule) — the rest-of-line argument must not be re-tokenized. */
static void dbgcmd_name_set_keeps_interior_spaces(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("name Jake H", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_NAME_SET, cmd.kind);
    TEST_ASSERT_EQUAL_STRING("Jake H", cmd.u.text);
}

static void dbgcmd_name_set_text_over_max_length_rejected(void)
{
    char line[8 + FF_DBGCMD_TEXT_MAX + 2];
    memcpy(line, "name ", 5);
    memset(line + 5, 'x', FF_DBGCMD_TEXT_MAX + 1u);
    size_t const len = 5 + FF_DBGCMD_TEXT_MAX + 1u;

    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, ff_dbgcmd_parse(line, len, &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_NONE, cmd.kind);
}

/* ------------------------------------------------------------------- */
/* CRLF tolerance / whitespace                                          */
/* ------------------------------------------------------------------- */

static void dbgcmd_tolerates_crlf(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, ff_dbgcmd_parse("me\r\n", 4, &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_ME, cmd.kind);
}

static void dbgcmd_tolerates_bare_lf(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, ff_dbgcmd_parse("roster\n", 7, &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_ROSTER, cmd.kind);
}

static void dbgcmd_tolerates_leading_trailing_and_internal_whitespace(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, parse_str("  send   hi there  ", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_SEND, cmd.kind);
    TEST_ASSERT_EQUAL_STRING("hi there", cmd.u.text);
}

static void dbgcmd_blank_line_is_empty_not_unknown(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_EMPTY, parse_str("   \r\n", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_NONE, cmd.kind);
}

static void dbgcmd_zero_length_line_is_empty(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_EMPTY, ff_dbgcmd_parse("", 0, &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_NONE, cmd.kind);
}

/* ------------------------------------------------------------------- */
/* garbage is rejected                                                   */
/* ------------------------------------------------------------------- */

static void dbgcmd_unknown_command_is_rejected(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_UNKNOWN_CMD, parse_str("frobnicate", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_NONE, cmd.kind);
}

static void dbgcmd_extra_args_on_zero_arg_command_rejected(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, parse_str("me now", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, parse_str("roster extra", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, parse_str("wall junk", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, parse_str("i2c 0x20", &cmd));
}

static void dbgcmd_flare_with_bad_arg_rejected(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, parse_str("flare xyz", &cmd));
}

static void dbgcmd_send_with_no_text_rejected(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, parse_str("send", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, parse_str("send   ", &cmd));
}

static void dbgcmd_dm_with_missing_text_rejected(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, parse_str("dm a1b2c3d4", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, parse_str("dm a1b2c3d4   ", &cmd));
}

static void dbgcmd_dm_with_missing_node_rejected(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, parse_str("dm", &cmd));
}

static void dbgcmd_dm_with_non_hex_node_rejected(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, parse_str("dm zzzz hello", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, parse_str("dm 12g4 hello", &cmd));
}

static void dbgcmd_dm_with_too_many_hex_digits_rejected(void)
{
    ff_dbgcmd_t cmd;
    /* 9 hex digits — one more than a uint32_t node id can ever need. */
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, parse_str("dm 123456789 hello", &cmd));
}

static void dbgcmd_dm_with_bare_prefix_no_digits_rejected(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, parse_str("dm ! hello", &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, parse_str("dm 0x hello", &cmd));
}

/* ------------------------------------------------------------------- */
/* arguments are bounded                                                 */
/* ------------------------------------------------------------------- */

static void dbgcmd_send_text_at_max_length_ok(void)
{
    char line[8 + FF_DBGCMD_TEXT_MAX + 1];
    memcpy(line, "send ", 5);
    memset(line + 5, 'x', FF_DBGCMD_TEXT_MAX);
    size_t const len = 5 + FF_DBGCMD_TEXT_MAX;

    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_OK, ff_dbgcmd_parse(line, len, &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_SEND, cmd.kind);
    TEST_ASSERT_EQUAL_size_t(FF_DBGCMD_TEXT_MAX, strlen(cmd.u.text));
}

static void dbgcmd_send_text_over_max_length_rejected(void)
{
    char line[8 + FF_DBGCMD_TEXT_MAX + 2];
    memcpy(line, "send ", 5);
    memset(line + 5, 'x', FF_DBGCMD_TEXT_MAX + 1u);
    size_t const len = 5 + FF_DBGCMD_TEXT_MAX + 1u;

    ff_dbgcmd_t cmd;
    /* One byte over FF_DBGCMD_TEXT_MAX is still well under
     * FF_DBGCMD_LINE_MAX (see ff_dbgcmd.h's sizing note), so this is the
     * text-length gate tripping specifically, not the whole-line one. */
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, ff_dbgcmd_parse(line, len, &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_NONE, cmd.kind);
}

/* ------------------------------------------------------------------- */
/* no buffer overreads — the 300-char-line case                         */
/* ------------------------------------------------------------------- */

static void dbgcmd_300_char_line_is_rejected_not_overread(void)
{
    /* No trailing NUL anywhere in this buffer — if the parser ever
     * reached for strlen()/a NUL search instead of respecting line_len,
     * this would read arbitrary stack memory looking for one. */
    char line[300];
    memset(line, 'a', sizeof(line));

    ff_dbgcmd_t cmd;
    ff_dbgcmd_status_t const st = ff_dbgcmd_parse(line, sizeof(line), &cmd);
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_TOO_LONG, st);
    TEST_ASSERT_EQUAL(FF_DBGCMD_NONE, cmd.kind);
}

static void dbgcmd_exactly_max_length_line_is_not_rejected_for_length(void)
{
    /* FF_DBGCMD_LINE_MAX bytes of "send " + filler is right at the
     * boundary — must NOT be TOO_LONG (that's the >max case above). */
    char line[FF_DBGCMD_LINE_MAX];
    memcpy(line, "send ", 5);
    memset(line + 5, 'b', sizeof(line) - 5u);

    ff_dbgcmd_t cmd;
    ff_dbgcmd_status_t const st = ff_dbgcmd_parse(line, sizeof(line), &cmd);
    TEST_ASSERT_NOT_EQUAL(FF_DBGCMD_ERR_TOO_LONG, st);
}

static void dbgcmd_null_line_with_nonzero_len_is_bad_args_not_crash(void)
{
    ff_dbgcmd_t cmd;
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, ff_dbgcmd_parse(NULL, 5, &cmd));
    TEST_ASSERT_EQUAL(FF_DBGCMD_NONE, cmd.kind);
}

static void dbgcmd_null_out_is_bad_args_not_crash(void)
{
    TEST_ASSERT_EQUAL(FF_DBGCMD_ERR_BAD_ARGS, ff_dbgcmd_parse("me", 2, NULL));
}

/* ------------------------------------------------------------------- */
/* names                                                                 */
/* ------------------------------------------------------------------- */

static void dbgcmd_names_are_never_null(void)
{
    TEST_ASSERT_NOT_NULL(ff_dbgcmd_kind_name(FF_DBGCMD_DM));
    TEST_ASSERT_NOT_NULL(ff_dbgcmd_kind_name((ff_dbgcmd_kind_t)99));
    TEST_ASSERT_NOT_NULL(ff_dbgcmd_status_name(FF_DBGCMD_ERR_OK));
    TEST_ASSERT_NOT_NULL(ff_dbgcmd_status_name((ff_dbgcmd_status_t)99));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(dbgcmd_help_parses);
    RUN_TEST(dbgcmd_me_parses);
    RUN_TEST(dbgcmd_roster_parses);
    RUN_TEST(dbgcmd_heard_parses);
    RUN_TEST(dbgcmd_wall_parses);
    RUN_TEST(dbgcmd_i2c_parses);
    RUN_TEST(dbgcmd_diag_parses);
    RUN_TEST(dbgcmd_diag_with_extra_arg_rejected);
    RUN_TEST(dbgcmd_send_parses_with_text);
    RUN_TEST(dbgcmd_dm_parses_with_hex_and_text);
    RUN_TEST(dbgcmd_dm_accepts_bang_prefix);
    RUN_TEST(dbgcmd_dm_accepts_0x_prefix);
    RUN_TEST(dbgcmd_flare_parses);
    RUN_TEST(dbgcmd_flare_cancel_parses);
    RUN_TEST(dbgcmd_cal_parses);
    RUN_TEST(dbgcmd_cal_start_parses);
    RUN_TEST(dbgcmd_cal_finish_parses);
    RUN_TEST(dbgcmd_cal_cancel_parses);
    RUN_TEST(dbgcmd_cal_clear_parses);
    RUN_TEST(dbgcmd_cal_with_bad_arg_rejected);

    RUN_TEST(dbgcmd_name_bare_parses);
    RUN_TEST(dbgcmd_name_set_parses_with_text);
    RUN_TEST(dbgcmd_name_set_keeps_interior_spaces);
    RUN_TEST(dbgcmd_name_set_text_over_max_length_rejected);

    RUN_TEST(dbgcmd_tolerates_crlf);
    RUN_TEST(dbgcmd_tolerates_bare_lf);
    RUN_TEST(dbgcmd_tolerates_leading_trailing_and_internal_whitespace);
    RUN_TEST(dbgcmd_blank_line_is_empty_not_unknown);
    RUN_TEST(dbgcmd_zero_length_line_is_empty);

    RUN_TEST(dbgcmd_unknown_command_is_rejected);
    RUN_TEST(dbgcmd_extra_args_on_zero_arg_command_rejected);
    RUN_TEST(dbgcmd_flare_with_bad_arg_rejected);
    RUN_TEST(dbgcmd_send_with_no_text_rejected);
    RUN_TEST(dbgcmd_dm_with_missing_text_rejected);
    RUN_TEST(dbgcmd_dm_with_missing_node_rejected);
    RUN_TEST(dbgcmd_dm_with_non_hex_node_rejected);
    RUN_TEST(dbgcmd_dm_with_too_many_hex_digits_rejected);
    RUN_TEST(dbgcmd_dm_with_bare_prefix_no_digits_rejected);

    RUN_TEST(dbgcmd_send_text_at_max_length_ok);
    RUN_TEST(dbgcmd_send_text_over_max_length_rejected);

    RUN_TEST(dbgcmd_300_char_line_is_rejected_not_overread);
    RUN_TEST(dbgcmd_exactly_max_length_line_is_not_rejected_for_length);
    RUN_TEST(dbgcmd_null_line_with_nonzero_len_is_bad_args_not_crash);
    RUN_TEST(dbgcmd_null_out_is_bad_args_not_crash);

    RUN_TEST(dbgcmd_names_are_never_null);

    return UNITY_END();
}
