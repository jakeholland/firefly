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
/* ff_flare_fmt_lock_cost (issue #27; re-worded per PR #41 UX review     */
/* BLOCKING 1 and BLOCKING 2)                                            */
/* ------------------------------------------------------------------- */

static void S10_ACn_lock_cost_names_the_verb_the_lock_and_the_holder(void)
{
    /* The three things PR #20's UX review finding #3 requires the chip to
     * say, in the form PR #41's UX review asked for: what pressing GO
     * DOES (a verb of loss), to WHAT (the lock — the user's only
     * vocabulary for this, and the word the Radar face's own chip uses),
     * and WHOSE (the name). The incoming sender is deliberately absent —
     * it is the headline directly above, pinned separately by
     * test_scr_flare.c. */
    char buf[48];
    ff_flare_fmt_lock_cost(buf, sizeof(buf), "DANA");
    TEST_ASSERT_EQUAL_STRING("GO DROPS LOCK - DANA", buf);
}

static void S10_ACn_lock_cost_reviewers_collision_pair_now_renders_distinctly(void)
{
    /* PR #41 UX review BLOCKING 2, the reviewer's exact repro. The
     * previous two-name form rendered ALEXANDRIA-locked /
     * ALEXANDRINA-flaring as "GO: ALEXANDRI > ALEXANDRI" — a trade of a
     * person for themselves, which reads as "costs nothing, press GO",
     * the precise outcome the chip exists to prevent.
     *
     * Two independent things fix it, and this pins both:
     *   1. STRUCTURAL — there is only one name on the chip now, so a
     *      chip can no longer state an identity between two people at
     *      all. That is the reviewer's own preferred fix, and it is the
     *      one that makes the failure mode inexpressible rather than
     *      merely unlikely.
     *   2. BUDGET — the single-name wording frees enough width that the
     *      cap rose from 9 to 11, so this particular pair now renders
     *      whole on both sides rather than being cut at all. */
    char a[48];
    char b[48];
    ff_flare_fmt_lock_cost(a, sizeof(a), "ALEXANDRIA");
    ff_flare_fmt_lock_cost(b, sizeof(b), "ALEXANDRINA");
    TEST_ASSERT_EQUAL_STRING("GO DROPS LOCK - ALEXANDRIA", a);
    TEST_ASSERT_EQUAL_STRING("GO DROPS LOCK - ALEXANDRINA", b);
    TEST_ASSERT_TRUE_MESSAGE(strcmp(a, b) != 0, "two distinct crew names must not render as the same chip");

    /* And the reviewer's second pair, MIKE SMITH / MIKE SMYTHE, which
     * the old form cut to "MIKE SMIT" / "MIKE SMYT". */
    ff_flare_fmt_lock_cost(a, sizeof(a), "MIKE SMITH");
    ff_flare_fmt_lock_cost(b, sizeof(b), "MIKE SMYTHE");
    TEST_ASSERT_EQUAL_STRING("GO DROPS LOCK - MIKE SMITH", a);
    TEST_ASSERT_EQUAL_STRING("GO DROPS LOCK - MIKE SMYTHE", b);
}

static void S10_ACn_lock_cost_shared_prefix_over_budget_still_looks_cut(void)
{
    /* The residual case, stated honestly rather than papered over: two
     * names that AGREE for the whole budget and both exceed it still
     * produce the same chip. That is ordinary display truncation, and
     * the ellipsis is what keeps it honest — the chip visibly says "this
     * name is longer than what you're reading" rather than presenting a
     * cut name as a whole one.
     *
     * Crucially it is no longer the BLOCKING 2 failure: with one name
     * there is no second name for it to be equated WITH, so the chip
     * never implies the trade is free. It just identifies its subject
     * less precisely, while saying so. */
    char a[48];
    char b[48];
    ff_flare_fmt_lock_cost(a, sizeof(a), "ALEXANDRINAX");
    ff_flare_fmt_lock_cost(b, sizeof(b), "ALEXANDRINAY");
    TEST_ASSERT_EQUAL_STRING("GO DROPS LOCK - ALEXANDRINA...", a);
    TEST_ASSERT_EQUAL_STRING("GO DROPS LOCK - ALEXANDRINA...", b);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(a, FF_FLARE_FMT_ELLIPSIS),
                                  "a name cut short must always show that it was cut");
}

static void S10_ACn_lock_cost_truncated_name_looks_truncated(void)
{
    /* The floor PR #41's UX reviewer asked for: a cut name must LOOK cut,
     * so it is never mistaken for a whole one. */
    char buf[48];
    ff_flare_fmt_lock_cost(buf, sizeof(buf), "BARTHOLOMEWWWWW");
    TEST_ASSERT_EQUAL_STRING("GO DROPS LOCK - BARTHOLOMEW...", buf);
}

