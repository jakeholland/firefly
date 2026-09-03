/**
 * test_rally.c — core/rally unit tests (tech-debt sprint: rally place/
 * name/when logic moved from ff_shell into core ff_rally; see ff_rally.h's
 * top comment).
 *
 * This is the behaviour-preservation proof for the move: every case here
 * mirrors an assertion the original `firmware/app/ff_shell.c` statics made
 * implicitly (via `shell_rally_place` / `shell_rally_compose_name` /
 * `shell_rally_when_suffix` / `shell_rally_when_echo`), now exercised
 * directly against the pure core functions. The pre-existing
 * `firmware/app/tests/test_shell.c` S22/S24/S26 rally tests are the
 * OTHER half of that proof — they still exercise ff_shell.c's dispatch
 * end to end and must pass UNCHANGED (see this repo's PR body for the
 * list; this file does not duplicate them).
 */
#include <string.h>

#include "unity.h"

#include "ff_proto.h" /* FF_PROTO_RALLY_NAME_MAX */
#include "ff_rally.h"

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------------------
 * ff_rally_nearest_landmark / ff_rally_place_name
 * ------------------------------------------------------------------- */

static void S22_nearest_landmark_chosen_among_several(void)
{
    ff_rally_landmark_t lm[3] = {
        {.name = "Far Stage", .has_pos = true, .east_m = 500.0f, .north_m = 0.0f},
        {.name = "Near Stage", .has_pos = true, .east_m = 30.0f, .north_m = 40.0f}, /* 50 m */
        {.name = "Mid Stage", .has_pos = true, .east_m = 100.0f, .north_m = 0.0f},
    };
    uint8_t idx = 99;
    bool const ok = ff_rally_nearest_landmark(lm, 3, 0.0f, 0.0f, FF_RALLY_LANDMARK_NEAR_M, &idx);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(1, idx);
    TEST_ASSERT_EQUAL_STRING("Near Stage", ff_rally_place_name(lm, 3, 0.0f, 0.0f));
}

/* Boundary pin, by LITERAL (not the macro) — mutation (a) flips 120 -> 121
 * and this is exactly the case that then starts (wrongly) succeeding. */
static void S22_landmark_at_exactly_120m_is_not_near(void)
{
    ff_rally_landmark_t lm[1] = {
        {.name = "Boundary Stage", .has_pos = true, .east_m = 120.0f, .north_m = 0.0f},
    };
    uint8_t idx = 99;
    bool const ok = ff_rally_nearest_landmark(lm, 1, 0.0f, 0.0f, 120.0f, &idx);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_STRING("MY SPOT", ff_rally_place_name(lm, 1, 0.0f, 0.0f));
}

static void S22_landmark_at_121m_is_not_near(void)
{
    ff_rally_landmark_t lm[1] = {
        {.name = "Far Boundary", .has_pos = true, .east_m = 121.0f, .north_m = 0.0f},
    };
    uint8_t idx = 99;
    bool const ok = ff_rally_nearest_landmark(lm, 1, 0.0f, 0.0f, 120.0f, &idx);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_STRING("MY SPOT", ff_rally_place_name(lm, 1, 0.0f, 0.0f));
}

/* Just inside the boundary (unlike the 120m/121m literal pins above, this
 * confirms the threshold is not ALSO broken the other way, e.g. by an
 * off-by-a-lot mutation that rejects everything). */
static void S22_landmark_just_inside_120m_is_near(void)
{
    ff_rally_landmark_t lm[1] = {
        {.name = "Just Inside", .has_pos = true, .east_m = 119.9f, .north_m = 0.0f},
    };
    uint8_t idx = 99;
    bool const ok = ff_rally_nearest_landmark(lm, 1, 0.0f, 0.0f, 120.0f, &idx);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(0, idx);
}

static void S22_no_landmark_in_range_falls_back_to_my_spot(void)
{
    ff_rally_landmark_t lm[2] = {
        {.name = "Way Out There", .has_pos = true, .east_m = 1000.0f, .north_m = 0.0f},
        {.name = "Also Far", .has_pos = true, .east_m = 0.0f, .north_m = 900.0f},
    };
    uint8_t idx = 99;
    TEST_ASSERT_FALSE(
        ff_rally_nearest_landmark(lm, 2, 0.0f, 0.0f, FF_RALLY_LANDMARK_NEAR_M, &idx));
    TEST_ASSERT_EQUAL_STRING("MY SPOT", ff_rally_place_name(lm, 2, 0.0f, 0.0f));
}

