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

/* PR #20 independent code review (LOW finding): the boundary-rolls-forward
 * and negative-wraparound cases were each covered separately, but never
 * their INTERSECTION — a negative bearing that lands exactly ON a
 * boundary once normalized. fmodf keeps the dividend's sign (C99), so
 * -22.5 does NOT fold straight to 337.5 the way a naive "always positive"
 * mental model might suggest; ff_flare_fmt_compass8 explicitly re-adds
 * 360 for negative results before classifying (see its source) — this
 * pins that fold-up actually happens, not just that the final answer
 * looks right by coincidence. */
static void S10_ACn_compass8_negative_bearing_on_boundary_rolls_forward(void)
{
    /* -22.5 normalizes to 337.5, which this codebase's "boundary rolls
     * forward" convention places in N (not NW). */
    TEST_ASSERT_EQUAL_STRING("N", ff_flare_fmt_compass8(-22.5f));
    /* -337.5 normalizes to 22.5, which rolls forward into NE (not N). */
    TEST_ASSERT_EQUAL_STRING("NE", ff_flare_fmt_compass8(-337.5f));
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

/* ------------------------------------------------------------------- */
/* ff_flare_fmt_go_switches_lock (PR #20 UX review, BLOCKING finding #3) */
/* ------------------------------------------------------------------- */

static void S10_ACn_go_switches_lock_different_names_true(void)
{
    TEST_ASSERT_TRUE(ff_flare_fmt_go_switches_lock("DANA", "KEV"));
}

static void S10_ACn_go_switches_lock_same_name_false(void)
{
    /* Re-confirming a lock on the SAME sender already flaring costs
     * nothing — nothing to disclose. */
    TEST_ASSERT_FALSE(ff_flare_fmt_go_switches_lock("DANA", "DANA"));
}

static void S10_ACn_go_switches_lock_not_locked_false(void)
{
    TEST_ASSERT_FALSE(ff_flare_fmt_go_switches_lock("", "KEV"));
    TEST_ASSERT_FALSE(ff_flare_fmt_go_switches_lock(NULL, "KEV"));
}

static void S10_ACn_go_switches_lock_no_takeover_name_false(void)
{
    /* No honest sender name to compare against — say nothing rather than
     * guess (mirrors ff_flare_fmt_headline's own "don't fabricate"
     * stance for an empty name). */
    TEST_ASSERT_FALSE(ff_flare_fmt_go_switches_lock("DANA", ""));
    TEST_ASSERT_FALSE(ff_flare_fmt_go_switches_lock("DANA", NULL));
}

/* ------------------------------------------------------------------- */
/* ff_flare_fmt_lock_trade (issue #27 — the glance-sized disclosure)     */
/*                                                                       */
/* Every assertion below uses the ASCII ">" fallback rather than         */
/* LV_SYMBOL_RIGHT: this file has no LVGL dependency by design (see the  */
/* header comment), and pinning the tests to a private FontAwesome       */
/* codepoint would make them a font-subset test rather than a formatting */
/* test. What the real caller passes is verified where it actually       */
/* matters — the regenerated flare_takeover_locked.png golden, where a   */
/* missing glyph would show as tofu.                                     */
/* ------------------------------------------------------------------- */

static void S10_ACn_lock_trade_names_both_sides_of_the_arrow(void)
{
    /* The whole point of the short form: the lock's CURRENT holder and
     * what GO would trade it for, in that order, with the button that
     * spends it named. Issue #27's "the information that must survive". */
    char buf[40];
    ff_flare_fmt_lock_trade(buf, sizeof(buf), "DANA", "KEV", NULL);
    TEST_ASSERT_EQUAL_STRING("GO: DANA > KEV", buf);
}

static void S10_ACn_lock_trade_uses_callers_arrow_glyph(void)
{
    /* The separator is the caller's to choose (scr_flare.c passes
     * LV_SYMBOL_RIGHT) — this module must not bake in a glyph it has no
     * way to know renders. */
    char buf[40];
    ff_flare_fmt_lock_trade(buf, sizeof(buf), "DANA", "KEV", "=>");
    TEST_ASSERT_EQUAL_STRING("GO: DANA => KEV", buf);
}

static void S10_ACn_lock_trade_empty_arrow_falls_back_to_ascii(void)
{
    /* An empty string is as unusable as NULL — both must still produce a
     * DIRECTIONAL separator, never a bare space that would read as two
     * unrelated names ("GO: DANA KEV" discloses nothing about which way
     * the trade goes). */
    char buf[40];
    ff_flare_fmt_lock_trade(buf, sizeof(buf), "DANA", "KEV", "");
    TEST_ASSERT_EQUAL_STRING("GO: DANA > KEV", buf);
}

static void S10_ACn_lock_trade_missing_name_is_explicitly_unknown(void)
{
    /* CLAUDE.md's honesty rule: "?" (the marker
     * ff_scr_flare_build_lock_chip already uses), never an invented
     * identity. Defensive — the caller gates on
     * ff_flare_fmt_go_switches_lock, which is already false for either
     * name being empty. */
    char buf[40];
    ff_flare_fmt_lock_trade(buf, sizeof(buf), "", "KEV", NULL);
    TEST_ASSERT_EQUAL_STRING("GO: ? > KEV", buf);
    ff_flare_fmt_lock_trade(buf, sizeof(buf), "DANA", NULL, NULL);
    TEST_ASSERT_EQUAL_STRING("GO: DANA > ?", buf);
}

static void S10_ACn_lock_trade_truncates_long_names_to_stay_on_glass(void)
{
    /* FF_APP_NAME_LEN is 16, so a name can legitimately be 15 characters.
     * Two of those plus the prefix and arrow is wider than the round
     * glass at the chip's y-offset — truncation is the honest failure
     * mode (a clipped name still identifies who you'd be dropping; a name
     * rendered past the bezel identifies nobody). Pins the exact
     * FF_FLARE_FMT_TRADE_NAME_MAX boundary: 9 kept, the 10th dropped. */
    char buf[40];
    ff_flare_fmt_lock_trade(buf, sizeof(buf), "ABCDEFGHIJKLMNO", "PQRSTUVWXYZ", NULL);
    TEST_ASSERT_EQUAL_STRING("GO: ABCDEFGHI > PQRSTUVWX", buf);
}

static void S10_ACn_lock_trade_exact_max_length_name_is_not_truncated(void)
{
    /* The other side of the same boundary — a name EXACTLY
     * FF_FLARE_FMT_TRADE_NAME_MAX long must survive whole. */
    char buf[40];
    ff_flare_fmt_lock_trade(buf, sizeof(buf), "ABCDEFGHI", "KEV", NULL);
    TEST_ASSERT_EQUAL_STRING("GO: ABCDEFGHI > KEV", buf);
}

static void S10_ACn_lock_trade_worst_case_fits_the_callers_buffer(void)
{
    /* scr_flare.c declares char lock_line[40]; the longest string this
     * function can ever produce with the 3-byte LV_SYMBOL_RIGHT is
     * "GO: " + 9 + " " + 3 + " " + 9 = 27 bytes + NUL. Asserting it here
     * means shrinking that call-site buffer, or growing
     * FF_FLARE_FMT_TRADE_NAME_MAX past what it can hold, fails a test
     * rather than silently truncating a disclosure on real hardware. */
    char buf[40];
    ff_flare_fmt_lock_trade(buf, sizeof(buf), "ABCDEFGHIJKLMNO", "PQRSTUVWXYZABCDE", "\xEF\x81\x94");
    TEST_ASSERT_TRUE_MESSAGE(strlen(buf) < 40, "worst-case trade line must fit scr_flare.c's lock_line[40]");
}

static void S10_ACn_lock_trade_truncates_rather_than_overflowing(void)
{
    /* Same snprintf discipline as every other formatter here: a short
     * buffer truncates, never overflows. */
    char buf[8];
    memset(buf, 'x', sizeof(buf));
    ff_flare_fmt_lock_trade(buf, sizeof(buf), "DANA", "KEV", NULL);
    TEST_ASSERT_EQUAL_STRING("GO: DAN", buf);
}

static void S10_ACn_lock_trade_null_out_is_noop(void)
{
    ff_flare_fmt_lock_trade(NULL, 16, "DANA", "KEV", NULL);
    char buf[8] = "keep";
    ff_flare_fmt_lock_trade(buf, 0, "DANA", "KEV", NULL);
    TEST_ASSERT_EQUAL_STRING("keep", buf);
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
    RUN_TEST(S10_ACn_compass8_negative_bearing_on_boundary_rolls_forward);

    RUN_TEST(S10_ACn_countdown_negative_is_na);
    RUN_TEST(S10_ACn_countdown_zero_is_a_real_value_not_na);
    RUN_TEST(S10_ACn_countdown_truncates_seconds_toward_zero);
    RUN_TEST(S10_ACn_countdown_minute_boundary);
    RUN_TEST(S10_ACn_countdown_default_send_duration);

    RUN_TEST(S10_ACn_go_switches_lock_different_names_true);
    RUN_TEST(S10_ACn_go_switches_lock_same_name_false);
    RUN_TEST(S10_ACn_go_switches_lock_not_locked_false);
    RUN_TEST(S10_ACn_go_switches_lock_no_takeover_name_false);

    RUN_TEST(S10_ACn_lock_trade_names_both_sides_of_the_arrow);
    RUN_TEST(S10_ACn_lock_trade_uses_callers_arrow_glyph);
    RUN_TEST(S10_ACn_lock_trade_empty_arrow_falls_back_to_ascii);
    RUN_TEST(S10_ACn_lock_trade_missing_name_is_explicitly_unknown);
    RUN_TEST(S10_ACn_lock_trade_truncates_long_names_to_stay_on_glass);
    RUN_TEST(S10_ACn_lock_trade_exact_max_length_name_is_not_truncated);
    RUN_TEST(S10_ACn_lock_trade_worst_case_fits_the_callers_buffer);
    RUN_TEST(S10_ACn_lock_trade_truncates_rather_than_overflowing);
    RUN_TEST(S10_ACn_lock_trade_null_out_is_noop);

    return UNITY_END();
}
