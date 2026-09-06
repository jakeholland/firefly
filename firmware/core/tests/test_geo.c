/**
 * test_geo.c — S01 core/geo acceptance criteria.
 *
 * Test names follow docs/specs/S01-core-geo.md's numbered acceptance
 * criteria: S01_ACn_description. Table-driven where the spec implies a set
 * of cases (distance/bearing/arrow/angdiff/heading fixtures).
 *
 * AC8 ("no libm beyond math.h; builds clean with -Wall -Wextra -Werror as
 * C11") is a build-level property, not a runtime assertion — it's enforced
 * by the CMake gate (ff_apply_warnings) and by this file (and ff_geo.c)
 * including nothing but <math.h>/<stdbool.h>/"unity.h"/"ff_geo.h". The
 * S01_AC8 test below is a light smoke check that the module is usable end
 * to end; the real proof is the green, warning-free build.
 */
#include <math.h>

#include "unity.h"

#include "ff_geo.h"

#define FF_TEST_PI 3.14159265358979323846
#define FF_TEST_EARTH_RADIUS_M 6371000.0

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* AC1 — distance                                                      */
/* ------------------------------------------------------------------- */

static void S01_AC1_distance_equator_one_degree_lon_is_analytic(void)
{
    /* On the equator, 1 degree of longitude is exactly R * (pi/180) of
     * great-circle arc — haversine reduces to this identically. */
    ff_latlon_t a = {0.0, 0.0};
    ff_latlon_t b = {0.0, 1.0};
    double expected = FF_TEST_EARTH_RADIUS_M * (FF_TEST_PI / 180.0);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, (float)expected, ff_geo_distance_m(a, b));
}

static void S01_AC1_distance_one_degree_lat_is_analytic(void)
{
    /* Any meridian: 1 degree of latitude is exactly R * (pi/180) too. */
    ff_latlon_t a = {10.0, 0.0};
    ff_latlon_t b = {11.0, 0.0};
    double expected = FF_TEST_EARTH_RADIUS_M * (FF_TEST_PI / 180.0);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, (float)expected, ff_geo_distance_m(a, b));
}

static void S01_AC1_distance_identical_points_is_zero(void)
{
    ff_latlon_t a = {39.9012, -82.4562};
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ff_geo_distance_m(a, a));
}

static void S01_AC1_distance_known_pair_within_half_percent(void)
{
    /* Legend Valley "bowl" stage -> "Wompy" camping landmark (Thornville,
     * OH festival grounds; approximate, not surveyed — a stable regression
     * fixture rather than a literal published vector, see PR notes).
     * Cross-checked independently here via a flat equirectangular
     * approximation, which is accurate to a small fraction of a percent at
     * this sub-kilometer scale, rather than by re-deriving haversine. */
    ff_latlon_t bowl = {39.9012, -82.4562};
    ff_latlon_t wompy = {39.9050, -82.4510};

    double lat0 = (bowl.lat + wompy.lat) * 0.5 * (FF_TEST_PI / 180.0);
    double dx = (wompy.lon - bowl.lon) * (FF_TEST_PI / 180.0) * cos(lat0) * FF_TEST_EARTH_RADIUS_M;
    double dy = (wompy.lat - bowl.lat) * (FF_TEST_PI / 180.0) * FF_TEST_EARTH_RADIUS_M;
    double flat_expected = sqrt(dx * dx + dy * dy);

    float got = ff_geo_distance_m(bowl, wompy);
    float tolerance = (float)(flat_expected * 0.005);
    TEST_ASSERT_FLOAT_WITHIN(tolerance, (float)flat_expected, got);
}

/* ------------------------------------------------------------------- */
/* AC2 — bearing                                                       */
/* ------------------------------------------------------------------- */

static void S01_AC2_bearing_cardinal_directions(void)
{
    ff_latlon_t origin = {0.0, 0.0};
    struct {
        ff_latlon_t to;
        float expected_deg;
        const char *label;
    } cases[] = {
        {{1.0, 0.0}, 0.0f, "due north"},
        {{0.0, 1.0}, 90.0f, "due east"},
        {{-1.0, 0.0}, 180.0f, "due south"},
        {{0.0, -1.0}, 270.0f, "due west"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        float got = ff_geo_bearing_deg(origin, cases[i].to);
        TEST_ASSERT_FLOAT_WITHIN(0.1f, cases[i].expected_deg, got);
    }
}

static void S01_AC2_bearing_wraps_across_antimeridian(void)
{
    /* From (0,179) to (0,-179): 2 degrees further east, crossing the
     * dateline. The short way is due east. */
    ff_latlon_t from = {0.0, 179.0};
    ff_latlon_t to = {0.0, -179.0};
    float got = ff_geo_bearing_deg(from, to);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 90.0f, got);
}

