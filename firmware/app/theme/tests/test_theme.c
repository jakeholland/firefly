/**
 * test_theme.c — app/theme: ff_theme_crew_color's palette contract
 * (S17 slice a, docs/specs/S17-usability-hardening.md).
 *
 * `ff_theme_crew_color` is a plain `static inline` function (no .c file —
 * see ff_theme.h's top comment), so this test #includes the header
 * directly rather than linking a library. It touches no LVGL runtime
 * state (no `lv_init()`, no widgets) — every assertion here is pure
 * integer/hex comparison, matching this repo's "pure C11, unit-tested
 * directly" convention for anything that can be tested without a display.
 *
 * Covers:
 *   AC1 — indices 0-3 are byte-identical to the ORIGINAL 4-entry brand
 *         palette; 4-7 are new and distinct; no wrap/dup within 0-7.
 *   AC2 — the colorblind-safe palette is a genuinely different 8-colour
 *         set, itself free of duplicates.
 *   Mutation-conscious: a mutant that always returns the brand palette
 *   (ignoring `colorblind`), or that drops the modulo wrap, is caught
 *   below — not just "some index returns some non-zero color".
 */
#include <stdio.h>

#include "unity.h"

#include "ff_theme.h"

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------------------
 * AC1 — brand palette.
 * ------------------------------------------------------------------- */

static void S17a_AC1_brand_indices_0_to_3_are_the_exact_original_hexes(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xFF5CA8u, ff_theme_crew_color(0, false));
    TEST_ASSERT_EQUAL_HEX32(0x4FD8C4u, ff_theme_crew_color(1, false));
    TEST_ASSERT_EQUAL_HEX32(0xB08CFFu, ff_theme_crew_color(2, false));
    TEST_ASSERT_EQUAL_HEX32(0x9BE07Bu, ff_theme_crew_color(3, false));
}

static void S17a_AC1_brand_palette_has_no_duplicates_across_all_8_indices(void)
{
    uint32_t seen[8];
    for (uint8_t i = 0; i < 8; i++) {
        seen[i] = ff_theme_crew_color(i, false);
    }
    for (uint8_t i = 0; i < 8; i++) {
        for (uint8_t j = i + 1; j < 8; j++) {
            /* TEST_ASSERT_NOT_EQUAL doesn't print which pair failed as
             * clearly as a manual message would, and there are 28 pairs —
             * use TEST_ASSERT_MESSAGE-style construction so a failure
             * names the two indices, not just "some pair matched". */
            char msg[64];
            snprintf(msg, sizeof(msg), "brand palette[%u] == palette[%u] (0x%06X)", (unsigned)i, (unsigned)j,
                      (unsigned)seen[i]);
            TEST_ASSERT_TRUE_MESSAGE(seen[i] != seen[j], msg);
        }
    }
}

/* Mutation-conscious: a mutant that reverts to the ORIGINAL 4-entry table
 * (i.e. wraps mod-4 instead of mod-8) would still pass the two tests
 * above for indices 0-3, and would make 4-7 silently equal 0-3 — this is
 * exactly the duplicate-across-all-8 test's job, but pinned again here
 * with an explicit named comparison against each low index, so a
 * reviewer reading test output sees "index 4 collided with index 0" by
 * name, not just "a pair matched somewhere in 0-7". */
static void S17a_AC1_indices_4_to_7_do_not_wrap_back_onto_0_to_3(void)
{
    for (uint8_t hi = 4; hi < 8; hi++) {
        for (uint8_t lo = 0; lo < 4; lo++) {
            TEST_ASSERT_NOT_EQUAL(ff_theme_crew_color(lo, false), ff_theme_crew_color(hi, false));
        }
    }
}

/* The out-of-range wrap contract (this header's own doc comment: "honest
 * but safe, never a crash") — index 8 must equal index 0, not read past
 * the table. Also proves the table really is 8 entries long: if a
 * mutant shrank it back to 4, index 8 would (8 % 4 == 0) still equal
 * index 0 too, so this alone isn't sufficient — paired with the
 * distinctness tests above, which DO distinguish an 8-entry table from a
 * 4-entry one. */
