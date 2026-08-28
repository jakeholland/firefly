/**
 * test_touchcal.c — S15 slice d: per-axis affine touch calibration.
 *
 * Criteria covered (docs/specs/S15d-touch-calibration.md, AC1):
 *   - a known offset+scale is recovered from synthetic capture points;
 *   - degenerate input (no x-spread or no y-spread) returns invalid and
 *     leaves identity (no garbage transform);
 *   - apply clamps to [0, 411].
 *
 * The synthetic points mirror the measured board-2 error: raw is offset +
 * slightly scaled off screen space, no swap/mirror/rotation. We generate
 * raw from a chosen ground-truth (a_gt, b_gt) as raw = (screen - b)/a and
 * check solve() inverts back to (a_gt, b_gt) and that apply(raw) lands on
 * the crosshair it targeted.
 */
#include <math.h>
#include <string.h>

#include "unity.h"

#include "ff_touchcal.h"

void setUp(void) {}
void tearDown(void) {}

/* The five spec targets (screen space): center + four insets. */
static const int TARGET_X[5] = {206, 90, 322, 90, 322};
static const int TARGET_Y[5] = {206, 90, 90, 322, 322};

/* Build a capture where the controller reports raw = a*screen_raw... —
 * i.e. given a ground-truth screen = A*raw + B, we invert to synthesize the
 * raw the controller would have produced for each target: raw = (screen-B)/A.
 * solve() should then recover (A, B). */
static void synth_capture(ff_cal_point_t pts[5], double ax, double bx, double ay, double by)
{
    for (int i = 0; i < 5; i++) {
        pts[i].screen_x = TARGET_X[i];
        pts[i].screen_y = TARGET_Y[i];
        pts[i].raw_x = (int)lround(((double)TARGET_X[i] - bx) / ax);
        pts[i].raw_y = (int)lround(((double)TARGET_Y[i] - by) / ay);
    }
}

/* --------------------------------------------------------------------- */
/* AC1 — recover a known offset+scale from synthetic points.             */
/* --------------------------------------------------------------------- */

static void S15d_AC1_recovers_known_offset_and_scale(void)
{
    /* Ground truth close to the measured board-2 error: both axes read a
     * bit high and top out near raw 406, so screen ≈ ~1.013*raw - ~14. */
    const double ax = 411.0 / 406.0; /* ~1.0123 */
    const double bx = -14.0;
    const double ay = 411.0 / 406.0;
    const double by = -18.0;

    ff_cal_point_t pts[5];
    synth_capture(pts, ax, bx, ay, by);

    ff_touchcal_t c;
    memset(&c, 0xAA, sizeof(c));
    bool ok = ff_touchcal_solve(pts, 5, &c);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(c.valid);
    /* Recovered params match ground truth to within rounding of the
     * synthetic integer raw coords. */
    TEST_ASSERT_FLOAT_WITHIN(0.02f, (float)ax, c.ax);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, (float)ay, c.ay);
    TEST_ASSERT_FLOAT_WITHIN(3.0f, (float)bx, c.bx);
    TEST_ASSERT_FLOAT_WITHIN(3.0f, (float)by, c.by);

    /* And, the property that actually matters: applying the fit to each
     * captured raw lands back on the crosshair it targeted (±2 px). */
    for (int i = 0; i < 5; i++) {
        int sx, sy;
        ff_touchcal_apply(&c, pts[i].raw_x, pts[i].raw_y, &sx, &sy);
        TEST_ASSERT_INT_WITHIN(2, TARGET_X[i], sx);
        TEST_ASSERT_INT_WITHIN(2, TARGET_Y[i], sy);
    }
}

static void S15d_AC1_recovers_pure_offset(void)
{
    /* Pure offset (scale 1): the "center reads ~14 high" part alone. */
    ff_cal_point_t pts[5];
    synth_capture(pts, 1.0, -14.0, 1.0, -19.0);

    ff_touchcal_t c;
    bool ok = ff_touchcal_solve(pts, 5, &c);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(c.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, c.ax);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, c.ay);
    TEST_ASSERT_FLOAT_WITHIN(2.0f, -14.0f, c.bx);
    TEST_ASSERT_FLOAT_WITHIN(2.0f, -19.0f, c.by);
}

/* --------------------------------------------------------------------- */
/* AC1 — degenerate input returns invalid/identity (no garbage).         */
/* --------------------------------------------------------------------- */

static void assert_identity(const ff_touchcal_t *c)
{
    TEST_ASSERT_FALSE(c->valid);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, c->ax);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, c->bx);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, c->ay);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, c->by);
}