static void S01_AC2_bearing_identical_points_returns_zero_no_nan(void)
{
    ff_latlon_t p = {39.9012, -82.4562};
    float got = ff_geo_bearing_deg(p, p);
    TEST_ASSERT_FALSE(isnan(got));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, got);
}

static void S01_AC2_bearing_near_identical_points_finite_in_range(void)
{
    ff_latlon_t from = {39.9012, -82.4562};
    ff_latlon_t to = {39.9012 + 1e-9, -82.4562 + 1e-9};
    float got = ff_geo_bearing_deg(from, to);
    TEST_ASSERT_FALSE(isnan(got));
    TEST_ASSERT_TRUE(got >= 0.0f && got < 360.0f);
}

/* ------------------------------------------------------------------- */
/* AC3 — arrow_deg == wrap(bearing - heading)                          */
/* ------------------------------------------------------------------- */

static void S01_AC3_arrow_deg_matches_wrap_of_bearing_minus_heading(void)
{
    struct {
        float bearing;
        float heading;
    } cases[] = {
        {0.0f, 0.0f},
        {90.0f, 0.0f},
        {0.0f, 90.0f},
        {350.0f, 10.0f},
        {10.0f, 350.0f},
        {180.0f, 180.0f},
        {45.0f, 315.0f},
        {359.9f, 0.2f},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        float expected = ff_geo_wrap_deg(cases[i].bearing - cases[i].heading);
        float got = ff_geo_arrow_deg(cases[i].bearing, cases[i].heading);
        TEST_ASSERT_FLOAT_WITHIN(0.001f, expected, got);
        TEST_ASSERT_TRUE(got >= 0.0f && got < 360.0f);
    }
}

/* ------------------------------------------------------------------- */
/* AC4 — tilt-compensated heading                                      */
/* ------------------------------------------------------------------- */

/* Synthetic mag+accel fixtures: board frame is +x=right, +y=forward,
 * +z=up; world field has a 60 degree dip (typical mid-latitude), no
 * declination. Generated from an independently-derived ENU rotation model
 * (see PR description) — yaw is the true heading of the device's forward
 * (+y) axis, tilt is a pure rotation about the right (x) axis so the
 * accel-derived tilt magnitude equals the fixture's tilt exactly. */
typedef struct {
    ff_vec3_t accel;
    ff_vec3_t mag;
    float yaw_deg;
    float tilt_deg;
} ff_heading_fixture_t;

static const ff_heading_fixture_t kHeadingFixtures[] = {
    {{0.000000000f, 0.000000000f, 1.000000000f}, {0.000000000f, 0.500000000f, -0.866025404f}, 0.0f, 0.0f},
    {{0.000000000f, -0.342020143f, 0.939692621f}, {0.000000000f, 0.766044443f, -0.642787610f}, 0.0f, 20.0f},
    {{0.000000000f, -0.642787610f, 0.766044443f}, {0.000000000f, 0.939692621f, -0.342020143f}, 0.0f, 40.0f},
    {{0.000000000f, 0.000000000f, 1.000000000f}, {-0.500000000f, 0.000000000f, -0.866025404f}, 90.0f, 0.0f},
    {{0.000000000f, -0.342020143f, 0.939692621f}, {-0.500000000f, 0.296198133f, -0.813797681f}, 90.0f, 20.0f},
    {{0.000000000f, -0.642787610f, 0.766044443f}, {-0.500000000f, 0.556670399f, -0.663413948f}, 90.0f, 40.0f},
    {{0.000000000f, 0.000000000f, 1.000000000f}, {0.000000000f, -0.500000000f, -0.866025404f}, 180.0f, 0.0f},
    {{0.000000000f, -0.342020143f, 0.939692621f}, {0.000000000f, -0.173648178f, -0.984807753f}, 180.0f, 20.0f},
    {{0.000000000f, -0.642787610f, 0.766044443f}, {0.000000000f, 0.173648178f, -0.984807753f}, 180.0f, 40.0f},
    {{0.000000000f, 0.000000000f, 1.000000000f}, {0.500000000f, 0.000000000f, -0.866025404f}, 270.0f, 0.0f},
    {{0.000000000f, -0.342020143f, 0.939692621f}, {0.500000000f, 0.296198133f, -0.813797681f}, 270.0f, 20.0f},
    {{0.000000000f, -0.642787610f, 0.766044443f}, {0.500000000f, 0.556670399f, -0.663413948f}, 270.0f, 40.0f},
};

