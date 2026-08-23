/**
 * test_png_diff.c — S14 slice b golden-diff threshold tests.
 *
 * The load-bearing cases are the exact 0.5% boundary tests (Wave
 * lessons: "exact boundary tests where applicable (threshold 0.5% —
 * test a 0.4% and 0.6% synthetic diff)"). Uses a synthetic 1000-pixel
 * buffer (40x25) so percentages land on exact, easy-to-reason-about
 * integers: 4/1000 = 0.4%, 5/1000 = 0.5% (the threshold itself,
 * inclusive per spec: "≤0.5%"), 6/1000 = 0.6%.
 */
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "png_diff.h"

#define FF_TEST_W 40u
#define FF_TEST_H 25u
#define FF_TEST_N_PIXELS (FF_TEST_W * FF_TEST_H) /* 1000 */

static uint8_t g_a[FF_TEST_N_PIXELS * 3];
static uint8_t g_b[FF_TEST_N_PIXELS * 3];

void setUp(void)
{
    /* Identical mid-gray images to start; each test flips however many
     * pixels it needs in g_b. */
    memset(g_a, 0x80, sizeof(g_a));
    memset(g_b, 0x80, sizeof(g_b));
}

void tearDown(void) {}

static void flip_n_pixels(uint8_t *buf, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        buf[i * 3 + 0] = 0xFF - buf[i * 3 + 0];
        buf[i * 3 + 1] = 0xFF - buf[i * 3 + 1];
        buf[i * 3 + 2] = 0xFF - buf[i * 3 + 2];
    }
}

/* ---------------------------------------------------------------------
 * The boundary: 0.4% passes, exactly 0.5% passes (inclusive), 0.6% fails.
 * ------------------------------------------------------------------- */

static void diff_0_4_pct_passes_at_0_5_threshold(void)
{
    flip_n_pixels(g_b, 4); /* 4/1000 = 0.4% */
    ff_png_diff_result_t r;
    ff_png_diff_compare(g_a, FF_TEST_W, FF_TEST_H, g_b, FF_TEST_W, FF_TEST_H, 0.5, &r);

    TEST_ASSERT_TRUE(r.dims_match);
    TEST_ASSERT_EQUAL_UINT64(4, r.n_diff_pixels);
    TEST_ASSERT_EQUAL_DOUBLE(0.4, r.diff_pct);
    TEST_ASSERT_TRUE(r.within_threshold);
}

static void diff_exactly_0_5_pct_passes_inclusive(void)
{
    flip_n_pixels(g_b, 5); /* 5/1000 = 0.5%, the threshold itself */
    ff_png_diff_result_t r;
    ff_png_diff_compare(g_a, FF_TEST_W, FF_TEST_H, g_b, FF_TEST_W, FF_TEST_H, 0.5, &r);

    TEST_ASSERT_EQUAL_UINT64(5, r.n_diff_pixels);
    TEST_ASSERT_EQUAL_DOUBLE(0.5, r.diff_pct);
    TEST_ASSERT_TRUE(r.within_threshold); /* spec: "≤0.5%", inclusive */
}

static void diff_0_6_pct_fails_at_0_5_threshold(void)
{
    flip_n_pixels(g_b, 6); /* 6/1000 = 0.6% */
    ff_png_diff_result_t r;
    ff_png_diff_compare(g_a, FF_TEST_W, FF_TEST_H, g_b, FF_TEST_W, FF_TEST_H, 0.5, &r);

    TEST_ASSERT_EQUAL_UINT64(6, r.n_diff_pixels);
    TEST_ASSERT_EQUAL_DOUBLE(0.6, r.diff_pct);
    TEST_ASSERT_FALSE(r.within_threshold);
}

/* ---------------------------------------------------------------------
 * Other cases.
 * ------------------------------------------------------------------- */

static void identical_images_pass(void)
{
    ff_png_diff_result_t r;
    ff_png_diff_compare(g_a, FF_TEST_W, FF_TEST_H, g_b, FF_TEST_W, FF_TEST_H, 0.5, &r);

    TEST_ASSERT_EQUAL_UINT64(0, r.n_diff_pixels);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.diff_pct);
    TEST_ASSERT_TRUE(r.within_threshold);
}