static void S22_null_landmarks_or_empty_list_is_honest_fallback(void)
{
    uint8_t idx = 99;
    TEST_ASSERT_FALSE(ff_rally_nearest_landmark(NULL, 0, 0.0f, 0.0f, FF_RALLY_LANDMARK_NEAR_M, &idx));
    TEST_ASSERT_EQUAL_STRING("MY SPOT", ff_rally_place_name(NULL, 0, 0.0f, 0.0f));

    ff_rally_landmark_t lm[1] = {
        {.name = "Unreachable", .has_pos = true, .east_m = 0.0f, .north_m = 0.0f},
    };
    TEST_ASSERT_FALSE(ff_rally_nearest_landmark(lm, 0, 0.0f, 0.0f, FF_RALLY_LANDMARK_NEAR_M, &idx));
}

static void S22_landmark_without_pos_or_name_is_skipped(void)
{
    ff_rally_landmark_t lm[3] = {
        {.name = "No Pos", .has_pos = false, .east_m = 1.0f, .north_m = 1.0f},
        {.name = "", .has_pos = true, .east_m = 2.0f, .north_m = 2.0f},
        {.name = NULL, .has_pos = true, .east_m = 3.0f, .north_m = 3.0f},
    };
    uint8_t idx = 99;
    TEST_ASSERT_FALSE(
        ff_rally_nearest_landmark(lm, 3, 0.0f, 0.0f, FF_RALLY_LANDMARK_NEAR_M, &idx));
    TEST_ASSERT_EQUAL_STRING("MY SPOT", ff_rally_place_name(lm, 3, 0.0f, 0.0f));
}

/* A landmark whose name would not fit the wire is skipped by the
 * nearest-search (unlike ff_rally_landmark_displayable, which has no
 * length check — that's the S24 WHERE list's more permissive predicate). */
static void S22_landmark_name_too_long_for_wire_is_skipped(void)
{
    char long_name[FF_PROTO_RALLY_NAME_MAX + 2];
    memset(long_name, 'A', sizeof long_name - 1u);
    long_name[sizeof long_name - 1u] = '\0';
    TEST_ASSERT_EQUAL_size_t((size_t)FF_PROTO_RALLY_NAME_MAX + 1u, strlen(long_name));

    ff_rally_landmark_t lm[1] = {
        {.name = long_name, .has_pos = true, .east_m = 1.0f, .north_m = 1.0f},
    };
    uint8_t idx = 99;
    TEST_ASSERT_FALSE(
        ff_rally_nearest_landmark(lm, 1, 0.0f, 0.0f, FF_RALLY_LANDMARK_NEAR_M, &idx));
    TEST_ASSERT_EQUAL_STRING("MY SPOT", ff_rally_place_name(lm, 1, 0.0f, 0.0f));
}

/* ---------------------------------------------------------------------
 * ff_rally_landmark_displayable — the S24 WHERE-list predicate (no
 * wire-length check, unlike the nearest-search filter above).
 * ------------------------------------------------------------------- */

static void S24_landmark_displayable_requires_pos_and_name(void)
{
    TEST_ASSERT_TRUE(ff_rally_landmark_displayable(true, "Main Stage"));
    TEST_ASSERT_FALSE(ff_rally_landmark_displayable(false, "Main Stage"));
    TEST_ASSERT_FALSE(ff_rally_landmark_displayable(true, ""));
    TEST_ASSERT_FALSE(ff_rally_landmark_displayable(true, NULL));
}

static void S24_landmark_displayable_does_not_check_wire_length(void)
{
    char long_name[FF_PROTO_RALLY_NAME_MAX + 10];
    memset(long_name, 'B', sizeof long_name - 1u);
    long_name[sizeof long_name - 1u] = '\0';
    /* Unlike the nearest-search filter, the WHERE-list predicate accepts
     * this — S24's picker shows it; only the SEND path (compose+truncate)
     * cares about the wire budget. */
    TEST_ASSERT_TRUE(ff_rally_landmark_displayable(true, long_name));
}

/* ---------------------------------------------------------------------
 * ff_rally_when_suffix / ff_rally_when_echo — literal vocabulary pins.
 * ------------------------------------------------------------------- */

static void S24_when_suffix_strings_by_literal(void)
{
    TEST_ASSERT_EQUAL_STRING("", ff_rally_when_suffix(0));
    TEST_ASSERT_EQUAL_STRING(" +15m", ff_rally_when_suffix(1));
    TEST_ASSERT_EQUAL_STRING(" +30m", ff_rally_when_suffix(2));
    /* Out-of-range treated as NOW (defensive default, matches the
     * original switch's `default:` case). */
    TEST_ASSERT_EQUAL_STRING("", ff_rally_when_suffix(99));
}