static void S01_AC4_heading_within_2deg_at_0_20_40_tilt(void)
{
    for (size_t i = 0; i < sizeof(kHeadingFixtures) / sizeof(kHeadingFixtures[0]); i++) {
        const ff_heading_fixture_t *f = &kHeadingFixtures[i];
        float got = ff_geo_heading_deg(f->mag, f->accel, NULL);
        TEST_ASSERT_TRUE(got >= 0.0f);
        float err = fabsf(ff_geo_angdiff_deg(f->yaw_deg, got));
        TEST_ASSERT_FLOAT_WITHIN(2.0f, 0.0f, err);
    }
}

static void S01_AC4_heading_just_under_60deg_tilt_is_still_reliable(void)
{
    /* 59 degrees, comfortably clear of the >60 threshold either way a
     * platform's libm might round the last ULP — must NOT trip the guard. */
    ff_vec3_t accel = {0.000000000f, -0.857167301f, 0.515038075f};
    ff_vec3_t mag = {-0.353553391f, 0.924422115f, -0.142981651f};
    float got = ff_geo_heading_deg(mag, accel, NULL);
    TEST_ASSERT_TRUE(got >= 0.0f);
    float err = fabsf(ff_geo_angdiff_deg(45.0f, got));
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 0.0f, err);
}

static void S01_AC4_heading_beyond_60deg_tilt_returns_negative(void)
{
    struct {
        ff_vec3_t accel;
        ff_vec3_t mag;
    } cases[] = {
        /* 61 degrees */
        {{0.000000000f, -0.874619707f, 0.484809620f}, {-0.353553391f, 0.928848970f, -0.110632684f}},
        /* 70 degrees */
        {{0.000000000f, -0.939692621f, 0.342020143f}, {-0.353553391f, 0.934720063f, 0.036033379f}},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        float got = ff_geo_heading_deg(cases[i].mag, cases[i].accel, NULL);
        TEST_ASSERT_TRUE(got < 0.0f);
    }
}

static void S01_AC4_heading_at_exactly_60deg_tilt_is_reliable(void)
{
    /* Pins the spec's ">60°" wording literally: the guard is
     * `tilt_deg > 60.0f`, so a tilt of exactly 60 degrees must NOT trip it
     * (see the boundary note on ff_geo_heading_deg in ff_geo.h). accel.z is
     * the exact float32 value 0.5f (cos(60deg)) and accel.y is the
     * correctly-rounded float32 value of -sin(60deg); on both toolchains
     * this module ships on (AppleClang/macOS dev, glibc/Ubuntu CI) this
     * normalizes to tilt_deg == 60.0f bit-for-bit, not just "close to 60".
     * If that ever stops holding on some platform's libm, loosen this to a
     * near-60 fixture rather than deleting the boundary coverage. */
    ff_vec3_t accel = {0.000000000f, -0.866025404f, 0.500000000f};
    ff_vec3_t mag = {-0.353553391f, 0.926776695f, -0.126826484f};
    float got = ff_geo_heading_deg(mag, accel, NULL);
    TEST_ASSERT_TRUE(got >= 0.0f);
    float err = fabsf(ff_geo_angdiff_deg(45.0f, got));
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 0.0f, err);
}

/* Guard-mutation coverage: each of these must fail if the guard it targets
 * is deleted from ff_geo_heading_deg. Without the guard, the offending
 * division becomes 0/0 (NaN) rather than a clean early return, so
 * `got < 0.0f` — true today via the guard's `-1.0f` — would instead be
 * false for a NaN result (NaN compares false against everything), flipping
 * these assertions. */

static void S01_AC4_sentinel_zero_accel(void)
{
    /* amag == 0: tilt can't be determined at all. */
    ff_vec3_t accel = {0.0f, 0.0f, 0.0f};
    ff_vec3_t mag = {0.0f, 0.5f, -0.866025404f}; /* otherwise-valid reading */
    float got = ff_geo_heading_deg(mag, accel, NULL);
    TEST_ASSERT_TRUE(got < 0.0f);
    TEST_ASSERT_FALSE(isnan(got));
}

static void S01_AC4_sentinel_zero_mag(void)
{
    /* mag == 0: no horizontal field component to resolve heading from,
     * regardless of accel/tilt. */
    ff_vec3_t accel = {0.0f, 0.0f, 1.0f}; /* level, otherwise valid */
    ff_vec3_t mag = {0.0f, 0.0f, 0.0f};
    float got = ff_geo_heading_deg(mag, accel, NULL);
    TEST_ASSERT_TRUE(got < 0.0f);
    TEST_ASSERT_FALSE(isnan(got));
}