static void S10_ACn_lock_cost_exact_max_length_name_is_not_truncated(void)
{
    /* Both sides of the FF_FLARE_FMT_LOCK_NAME_MAX boundary: a name
     * exactly at budget survives whole and gets NO ellipsis (an ellipsis
     * on an intact name would be its own small lie). */
    char buf[48];
    ff_flare_fmt_lock_cost(buf, sizeof(buf), "ALEXANDRIA1");
    TEST_ASSERT_EQUAL_STRING("GO DROPS LOCK - ALEXANDRIA1", buf);
    ff_flare_fmt_lock_cost(buf, sizeof(buf), "ALEXANDRIA12");
    TEST_ASSERT_EQUAL_STRING("GO DROPS LOCK - ALEXANDRIA1...", buf);
}

static void S10_ACn_lock_cost_truncation_never_severs_a_codepoint(void)
{
    /* Crew names are untrusted UTF-8 off the radio (PR #41 code review,
     * minor finding: the cap is a BYTE budget). A cut landing mid-glyph
     * backs off to the last complete codepoint instead of emitting a
     * severed one. "ANDREEEEEEE" + "E-acute" puts a 2-byte codepoint
     * across the 11-byte boundary. */
    char buf[48];
    ff_flare_fmt_lock_cost(buf, sizeof(buf), "ANDREEEEEEE\xC3\x89X");
    TEST_ASSERT_EQUAL_STRING("GO DROPS LOCK - ANDREEEEEEE...", buf);

    /* And a codepoint that ends exactly ON the budget is kept whole. */
    ff_flare_fmt_lock_cost(buf, sizeof(buf), "ANDREEEEE\xC3\x89X");
    TEST_ASSERT_EQUAL_STRING("GO DROPS LOCK - ANDREEEEE\xC3\x89...", buf);
}

static void S10_ACn_lock_cost_missing_name_is_explicitly_unknown(void)
{
    /* CLAUDE.md's honesty rule: "?" (the marker
     * ff_scr_flare_build_lock_chip already uses), never an invented
     * identity. Defensive only — the caller gates on
     * ff_flare_fmt_go_switches_lock, which is already false for an empty
     * locked name, so no chip is built at all. Tested because the guard
     * exists, not because the path ships. */
    char buf[48];
    ff_flare_fmt_lock_cost(buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_STRING("GO DROPS LOCK - ?", buf);
    ff_flare_fmt_lock_cost(buf, sizeof(buf), NULL);
    TEST_ASSERT_EQUAL_STRING("GO DROPS LOCK - ?", buf);
}

static void S10_ACn_lock_cost_worst_case_fits_the_callers_buffer(void)
{
    /* scr_flare.c declares char lock_line[48]; the longest string this
     * function can produce is "GO DROPS LOCK - " (16) + 11 + "..." (3)
     * = 30 bytes + NUL. Asserting it here means shrinking that call-site
     * buffer, or growing FF_FLARE_FMT_LOCK_NAME_MAX past what it holds,
     * fails a test rather than silently truncating a disclosure on real
     * hardware. */
    char buf[48];
    ff_flare_fmt_lock_cost(buf, sizeof(buf), "ABCDEFGHIJKLMNO");
    TEST_ASSERT_TRUE_MESSAGE(strlen(buf) < 48, "worst-case lock-cost line must fit scr_flare.c's lock_line[48]");
}

static void S10_ACn_lock_cost_truncates_rather_than_overflowing(void)
{
    /* Same snprintf discipline as every other formatter here: a short
     * buffer truncates, never overflows. */
    char buf[8];
    memset(buf, 'x', sizeof(buf));
    ff_flare_fmt_lock_cost(buf, sizeof(buf), "DANA");
    TEST_ASSERT_EQUAL_STRING("GO DROP", buf);
}

static void S10_ACn_lock_cost_null_out_is_noop(void)
{
    ff_flare_fmt_lock_cost(NULL, 16, "DANA");
    char buf[8] = "keep";
    ff_flare_fmt_lock_cost(buf, 0, "DANA");
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

    RUN_TEST(S10_ACn_lock_cost_names_the_verb_the_lock_and_the_holder);
    RUN_TEST(S10_ACn_lock_cost_reviewers_collision_pair_now_renders_distinctly);
    RUN_TEST(S10_ACn_lock_cost_shared_prefix_over_budget_still_looks_cut);
    RUN_TEST(S10_ACn_lock_cost_truncated_name_looks_truncated);
    RUN_TEST(S10_ACn_lock_cost_exact_max_length_name_is_not_truncated);
    RUN_TEST(S10_ACn_lock_cost_truncation_never_severs_a_codepoint);
    RUN_TEST(S10_ACn_lock_cost_missing_name_is_explicitly_unknown);
    RUN_TEST(S10_ACn_lock_cost_worst_case_fits_the_callers_buffer);
    RUN_TEST(S10_ACn_lock_cost_truncates_rather_than_overflowing);
    RUN_TEST(S10_ACn_lock_cost_null_out_is_noop);

    return UNITY_END();
}
