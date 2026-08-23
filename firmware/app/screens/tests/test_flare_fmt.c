/**
 * test_flare_fmt.c — assertion-level coverage for flare_fmt.h (S10 slice
 * b). No LVGL anywhere in this file, same rationale as
 * test_radar_layout.c: every assertion here is a direct check on the
 * exact string/bucket the render layer will use, not a pixel-diff proxy.
 *
 * Test names follow docs/specs/S10-flare.md's numbered acceptance
 * criteria where one applies; the rest are named for the boundary they
 * pin, per the wave-lessons note in the task brief ("exact boundary
 * tests... every guard path tested").
 *
 * Mutation-check (spot-checked by hand before pushing, per the task
 * brief): deleting the `< 0.0f` wraparound fold-up in
 * ff_flare_fmt_compass8 fails S10_ACn_compass8_negative_bearing_wraps
 * (fmodf(-10, 360) == -10 in C, so idx would go negative and index the
 * points[] array out of bounds instead of correctly returning "N");
 * deleting the `expires_in_ms < 0` guard in ff_flare_fmt_countdown fails
 * S10_ACn_countdown_negative_is_na (a negative ms value would otherwise
 * render a nonsensical negative minute/second pair instead of "--:--").
 */
#include <string.h>

#include "unity.h"

#include "flare_fmt.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* ff_flare_fmt_headline                                               */
/* ------------------------------------------------------------------- */

static void S10_ACn_headline_uses_name_verbatim(void)
{
    char buf[40];
    ff_flare_fmt_headline(buf, sizeof(buf), "DANA");
    TEST_ASSERT_EQUAL_STRING("DANA IS FLARING", buf);
}

static void S10_ACn_headline_empty_name_falls_back_honestly(void)
{
    char buf[40];
    ff_flare_fmt_headline(buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("SOMEONE IS FLARING", buf);
}

static void S10_ACn_headline_null_name_falls_back_honestly(void)
{
    char buf[40];
    ff_flare_fmt_headline(buf, sizeof(buf), NULL);
    TEST_ASSERT_EQUAL_STRING("SOMEONE IS FLARING", buf);
}

static void S10_ACn_headline_null_out_is_noop(void)
{
    /* Must not crash — nothing to assert beyond "returns". */
    ff_flare_fmt_headline(NULL, 40, "DANA");
}

/* ------------------------------------------------------------------- */
/* ff_flare_fmt_compass8 — every bucket, plus every boundary            */
/* ------------------------------------------------------------------- */

static void S10_ACn_compass8_cardinal_centers(void)
{
    TEST_ASSERT_EQUAL_STRING("N", ff_flare_fmt_compass8(0.0f));
    TEST_ASSERT_EQUAL_STRING("NE", ff_flare_fmt_compass8(45.0f));
    TEST_ASSERT_EQUAL_STRING("E", ff_flare_fmt_compass8(90.0f));
    TEST_ASSERT_EQUAL_STRING("SE", ff_flare_fmt_compass8(135.0f));
    TEST_ASSERT_EQUAL_STRING("S", ff_flare_fmt_compass8(180.0f));
    TEST_ASSERT_EQUAL_STRING("SW", ff_flare_fmt_compass8(225.0f));
    TEST_ASSERT_EQUAL_STRING("W", ff_flare_fmt_compass8(270.0f));
    TEST_ASSERT_EQUAL_STRING("NW", ff_flare_fmt_compass8(315.0f));
}

static void S10_ACn_compass8_boundary_rolls_forward(void)
{
    /* Exactly on a boundary belongs to the NEXT point (rolls forward),
     * matching this codebase's existing boundary convention — see this
     * file's header comment. */
    TEST_ASSERT_EQUAL_STRING("NE", ff_flare_fmt_compass8(22.5f));
    TEST_ASSERT_EQUAL_STRING("N", ff_flare_fmt_compass8(337.5f));

    /* Just below a boundary still belongs to the PRIOR point. */
    TEST_ASSERT_EQUAL_STRING("N", ff_flare_fmt_compass8(22.4f));
    TEST_ASSERT_EQUAL_STRING("NW", ff_flare_fmt_compass8(337.4f));
}

static void S10_ACn_compass8_negative_bearing_wraps(void)
{
    /* -10 normalizes to 350, which is inside N's [337.5, 360) half. */
    TEST_ASSERT_EQUAL_STRING("N", ff_flare_fmt_compass8(-10.0f));
    /* -100 normalizes to 260, inside W's [247.5, 292.5). */
    TEST_ASSERT_EQUAL_STRING("W", ff_flare_fmt_compass8(-100.0f));
}

static void S10_ACn_compass8_over_360_wraps(void)
{
    TEST_ASSERT_EQUAL_STRING("N", ff_flare_fmt_compass8(360.0f));
    TEST_ASSERT_EQUAL_STRING("NE", ff_flare_fmt_compass8(405.0f)); /* 405 - 360 = 45 */
}

/* ------------------------------------------------------------------- */
/* ff_flare_fmt_countdown                                               */
/* ------------------------------------------------------------------- */

static void S10_ACn_countdown_negative_is_na(void)
{
    char buf[16];
    ff_flare_fmt_countdown(buf, sizeof(buf), -1);
    TEST_ASSERT_EQUAL_STRING("--:--", buf);
}

static void S10_ACn_countdown_zero_is_a_real_value_not_na(void)
{
    char buf[16];
    ff_flare_fmt_countdown(buf, sizeof(buf), 0);
    TEST_ASSERT_EQUAL_STRING("0:00", buf);
}

static void S10_ACn_countdown_truncates_seconds_toward_zero(void)
{
    char buf[16];
    ff_flare_fmt_countdown(buf, sizeof(buf), 59999); /* 59.999s -> 0:59, not 1:00 */
    TEST_ASSERT_EQUAL_STRING("0:59", buf);
}

static void S10_ACn_countdown_minute_boundary(void)
{
    char buf[16];
    ff_flare_fmt_countdown(buf, sizeof(buf), 60000);
    TEST_ASSERT_EQUAL_STRING("1:00", buf);
}

static void S10_ACn_countdown_default_send_duration(void)
{
    /* FF_FLARE_DEFAULT_DUR_S (ff_flare.h) is 300s == 300000ms. */
    char buf[16];
    ff_flare_fmt_countdown(buf, sizeof(buf), 300000);
    TEST_ASSERT_EQUAL_STRING("5:00", buf);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S10_ACn_headline_uses_name_verbatim);
    RUN_TEST(S10_ACn_headline_empty_name_falls_back_honestly);
    RUN_TEST(S10_ACn_headline_null_name_falls_back_honestly);
    RUN_TEST(S10_ACn_headline_null_out_is_noop);

    RUN_TEST(S10_ACn_compass8_cardinal_centers);
    RUN_TEST(S10_ACn_compass8_boundary_rolls_forward);
    RUN_TEST(S10_ACn_compass8_negative_bearing_wraps);
    RUN_TEST(S10_ACn_compass8_over_360_wraps);

    RUN_TEST(S10_ACn_countdown_negative_is_na);
    RUN_TEST(S10_ACn_countdown_zero_is_a_real_value_not_na);
    RUN_TEST(S10_ACn_countdown_truncates_seconds_toward_zero);
    RUN_TEST(S10_ACn_countdown_minute_boundary);
    RUN_TEST(S10_ACn_countdown_default_send_duration);

    return UNITY_END();
}