static void S01_AC4_sentinel_mag_parallel_gravity(void)
{
    /* mag is nonzero but points straight along the accel (gravity) axis:
     * down x mag == 0, so there's still no horizontal component, distinct
     * from the mag==0 case above (a guard that only special-cased the zero
     * vector would miss this). */
    ff_vec3_t accel = {0.0f, 0.0f, 1.0f};
    ff_vec3_t mag = {0.0f, 0.0f, 5.0f}; /* parallel to accel's "up" axis */
    float got = ff_geo_heading_deg(mag, accel, NULL);
    TEST_ASSERT_TRUE(got < 0.0f);
    TEST_ASSERT_FALSE(isnan(got));
}

/* ------------------------------------------------------------------- */
/* AC5 — calibration                                                    */
/* ------------------------------------------------------------------- */

static void S01_AC5_calibration_recovers_hard_offset_and_improves_heading(void)
{
    /* True hard/soft-iron corruption applied to unit-ish directions
     * spanning all 8 octants (6 face + 8 corner directions of a cube,
     * fed out of order like a real figure-eight motion would produce). */
    const ff_vec3_t true_offset = {0.05f, -0.03f, 0.02f};
    const float true_scale[3] = {1.2f, 0.9f, 1.1f};
    const float field_radius = 0.5f;

    /* Raw = offset + scale * direction * field_radius, direction from a
     * shuffled cube face+corner set (see PR notes for generation). */
    const ff_vec3_t raw_samples[] = {
        {0.050000f, -0.480000f, 0.020000f},
        {-0.296410f, 0.229808f, -0.297543f},
        {-0.296410f, -0.289808f, -0.297543f},
        {0.396410f, 0.229808f, -0.297543f},
        {0.396410f, -0.289808f, -0.297543f},
        {0.050000f, -0.030000f, 0.570000f},
        {-0.296410f, -0.289808f, 0.337543f},
        {0.396410f, -0.289808f, 0.337543f},
        {-0.550000f, -0.030000f, 0.020000f},
        {0.650000f, -0.030000f, 0.020000f},
        {-0.296410f, 0.229808f, 0.337543f},
        {0.396410f, 0.229808f, 0.337543f},
        {0.050000f, 0.420000f, 0.020000f},
        {0.050000f, -0.030000f, -0.530000f},
    };

    ff_geo_cal_state_t st;
    ff_geo_cal_begin(&st);
    for (size_t i = 0; i < sizeof(raw_samples) / sizeof(raw_samples[0]); i++) {
        ff_geo_cal_feed(&st, raw_samples[i]);
    }

    TEST_ASSERT_EQUAL_INT(100, ff_geo_cal_progress_pct(&st));

    ff_geo_cal_t cal;
    TEST_ASSERT_TRUE(ff_geo_cal_finish(&st, &cal));

    float tol = 0.05f * field_radius;
    TEST_ASSERT_FLOAT_WITHIN(tol, true_offset.x, cal.hard_offset.x);
    TEST_ASSERT_FLOAT_WITHIN(tol, true_offset.y, cal.hard_offset.y);
    TEST_ASSERT_FLOAT_WITHIN(tol, true_offset.z, cal.hard_offset.z);

    /* Post-cal heading error must drop under 3 degrees for a fresh sample
     * at a known true yaw, tilted 15 degrees. Un-calibrated, the same
     * corruption produces a much larger error, proving the calibration
     * actually does something. */
    const float true_yaw = 37.0f;
    ff_vec3_t accel = {0.000000f, -0.258819f, 0.965926f}; /* 15deg pure tilt at yaw 37 */
    ff_vec3_t true_mag = {-0.150454f, 0.304928f, -0.366583f};
    ff_vec3_t corrupt_mag = {
        true_offset.x + true_scale[0] * true_mag.x,
        true_offset.y + true_scale[1] * true_mag.y,
        true_offset.z + true_scale[2] * true_mag.z,
    };

    float hdg_uncal = ff_geo_heading_deg(corrupt_mag, accel, NULL);
    float hdg_cal = ff_geo_heading_deg(corrupt_mag, accel, &cal);

    float err_uncal = fabsf(ff_geo_angdiff_deg(true_yaw, hdg_uncal));
    float err_cal = fabsf(ff_geo_angdiff_deg(true_yaw, hdg_cal));

    TEST_ASSERT_TRUE(err_uncal > 3.0f);
    TEST_ASSERT_FLOAT_WITHIN(3.0f, 0.0f, err_cal);
}