static void S17a_AC1_brand_out_of_range_index_wraps_modulo_8(void)
{
    TEST_ASSERT_EQUAL_HEX32(ff_theme_crew_color(0, false), ff_theme_crew_color(8, false));
    TEST_ASSERT_EQUAL_HEX32(ff_theme_crew_color(5, false), ff_theme_crew_color(13, false));
    TEST_ASSERT_EQUAL_HEX32(ff_theme_crew_color(7, false), ff_theme_crew_color(255, false));
}

/* ---------------------------------------------------------------------
 * AC2 — colorblind-safe palette.
 * ------------------------------------------------------------------- */

static void S17a_AC2_colorblind_palette_has_no_duplicates_across_all_8_indices(void)
{
    uint32_t seen[8];
    for (uint8_t i = 0; i < 8; i++) {
        seen[i] = ff_theme_crew_color(i, true);
    }
    for (uint8_t i = 0; i < 8; i++) {
        for (uint8_t j = i + 1; j < 8; j++) {
            char msg[64];
            snprintf(msg, sizeof(msg), "colorblind palette[%u] == palette[%u] (0x%06X)", (unsigned)i, (unsigned)j,
                      (unsigned)seen[i]);
            TEST_ASSERT_TRUE_MESSAGE(seen[i] != seen[j], msg);
        }
    }
}

static void S17a_AC2_colorblind_out_of_range_index_wraps_modulo_8(void)
{
    TEST_ASSERT_EQUAL_HEX32(ff_theme_crew_color(0, true), ff_theme_crew_color(8, true));
    TEST_ASSERT_EQUAL_HEX32(ff_theme_crew_color(7, true), ff_theme_crew_color(255, true));
}

/* colorblind=false and colorblind=true must select genuinely DIFFERENT
 * tables, not the same one under two names — a mutant that ignores the
 * `colorblind` parameter entirely (always returns the brand palette)
 * would pass every test above except this one. */
static void S17a_AC2_colorblind_flag_actually_changes_the_palette(void)
{
    int at_least_one_differs = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (ff_theme_crew_color(i, false) != ff_theme_crew_color(i, true)) {
            at_least_one_differs = 1;
        }
    }
    TEST_ASSERT_TRUE(at_least_one_differs);

    /* Stronger than "at least one" — every index differs, since these
     * are two entirely separate 8-entry tables, not a partial override. */
    for (uint8_t i = 0; i < 8; i++) {
        TEST_ASSERT_NOT_EQUAL(ff_theme_crew_color(i, false), ff_theme_crew_color(i, true));
    }
}

/* AC2's exact wording: colorblind=true "for all 8 indices", colorblind=false
 * "the brand palette" — re-asserts the brand-palette identity (already
 * pinned above) is unaffected by ever having called with colorblind=true
 * in between, i.e. no shared mutable state between the two calls (a
 * `static` table-selection cache would be the mutation this catches). */
static void S17a_AC2_toggling_colorblind_does_not_perturb_the_brand_palette(void)
{
    (void)ff_theme_crew_color(3, true);
    (void)ff_theme_crew_color(7, true);
    TEST_ASSERT_EQUAL_HEX32(0xFF5CA8u, ff_theme_crew_color(0, false));
    TEST_ASSERT_EQUAL_HEX32(0x9BE07Bu, ff_theme_crew_color(3, false));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S17a_AC1_brand_indices_0_to_3_are_the_exact_original_hexes);
    RUN_TEST(S17a_AC1_brand_palette_has_no_duplicates_across_all_8_indices);
    RUN_TEST(S17a_AC1_indices_4_to_7_do_not_wrap_back_onto_0_to_3);
    RUN_TEST(S17a_AC1_brand_out_of_range_index_wraps_modulo_8);

    RUN_TEST(S17a_AC2_colorblind_palette_has_no_duplicates_across_all_8_indices);
    RUN_TEST(S17a_AC2_colorblind_out_of_range_index_wraps_modulo_8);
    RUN_TEST(S17a_AC2_colorblind_flag_actually_changes_the_palette);
    RUN_TEST(S17a_AC2_toggling_colorblind_does_not_perturb_the_brand_palette);

    return UNITY_END();
}
