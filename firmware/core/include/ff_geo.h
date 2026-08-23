/**
 * ff_geo.h — core/geo: bearing, distance, tilt-compensated heading, compass
 * calibration, local flat projection.
 *
 * Spec: docs/specs/S01-core-geo.md
 *
 * Pure C11, `math.h` only, no allocation, no I/O. All angles are degrees
 * unless noted; latitude/longitude are WGS84 degrees.
 *
 * ## Deviations from the spec's interface sketch (see PR for detail)
 *  - `ff_geo_cal_state_t` is defined here as a concrete struct. The spec
 *    references the type (`ff_geo_cal_begin(ff_geo_cal_state_t *st)` etc.)
 *    but never defines its fields. C requires a complete type for callers
 *    to allocate it on the stack (no allocation elsewhere in this module),
 *    so a definition had to be added; it is intentionally a plain,
 *    inspectable struct (running min/max + an 8-bit octant-coverage mask)
 *    rather than an opaque handle, matching this module's no-hidden-state
 *    style.
 */
#ifndef FF_GEO_H
#define FF_GEO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A WGS84 geodetic coordinate, degrees. */
typedef struct {
    double lat;
    double lon;
} ff_latlon_t;

/** A 3-axis sensor sample in board frame. */
typedef struct {
    float x;
    float y;
    float z;
} ff_vec3_t;

/**
 * ff_geo_distance_m — great-circle distance between two points.
 *
 * Haversine formula over a spherical-earth approximation (mean radius
 * 6,371,000 m). Accurate to within 0.5% for terrestrial distances.
 *
 * Returns meters, always >= 0. Identical points return exactly 0.
 */
float ff_geo_distance_m(ff_latlon_t a, ff_latlon_t b);

/**
 * ff_geo_bearing_deg — initial great-circle bearing from `from` to `to`.
 *
 * Returns degrees, 0 = true north, clockwise, range [0, 360). Correct
 * across the antimeridian. Identical (or numerically indistinguishable)
 * points return 0, never NaN.
 */
float ff_geo_bearing_deg(ff_latlon_t from, ff_latlon_t to);

/** ff_geo_wrap_deg — normalize an angle to [0, 360). */
float ff_geo_wrap_deg(float deg);

/**
 * ff_geo_angdiff_deg — shortest signed angular difference from `a` to `b`,
 * i.e. how far (and which way) to rotate `a` to reach `b`.
 *
 * Range [-180, 180]. Positive = clockwise from `a` to `b`.
 */
float ff_geo_angdiff_deg(float a, float b);

/**
 * ff_geo_arrow_deg — screen rotation to draw the friend arrow, given the
 * true bearing to the friend and the device's own compass heading.
 *
 * Equivalent to `ff_geo_wrap_deg(bearing_deg - heading_deg)`.
 * Returns degrees, range [0, 360).
 */
float ff_geo_arrow_deg(float bearing_deg, float heading_deg);

/**
 * ff_geo_cal_t — compass calibration (hard/soft-iron correction) plus
 * magnetic declination. Persisted by S11; produced by `ff_geo_cal_finish`.
 */
typedef struct {
    ff_vec3_t hard_offset;    /* per-axis bias to subtract, sensor units */
    float soft_scale[3];      /* per-axis scale to apply after bias removal */
    float declination_deg;    /* added to computed heading; default 0 */
} ff_geo_cal_t;

/**
 * ff_geo_heading_deg — tilt-compensated compass heading.
 *
 * `mag` and `accel` are raw sensor samples in board frame (board convention:
 * +x = right, +y = forward/"top of puck", +z = up out of the screen;
 * stationary level accel reads ~(0,0,+1g)). `cal` (may be NULL) is applied
 * to `mag` as `(mag - hard_offset) * soft_scale`, and `cal->declination_deg`
 * is added to the result; a NULL `cal` is treated as identity (no offset,
 * unit scale, zero declination).
 *
 * Returns heading in degrees [0, 360), 0 = north, clockwise — OR a negative
 * value if any of the following make the result unreliable:
 *  - the device is tilted MORE THAN 60 degrees from level. The boundary is
 *    exclusive: a tilt of exactly 60 degrees is still considered reliable
 *    and returns a heading (guard condition is `tilt_deg > 60.0f`, per the
 *    spec's ">60°" wording) — caller should show "hold flatter" only once
 *    this function actually returns negative.
 *  - `accel` is a zero (or near-zero) vector, so tilt can't be determined.
 *  - the (calibrated) `mag` reading is zero, or parallel to gravity (no
 *    usable horizontal component), so heading can't be resolved.
 */
float ff_geo_heading_deg(ff_vec3_t mag, ff_vec3_t accel, ff_geo_cal_t const *cal);

/**
 * ff_geo_cal_state_t — running state for the figure-eight calibration
 * ritual. Zero-initialize via `ff_geo_cal_begin`; no allocation, safe on
 * the stack or in a static.
 */
typedef struct {
    ff_vec3_t min;             /* running per-axis minimum raw mag sample */
    ff_vec3_t max;             /* running per-axis maximum raw mag sample */
    unsigned char octant_mask; /* bit i set => 3D direction octant i seen */
    unsigned int sample_count;
} ff_geo_cal_state_t;

/** ff_geo_cal_begin — reset calibration state before a new ritual. */
void ff_geo_cal_begin(ff_geo_cal_state_t *st);

/**
 * ff_geo_cal_feed — feed one raw magnetometer sample during the figure-eight
 * motion. Updates the running per-axis min/max and marks the 3D-direction
 * octant (relative to the current center estimate) as covered.
 */
void ff_geo_cal_feed(ff_geo_cal_state_t *st, ff_vec3_t mag);

/**
 * ff_geo_cal_progress_pct — fraction of the 8 direction octants seen so
 * far, as a percentage 0..100. Drives the on-device progress ring (S12).
 */
int ff_geo_cal_progress_pct(ff_geo_cal_state_t const *st);

/**
 * ff_geo_cal_finish — compute hard/soft-iron calibration from the samples
 * fed so far.
 *
 * Hard-iron offset = per-axis (min+max)/2. Soft-iron scale = per-axis
 * (average axis radius) / (that axis's radius), so the corrected samples
 * lie on a sphere. `declination_deg` is left 0 (declination is not
 * derivable from magnetometer samples alone; set separately, see S11).
 *
 * Returns false (leaving `*out` unmodified) if octant coverage is below
 * 70% — not enough of the sphere was sampled for a trustworthy fit.
 */
bool ff_geo_cal_finish(ff_geo_cal_state_t const *st, ff_geo_cal_t *out);

/**
 * ff_geo_project — local flat (equirectangular) projection of `p` relative
 * to `origin`, in meters east/north. Valid at festival scale (<10 km);
 * do not use for long-range navigation.
 */
void ff_geo_project(ff_latlon_t origin, ff_latlon_t p, float *east_m, float *north_m);

#ifdef __cplusplus
}
#endif

#endif /* FF_GEO_H */