static void S01_AC5_calibration_below_70pct_coverage_finish_fails(void)
{
    ff_geo_cal_state_t st;
    ff_geo_cal_begin(&st);

    /* A handful of samples clustered in roughly the same direction: far
     * short of the 8-octant sweep a real figure-eight produces. */
    ff_vec3_t samples[] = {
        {0.40f, 0.05f, 0.10f},
        {0.42f, 0.06f, 0.11f},
        {0.38f, 0.04f, 0.09f},
    };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        ff_geo_cal_feed(&st, samples[i]);
    }

    TEST_ASSERT_TRUE(ff_geo_cal_progress_pct(&st) < 70);

    ff_geo_cal_t cal;
    TEST_ASSERT_FALSE(ff_geo_cal_finish(&st, &cal));
}

/* ------------------------------------------------------------------- */
/* AC5 — coverage honesty (S12 step 3 bench finding, PR #219)           */
/*                                                                       */
/* Bench finding (2026-09-06, real puck): a motionless puck on the      */
/* bench reached 100% progress and a finishable "custom" calibration    */
/* after ~88 samples, raw mag constant to noise (-52,238,-28) +/-3 LSB. */
/* Root cause: octant classification was relative to the *running*      */
/* center estimate (min+max)/2 with no check that the running span was  */
/* actually meaningful, so sensor noise around a single point flips     */
/* signs relative to that wobbling center and "covers" octants that     */
/* were never really visited. These three tests must fail against the   */
/* pre-fix code (run first to confirm — see PR #219's comment) and pass */
/* once ff_geo_cal_feed/_finish gate on relative, scale-invariant       */
/* thresholds (FF_GEO_CAL_MIN_SPAN_RATIO, FF_GEO_CAL_MIN_AXIS_RATIO).   */
/* ------------------------------------------------------------------- */

/* Deterministic +/-3 "noise" cycle (no RNG, portable/reproducible across
 * clang and gcc-14): three different-period integer sawtooths so the
 * three axes don't move in lockstep. */
static float s01_ac5_noise(unsigned i, unsigned period)
{
    return (float)((int)(i % period) - (int)(period / 2));
}

static void S01_AC5_stationary_device_never_reaches_finishable_coverage(void)
{
    ff_geo_cal_state_t st;
    ff_geo_cal_begin(&st);

    /* Real bench vector, +/-3 LSB noise per axis, 300 samples. */
    for (unsigned i = 0; i < 300; i++) {
        ff_vec3_t mag = {
            -52.0f + s01_ac5_noise(i, 7),
            238.0f + s01_ac5_noise(i * 3u, 7),
            -28.0f + s01_ac5_noise(i * 5u, 7),
        };
        ff_geo_cal_feed(&st, mag);
    }

    /* At most one octant (noise straddling a running center can validly
     * flip one bit at the very margins) — nowhere near enough to imply
     * real sphere coverage. A motionless device must stay at 0 in the
     * common case; 12% (1/8) is the documented worst-case ceiling. */
    TEST_ASSERT_TRUE(ff_geo_cal_progress_pct(&st) <= 12);

    ff_geo_cal_t cal;
    TEST_ASSERT_FALSE(ff_geo_cal_finish(&st, &cal));
}

static void S01_AC5_needle_along_one_axis_finish_fails(void)
{
    ff_geo_cal_state_t st;
    ff_geo_cal_begin(&st);

    /* Samples spread across a wide range on X only; Y/Z carry only
     * sensor-noise-scale variation. A real figure-eight covers the whole
     * field sphere — a single swung axis is not that, even if enough of
     * the octant *mask* bits happen to flip from axis-independent noise
     * on Y/Z (which is why the shape check in ff_geo_cal_finish, not
     * progress_pct, is what must catch this). */
    const float radius = 250.0f;
    for (unsigned i = 0; i < 300; i++) {
        float x = -radius + (2.0f * radius) * ((float)i / 299.0f);
        ff_vec3_t mag = {
            x,
            s01_ac5_noise(i * 3u, 7),
            s01_ac5_noise(i * 5u, 7),
        };
        ff_geo_cal_feed(&st, mag);
    }

    ff_geo_cal_t cal;
    TEST_ASSERT_FALSE(ff_geo_cal_finish(&st, &cal));
}

/* Deterministic Fibonacci-sphere sample generator: N points spread
 * ~evenly over a sphere of the given radius centered at `center`. Used
 * by both the sphere-coverage test and its scale-invariance repeat. */
static ff_vec3_t s01_ac5_fib_sphere_sample(unsigned i, unsigned n, float radius, ff_vec3_t center)
{
    const double golden_angle = FF_TEST_PI * (3.0 - sqrt(5.0));
    double y_unit = 1.0 - (2.0 * (double)i + 1.0) / (double)n; /* (-1, 1) */
    double r_unit = sqrt(1.0 - y_unit * y_unit);
    double theta = golden_angle * (double)i;

    ff_vec3_t s;
    s.x = center.x + (float)(cos(theta) * r_unit * (double)radius);
    s.y = center.y + (float)(y_unit * (double)radius);
    s.z = center.z + (float)(sin(theta) * r_unit * (double)radius);
    return s;
}