static void S24_when_echo_strings_by_literal(void)
{
    TEST_ASSERT_EQUAL_STRING("Now", ff_rally_when_echo(0));
    TEST_ASSERT_EQUAL_STRING("+15m", ff_rally_when_echo(1));
    TEST_ASSERT_EQUAL_STRING("+30m", ff_rally_when_echo(2));
    TEST_ASSERT_EQUAL_STRING("Now", ff_rally_when_echo(99));
}

/* ---------------------------------------------------------------------
 * ff_rally_compose_name — truncation policy (S24 AC6: truncate the
 * PLACE, never the SUFFIX), pinned to exact output strings.
 * ------------------------------------------------------------------- */

static void S24_compose_name_short_place_no_truncation(void)
{
    char out[FF_PROTO_RALLY_NAME_MAX + 1];
    TEST_ASSERT_TRUE(ff_rally_compose_name("Main Stage", 1, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("Main Stage +15m", out);
}

static void S24_compose_name_now_has_no_suffix(void)
{
    char out[FF_PROTO_RALLY_NAME_MAX + 1];
    TEST_ASSERT_TRUE(ff_rally_compose_name("Main Stage", 0, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("Main Stage", out);
}

/* FF_PROTO_RALLY_NAME_MAX is 24. A 30-char place + " +15m" (5 chars) must
 * truncate the PLACE to 19 chars (24 - 5) and keep the suffix intact,
 * exact string pinned. */
static void S24_compose_name_long_place_truncates_place_keeps_suffix(void)
{
    char const *place = "This Stage Name Is Definitely Too Long";
    TEST_ASSERT_TRUE(strlen(place) > (size_t)FF_PROTO_RALLY_NAME_MAX);
    char out[FF_PROTO_RALLY_NAME_MAX + 1];
    TEST_ASSERT_TRUE(ff_rally_compose_name(place, 1, out, sizeof out));
    /* Place truncated to the first 19 bytes (24 - strlen(" +15m")), which
     * happens to end in a space right before "Definitely" — the suffix's
     * own leading space then makes a visible double space. Pinned exactly
     * so a truncation-boundary regression cannot hide behind "close
     * enough". */
    TEST_ASSERT_EQUAL_STRING("This Stage Name Is  +15m", out);
    TEST_ASSERT_EQUAL_size_t((size_t)FF_PROTO_RALLY_NAME_MAX, strlen(out));
    /* The suffix itself must appear byte-for-byte, un-truncated. */
    TEST_ASSERT_NOT_NULL(strstr(out, " +15m"));
}

static void S24_compose_name_long_place_with_30m_suffix(void)
{
    char const *place = "This Stage Name Is Definitely Too Long";
    char out[FF_PROTO_RALLY_NAME_MAX + 1];
    TEST_ASSERT_TRUE(ff_rally_compose_name(place, 2, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("This Stage Name Is  +30m", out);
}

static void S24_compose_name_empty_place_fails(void)
{
    char out[FF_PROTO_RALLY_NAME_MAX + 1];
    memset(out, 'x', sizeof out);
    TEST_ASSERT_FALSE(ff_rally_compose_name("", 0, out, sizeof out));
    TEST_ASSERT_FALSE(ff_rally_compose_name(NULL, 0, out, sizeof out));
}

static void S24_compose_name_undersized_buffer_fails(void)
{
    char out[4]; /* well under FF_PROTO_RALLY_NAME_MAX + 1 */
    TEST_ASSERT_FALSE(ff_rally_compose_name("Main Stage", 0, out, sizeof out));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S22_nearest_landmark_chosen_among_several);
    RUN_TEST(S22_landmark_at_exactly_120m_is_not_near);
    RUN_TEST(S22_landmark_at_121m_is_not_near);
    RUN_TEST(S22_landmark_just_inside_120m_is_near);
    RUN_TEST(S22_no_landmark_in_range_falls_back_to_my_spot);
    RUN_TEST(S22_null_landmarks_or_empty_list_is_honest_fallback);
    RUN_TEST(S22_landmark_without_pos_or_name_is_skipped);
    RUN_TEST(S22_landmark_name_too_long_for_wire_is_skipped);

    RUN_TEST(S24_landmark_displayable_requires_pos_and_name);
    RUN_TEST(S24_landmark_displayable_does_not_check_wire_length);

    RUN_TEST(S24_when_suffix_strings_by_literal);
    RUN_TEST(S24_when_echo_strings_by_literal);

    RUN_TEST(S24_compose_name_short_place_no_truncation);
    RUN_TEST(S24_compose_name_now_has_no_suffix);
    RUN_TEST(S24_compose_name_long_place_truncates_place_keeps_suffix);
    RUN_TEST(S24_compose_name_long_place_with_30m_suffix);
    RUN_TEST(S24_compose_name_empty_place_fails);
    RUN_TEST(S24_compose_name_undersized_buffer_fails);

    return UNITY_END();
}