static void completely_different_images_fail(void)
{
    flip_n_pixels(g_b, FF_TEST_N_PIXELS);
    ff_png_diff_result_t r;
    ff_png_diff_compare(g_a, FF_TEST_W, FF_TEST_H, g_b, FF_TEST_W, FF_TEST_H, 0.5, &r);

    TEST_ASSERT_EQUAL_UINT64(FF_TEST_N_PIXELS, r.n_diff_pixels);
    TEST_ASSERT_EQUAL_DOUBLE(100.0, r.diff_pct);
    TEST_ASSERT_FALSE(r.within_threshold);
}

static void zero_threshold_requires_byte_exact(void)
{
    flip_n_pixels(g_b, 1); /* smallest possible difference: 1 pixel */
    ff_png_diff_result_t r;
    ff_png_diff_compare(g_a, FF_TEST_W, FF_TEST_H, g_b, FF_TEST_W, FF_TEST_H, 0.0, &r);
    TEST_ASSERT_FALSE(r.within_threshold);

    ff_png_diff_result_t r2;
    ff_png_diff_compare(g_a, FF_TEST_W, FF_TEST_H, g_a, FF_TEST_W, FF_TEST_H, 0.0, &r2);
    TEST_ASSERT_TRUE(r2.within_threshold); /* identical buffers still pass at threshold 0 */
}

static void dimension_mismatch_is_a_hard_failure_not_scored(void)
{
    ff_png_diff_result_t r;
    /* Even with a generous 100% threshold, mismatched dimensions never pass. */
    ff_png_diff_compare(g_a, FF_TEST_W, FF_TEST_H, g_b, FF_TEST_W + 1, FF_TEST_H, 100.0, &r);

    TEST_ASSERT_FALSE(r.dims_match);
    TEST_ASSERT_FALSE(r.within_threshold);
    TEST_ASSERT_EQUAL_UINT64(0, r.n_total_pixels);
}

/* ---------------------------------------------------------------------
 * Side-by-side diff render.
 * ------------------------------------------------------------------- */

static void sidebyside_layout_places_a_b_and_highlight_correctly(void)
{
    /* 2x1 image: pixel 0 differs, pixel 1 doesn't. */
    uint8_t a[2 * 3] = {10, 20, 30, 40, 50, 60};
    uint8_t b[2 * 3] = {99, 99, 99, 40, 50, 60};

    size_t sz = ff_png_diff_sidebyside_size(2, 1);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)(3 * 2) * 1 * 3, (uint64_t)sz);

    uint8_t *out = malloc(sz);
    TEST_ASSERT_NOT_NULL(out);
    ff_png_diff_render_sidebyside(a, b, 2, 1, out);

    /* Panel A occupies columns [0,2), panel B [2,4), diff [4,6); each
     * row is 6 pixels wide (3*w = 6), 3 bytes/pixel. */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, out, 6);        /* panel A == source a, verbatim */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(b, out + 6, 6);     /* panel B == source b, verbatim */

    uint8_t const *diff_px0 = out + 12;  /* column 4 (pixel 0's diff cell): differs -> red */
    uint8_t const *diff_px1 = out + 15;  /* column 5 (pixel 1's diff cell): same -> black */
    TEST_ASSERT_EQUAL_UINT8(255, diff_px0[0]);
    TEST_ASSERT_EQUAL_UINT8(0, diff_px0[1]);
    TEST_ASSERT_EQUAL_UINT8(0, diff_px0[2]);
    TEST_ASSERT_EQUAL_UINT8(0, diff_px1[0]);
    TEST_ASSERT_EQUAL_UINT8(0, diff_px1[1]);
    TEST_ASSERT_EQUAL_UINT8(0, diff_px1[2]);

    free(out);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(diff_0_4_pct_passes_at_0_5_threshold);
    RUN_TEST(diff_exactly_0_5_pct_passes_inclusive);
    RUN_TEST(diff_0_6_pct_fails_at_0_5_threshold);

    RUN_TEST(identical_images_pass);
    RUN_TEST(completely_different_images_fail);
    RUN_TEST(zero_threshold_requires_byte_exact);
    RUN_TEST(dimension_mismatch_is_a_hard_failure_not_scored);

    RUN_TEST(sidebyside_layout_places_a_b_and_highlight_correctly);

    return UNITY_END();
}