/* Feed order for the Fibonacci-sphere set below: generation order sweeps
 * one pole to the other monotonically in Y (by construction of the
 * Fibonacci-sphere formula), which is a poor stand-in for a real
 * figure-eight — a real one reverses direction across all three axes
 * many times over. Fed in that raw order, the running-center classifier
 * in ff_geo_cal_feed (deliberately, correctly) never gets to see a
 * sample on the "already covered" side of an axis until the sweep
 * reverses, so it stalls partway (caught while writing this test: 50%,
 * not 100%, fed in raw order). Ping-pong the feed order (first, last,
 * second, second-last, ...) instead so both extremes of the sweep are
 * visited within the first few samples — still a deterministic, fully
 * reproducible order, just one that actually resembles a hand swinging
 * the puck back and forth rather than a single one-way ramp. */
static unsigned s01_ac5_pingpong_index(unsigned k, unsigned n)
{
    if (k % 2u == 0u) {
        return k / 2u;
    }
    return n - 1u - (k / 2u);
}

static void s01_ac5_run_sphere_coverage_case(float radius)
{
    const unsigned n = 400;
    const ff_vec3_t center = {30.0f, -40.0f, 10.0f};

    ff_geo_cal_state_t st;
    ff_geo_cal_begin(&st);
    for (unsigned k = 0; k < n; k++) {
        unsigned i = s01_ac5_pingpong_index(k, n);
        ff_geo_cal_feed(&st, s01_ac5_fib_sphere_sample(i, n, radius, center));
    }

    TEST_ASSERT_EQUAL_INT(100, ff_geo_cal_progress_pct(&st));

    ff_geo_cal_t cal;
    TEST_ASSERT_TRUE(ff_geo_cal_finish(&st, &cal));

    float tol = 0.02f * radius;
    if (tol < 5.0f) {
        tol = 5.0f;
    }
    TEST_ASSERT_FLOAT_WITHIN(tol, center.x, cal.hard_offset.x);
    TEST_ASSERT_FLOAT_WITHIN(tol, center.y, cal.hard_offset.y);
    TEST_ASSERT_FLOAT_WITHIN(tol, center.z, cal.hard_offset.z);

    TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.0f, cal.soft_scale[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.0f, cal.soft_scale[1]);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.0f, cal.soft_scale[2]);
}

static void S01_AC5_sphere_400_samples_full_coverage_and_good_fit(void)
{
    s01_ac5_run_sphere_coverage_case(250.0f);
}

static void S01_AC5_sphere_scale_invariance_at_radius_7500(void)
{
    /* Same shape, same relative thresholds, 30x the field magnitude
     * (this sensor reads Earth's field at ~245 LSB; another part on
     * another board could read ~7500 — the ratios must not care). */
    s01_ac5_run_sphere_coverage_case(7500.0f);
}

/* ------------------------------------------------------------------- */
/* AC6 — angdiff                                                        */
/* ------------------------------------------------------------------- */

static void S01_AC6_angdiff_table(void)
{
    struct {
        float a, b, expected;
    } cases[] = {
        {350.0f, 10.0f, 20.0f},
        {10.0f, 350.0f, -20.0f},
        {0.0f, 0.0f, 0.0f},
        {0.0f, 180.0f, 180.0f},
        {180.0f, 0.0f, -180.0f},
        {720.0f + 10.0f, 5.0f, -5.0f},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        float got = ff_geo_angdiff_deg(cases[i].a, cases[i].b);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, cases[i].expected, got);
        TEST_ASSERT_TRUE(got >= -180.0f && got <= 180.0f);
    }
}

/* ------------------------------------------------------------------- */
/* AC7 — local flat projection                                          */
/* ------------------------------------------------------------------- */

static void S01_AC7_projection_matches_equirect_reference(void)
{
    ff_latlon_t origin = {39.9012, -82.4562};
    ff_latlon_t p = {39.91593362263924, -82.44275208167251}; /* ~2km away */

    float east_m = 0.0f, north_m = 0.0f;
    ff_geo_project(origin, p, &east_m, &north_m);

    TEST_ASSERT_FLOAT_WITHIN(0.5f, 1147.1528727016373f, east_m);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 1638.304088578241f, north_m);
}