static void S15d_AC1_no_x_spread_is_degenerate_identity(void)
{
    /* Every raw_x identical -> x axis has no spread -> degenerate. y is
     * fine, but per-axis both must be well-conditioned. */
    ff_cal_point_t pts[5];
    for (int i = 0; i < 5; i++) {
        pts[i].raw_x = 200; /* flat */
        pts[i].screen_x = TARGET_X[i];
        pts[i].raw_y = TARGET_Y[i];
        pts[i].screen_y = TARGET_Y[i];
    }

    ff_touchcal_t c;
    memset(&c, 0xAA, sizeof(c));
    bool ok = ff_touchcal_solve(pts, 5, &c);
    TEST_ASSERT_FALSE(ok);
    assert_identity(&c);
}

static void S15d_AC1_no_y_spread_is_degenerate_identity(void)
{
    ff_cal_point_t pts[5];
    for (int i = 0; i < 5; i++) {
        pts[i].raw_x = TARGET_X[i];
        pts[i].screen_x = TARGET_X[i];
        pts[i].raw_y = 200; /* flat */
        pts[i].screen_y = TARGET_Y[i];
    }

    ff_touchcal_t c;
    memset(&c, 0xAA, sizeof(c));
    bool ok = ff_touchcal_solve(pts, 5, &c);
    TEST_ASSERT_FALSE(ok);
    assert_identity(&c);
}

static void S15d_AC1_too_few_points_is_identity(void)
{
    ff_cal_point_t pts[1] = {{200, 200, 206, 206}};

    ff_touchcal_t c;
    memset(&c, 0xAA, sizeof(c));
    TEST_ASSERT_FALSE(ff_touchcal_solve(pts, 1, &c));
    assert_identity(&c);

    /* n=0 / NULL are identity too. */
    memset(&c, 0xAA, sizeof(c));
    TEST_ASSERT_FALSE(ff_touchcal_solve(pts, 0, &c));
    assert_identity(&c);
    memset(&c, 0xAA, sizeof(c));
    TEST_ASSERT_FALSE(ff_touchcal_solve(NULL, 5, &c));
    assert_identity(&c);
}

/* --------------------------------------------------------------------- */
/* AC1 — apply clamps to [0, 411], identity passes through.              */
/* --------------------------------------------------------------------- */

static void S15d_AC1_apply_clamps_to_panel(void)
{
    /* A valid transform that pushes a corner far off-panel must clamp. */
    ff_touchcal_t c = {.ax = 2.0f, .bx = 0.0f, .ay = 2.0f, .by = 0.0f, .valid = true};
    int sx, sy;

    ff_touchcal_apply(&c, 400, 400, &sx, &sy); /* 800,800 -> clamp 411 */
    TEST_ASSERT_EQUAL_INT(411, sx);
    TEST_ASSERT_EQUAL_INT(411, sy);

    /* Negative result clamps to 0. */
    ff_touchcal_t neg = {.ax = 1.0f, .bx = -1000.0f, .ay = 1.0f, .by = -1000.0f, .valid = true};
    ff_touchcal_apply(&neg, 10, 10, &sx, &sy);
    TEST_ASSERT_EQUAL_INT(0, sx);
    TEST_ASSERT_EQUAL_INT(0, sy);
}

static void S15d_AC1_apply_identity_passes_through_clamped(void)
{
    ff_touchcal_t id;
    ff_touchcal_identity(&id);
    int sx, sy;

    ff_touchcal_apply(&id, 206, 206, &sx, &sy);
    TEST_ASSERT_EQUAL_INT(206, sx);
    TEST_ASSERT_EQUAL_INT(206, sy);

    /* NULL cal is identity too, and still clamps an off-panel raw. */
    ff_touchcal_apply(NULL, 500, -5, &sx, &sy);
    TEST_ASSERT_EQUAL_INT(411, sx);
    TEST_ASSERT_EQUAL_INT(0, sy);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S15d_AC1_recovers_known_offset_and_scale);
    RUN_TEST(S15d_AC1_recovers_pure_offset);
    RUN_TEST(S15d_AC1_no_x_spread_is_degenerate_identity);
    RUN_TEST(S15d_AC1_no_y_spread_is_degenerate_identity);
    RUN_TEST(S15d_AC1_too_few_points_is_identity);
    RUN_TEST(S15d_AC1_apply_clamps_to_panel);
    RUN_TEST(S15d_AC1_apply_identity_passes_through_clamped);
    return UNITY_END();
}
