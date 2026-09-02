/**
 * test_flare_mark.c — core/include/ff_flare_mark.h: the shared Firefly
 * flare-mark geometry table (S26g, docs/specs/S26-device-lifecycle.md
 * "(g) Boot animation" amendment, 2026-09-02).
 *
 * `ff_flare_mark_ray_offset` is a plain `static inline` function (no .c
 * file — see that header's own top comment), so this test #includes the
 * header directly rather than linking a library, matching
 * app/theme/tests/test_theme.c's convention for the same shape of
 * header. Needs libm (sinf/cosf) — this test executable links `ff-core`
 * (which conditionally links `m` on UNIX; see firmware/core/CMakeLists.txt's
 * note on why that's enough for any consumer) rather than adding a
 * second, parallel libm link rule.
 *
 * Independent review (PR #146, FAIL 3): AGENTS.md — "New public API =
 * header + doc comment + unit test, or it doesn't exist" — and this repo's
 * own house failure mode (AGENTS.md's "the proxy check", code-review.md
 * item 6) means the constants below are pinned against LITERAL numbers
 * typed by hand (8, 42.0f, 7.0f, 5.0f), not against the header's own
 * macros re-read — a test that compared FF_FLARE_MARK_N_RAYS to itself
 * would pass for a header edited to say anything at all.
 */
#include <math.h>

#include "unity.h"

#include "ff_flare_mark.h"

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------------------
 * Constants pinned by literal, not by re-reading the header's own
 * macros — see the file's top comment.
 * ------------------------------------------------------------------- */

static void S26g_constants_pinned_by_literal(void)
{
    TEST_ASSERT_EQUAL_INT(8, FF_FLARE_MARK_N_RAYS);
    TEST_ASSERT_EQUAL_FLOAT(42.0f, FF_FLARE_MARK_MAX_LEN_PX);
    TEST_ASSERT_EQUAL_FLOAT(7.0f, FF_FLARE_MARK_CENTER_R_PX);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, FF_FLARE_MARK_LINE_W_PX);
}

/* ---------------------------------------------------------------------
 * ff_flare_mark_ray_offset — geometry.
 * ------------------------------------------------------------------- */

/* Every one of the 8 offsets, scaled by FF_FLARE_MARK_MAX_LEN_PX,
 * reproduces the fraction table's ray length (|(dx,dy)| ==
 * FF_FLARE_MARK_RAY_FRAC[i] * max_len) — this is the guarantee
 * scr_flare.c's flare_build_mark and ff_display.c's boot-splash
 * rasterizer both rely on to draw the identical shape. */
static void S26g_ray_offset_reproduces_fraction_table_lengths(void)
{
    float const max_len = 42.0f;
    for (int i = 0; i < FF_FLARE_MARK_N_RAYS; i++) {
        float dx, dy;
        ff_flare_mark_ray_offset(i, max_len, &dx, &dy);

        float const got_len = sqrtf(dx * dx + dy * dy);
        float const want_len = FF_FLARE_MARK_RAY_FRAC[i] * max_len;
        TEST_ASSERT_FLOAT_WITHIN(0.01f, want_len, got_len);
    }
}

/* Ray 0 (index 0, the deliberate standout — spec: "a long north ray")
 * is straight up at full max_len: (0, -42). Screen +Y is down, so "up"
 * is -Y — pinned here as the literal (0.0f, -42.0f), not derived. */
static void S26g_ray0_points_straight_up_at_full_length(void)
{
    float dx, dy;
    ff_flare_mark_ray_offset(0, 42.0f, &dx, &dy);

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, dx);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -42.0f, dy);
}

/* Ray 4 (south, opposite ray 0) points straight DOWN — the sign flip
 * that makes ray 0's "up is -Y" convention meaningful rather than an
 * accident of one test's tolerance. FF_FLARE_MARK_RAY_FRAC[4] == 0.58,
 * pinned by literal alongside the sign. */
static void S26g_ray4_points_straight_down(void)
{
    float dx, dy;
    ff_flare_mark_ray_offset(4, 42.0f, &dx, &dy);

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, dx);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 24.36f, dy); /* 0.58f * 42.0f, literal */
}

/* Symmetry pairs (1<->7, 2<->6, 3<->5): each pair shares a fraction
 * table entry (FF_FLARE_MARK_RAY_FRAC[1]==[7], [2]==[6], [3]==[5]) and
 * is mirrored about the north-south (vertical) axis — same dy, negated
 * dx. A mutant that broke the mirroring (e.g. a wrong angle sign, or an
 * index computed clockwise-vs-counterclockwise inconsistently across
 * the table) would flip this without necessarily breaking the
 * length-only check above. */
static void S26g_symmetry_pairs_mirror_dx_match_dy(void)
{
    int const pairs[3][2] = {{1, 7}, {2, 6}, {3, 5}};

    for (int p = 0; p < 3; p++) {
        int const i = pairs[p][0];
        int const j = pairs[p][1];
        TEST_ASSERT_EQUAL_FLOAT(FF_FLARE_MARK_RAY_FRAC[i], FF_FLARE_MARK_RAY_FRAC[j]);

        float dxi, dyi, dxj, dyj;
        ff_flare_mark_ray_offset(i, 42.0f, &dxi, &dyi);
        ff_flare_mark_ray_offset(j, 42.0f, &dxj, &dyj);

        TEST_ASSERT_FLOAT_WITHIN(0.01f, -dxi, dxj); /* mirrored horizontally */
        TEST_ASSERT_FLOAT_WITHIN(0.01f, dyi, dyj);  /* same vertical offset */
    }
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S26g_constants_pinned_by_literal);
    RUN_TEST(S26g_ray_offset_reproduces_fraction_table_lengths);
    RUN_TEST(S26g_ray0_points_straight_up_at_full_length);
    RUN_TEST(S26g_ray4_points_straight_down);
    RUN_TEST(S26g_symmetry_pairs_mirror_dx_match_dy);

    return UNITY_END();
}