static void S01_AC7_projection_round_trips_within_1m_at_2km(void)
{
    ff_latlon_t origin = {39.9012, -82.4562};
    ff_latlon_t p = {39.91593362263924, -82.44275208167251}; /* ~2km from origin */

    float east_m = 0.0f, north_m = 0.0f;
    ff_geo_project(origin, p, &east_m, &north_m);

    /* Invert with the algebraic inverse of the same equirectangular model,
     * computed independently here (ff_geo.h exposes no unproject — this
     * mirrors how a map-face consumer would recover lat/lon from meters). */
    double lat0_rad = origin.lat * (FF_TEST_PI / 180.0);
    double lat2 = origin.lat + (double)north_m / FF_TEST_EARTH_RADIUS_M * (180.0 / FF_TEST_PI);
    double lon2 = origin.lon + (double)east_m / (FF_TEST_EARTH_RADIUS_M * cos(lat0_rad)) * (180.0 / FF_TEST_PI);

    ff_latlon_t recovered = {lat2, lon2};
    float err_m = ff_geo_distance_m(p, recovered);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 0.0f, err_m);
}

/* S24 slice d — ff_geo_unproject is the exact inverse of ff_geo_project:
 * project to meters then unproject recovers lat/lon within a meter at
 * festival scale (the rally landmark position round-trip). */
static void S24_AC6_unproject_inverts_project(void)
{
    ff_latlon_t origin = {39.9012, -82.4562};
    ff_latlon_t p = {39.91593362263924, -82.44275208167251}; /* ~2km from origin */

    float east_m = 0.0f, north_m = 0.0f;
    ff_geo_project(origin, p, &east_m, &north_m);

    ff_latlon_t recovered = {0.0, 0.0};
    ff_geo_unproject(origin, east_m, north_m, &recovered);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 0.0f, ff_geo_distance_m(p, recovered));

    /* Origin unprojects to itself; a NULL out is a safe no-op. */
    ff_latlon_t at_origin = {1.0, 1.0};
    ff_geo_unproject(origin, 0.0f, 0.0f, &at_origin);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, origin.lat, at_origin.lat);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, origin.lon, at_origin.lon);
    ff_geo_unproject(origin, 10.0f, 10.0f, NULL); /* must not crash */
}

/* ------------------------------------------------------------------- */
/* S06 2026-09-05 amendment — ff_geo_compass_point (RADAR_NOHDG).       */
/* ------------------------------------------------------------------- */

static void S06_compass_point_cardinal_and_intercardinal_centers(void)
{
    char out[4];
    ff_geo_compass_point(0.0f, out);
    TEST_ASSERT_EQUAL_STRING("N", out);
    ff_geo_compass_point(45.0f, out);
    TEST_ASSERT_EQUAL_STRING("NE", out);
    ff_geo_compass_point(90.0f, out);
    TEST_ASSERT_EQUAL_STRING("E", out);
    ff_geo_compass_point(135.0f, out);
    TEST_ASSERT_EQUAL_STRING("SE", out);
    ff_geo_compass_point(180.0f, out);
    TEST_ASSERT_EQUAL_STRING("S", out); /* pins the S06 spec's own bearing-180 example */
    ff_geo_compass_point(225.0f, out);
    TEST_ASSERT_EQUAL_STRING("SW", out);
    ff_geo_compass_point(270.0f, out);
    TEST_ASSERT_EQUAL_STRING("W", out);
    ff_geo_compass_point(315.0f, out);
    TEST_ASSERT_EQUAL_STRING("NW", out);
}

static void S06_compass_point_all_16_sector_centers(void)
{
    char const *const names[16] = {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW",
    };
    char out[4];
    for (int i = 0; i < 16; i++) {
        ff_geo_compass_point((float)i * 22.5f, out);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(names[i], out, names[i]);
    }
}

/* Boundary tie-break: the doc comment pins "exactly on a boundary
 * resolves to the CLOCKWISE-most (higher-bearing) sector" — 11.25 deg is
 * the N/NNE seam and must read NNE, not N. */
static void S06_compass_point_boundary_ties_break_clockwise(void)
{
    char out[4];
    ff_geo_compass_point(11.25f, out);
    TEST_ASSERT_EQUAL_STRING("NNE", out);
    ff_geo_compass_point(33.75f, out); /* NNE/NE seam -> NE */
    TEST_ASSERT_EQUAL_STRING("NE", out);
    ff_geo_compass_point(348.75f, out); /* NNW/N seam, wraps to idx 16 -> 0 -> N */
    TEST_ASSERT_EQUAL_STRING("N", out);
}

/* A full tenth-of-a-degree sweep of every one of the 16 sectors' full
 * span, checked against the boundary formula directly (not just spot
 * checks) — AGENTS.md's "measuring, not reasoning harder" for a function
 * whose whole job is getting every one of 3600 tenth-degree readings
 * right, not just the ones a human thought to spot-check. */
static void S06_compass_point_full_tenth_degree_sweep(void)
{
    char const *const names[16] = {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW",
    };
    char out[4];
    for (int tenth = 0; tenth < 3600; tenth++) {
        float deg = (float)tenth / 10.0f;
        int expect_idx = ((int)((deg + 11.25f) / 22.5f)) % 16;
        ff_geo_compass_point(deg, out);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(names[expect_idx], out, "compass sweep mismatch");
    }
}

static void S06_compass_point_wraps_negative_and_over_360(void)
{
    char out[4];
    ff_geo_compass_point(-90.0f, out); /* == 270 -> W */
    TEST_ASSERT_EQUAL_STRING("W", out);
    ff_geo_compass_point(720.0f + 45.0f, out); /* two full wraps + 45 -> NE */
    TEST_ASSERT_EQUAL_STRING("NE", out);
}

static void S06_compass_point_null_out_is_a_safe_noop(void)
{
    ff_geo_compass_point(90.0f, NULL); /* must not crash */
}

/* ------------------------------------------------------------------- */
/* AC8 — math.h only, warnings-clean C11 build (enforced by the CMake   */
/* gate; this is a light end-to-end smoke check).                       */
/* ------------------------------------------------------------------- */

static void S01_AC8_module_is_usable_end_to_end(void)
{
    ff_latlon_t a = {0.0, 0.0};
    ff_latlon_t b = {0.0, 1.0};
    float d = ff_geo_distance_m(a, b);
    float br = ff_geo_bearing_deg(a, b);
    TEST_ASSERT_TRUE(d > 0.0f);
    TEST_ASSERT_TRUE(br >= 0.0f && br < 360.0f);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S01_AC1_distance_equator_one_degree_lon_is_analytic);
    RUN_TEST(S01_AC1_distance_one_degree_lat_is_analytic);
    RUN_TEST(S01_AC1_distance_identical_points_is_zero);
    RUN_TEST(S01_AC1_distance_known_pair_within_half_percent);

    RUN_TEST(S01_AC2_bearing_cardinal_directions);
    RUN_TEST(S01_AC2_bearing_wraps_across_antimeridian);
    RUN_TEST(S01_AC2_bearing_identical_points_returns_zero_no_nan);
    RUN_TEST(S01_AC2_bearing_near_identical_points_finite_in_range);

    RUN_TEST(S01_AC3_arrow_deg_matches_wrap_of_bearing_minus_heading);

    RUN_TEST(S01_AC4_heading_within_2deg_at_0_20_40_tilt);
    RUN_TEST(S01_AC4_heading_just_under_60deg_tilt_is_still_reliable);
    RUN_TEST(S01_AC4_heading_at_exactly_60deg_tilt_is_reliable);
    RUN_TEST(S01_AC4_heading_beyond_60deg_tilt_returns_negative);
    RUN_TEST(S01_AC4_sentinel_zero_accel);
    RUN_TEST(S01_AC4_sentinel_zero_mag);
    RUN_TEST(S01_AC4_sentinel_mag_parallel_gravity);

    RUN_TEST(S01_AC5_calibration_recovers_hard_offset_and_improves_heading);
    RUN_TEST(S01_AC5_calibration_below_70pct_coverage_finish_fails);
    RUN_TEST(S01_AC5_stationary_device_never_reaches_finishable_coverage);
    RUN_TEST(S01_AC5_needle_along_one_axis_finish_fails);
    RUN_TEST(S01_AC5_sphere_400_samples_full_coverage_and_good_fit);
    RUN_TEST(S01_AC5_sphere_scale_invariance_at_radius_7500);

    RUN_TEST(S01_AC6_angdiff_table);

    RUN_TEST(S01_AC7_projection_matches_equirect_reference);
    RUN_TEST(S01_AC7_projection_round_trips_within_1m_at_2km);
    RUN_TEST(S24_AC6_unproject_inverts_project);

    RUN_TEST(S01_AC8_module_is_usable_end_to_end);

    RUN_TEST(S06_compass_point_cardinal_and_intercardinal_centers);
    RUN_TEST(S06_compass_point_all_16_sector_centers);
    RUN_TEST(S06_compass_point_boundary_ties_break_clockwise);
    RUN_TEST(S06_compass_point_full_tenth_degree_sweep);
    RUN_TEST(S06_compass_point_wraps_negative_and_over_360);
    RUN_TEST(S06_compass_point_null_out_is_a_safe_noop);

    return UNITY_END();
}
