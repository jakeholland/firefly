# S01 · core/geo — bearing, distance, heading

## Purpose
All spatial math for the arrow: where is my friend relative to where I'm pointing. Pure functions, `float` (device has FPU), no allocation.

## Interface (`core/include/ff_geo.h`)
```c
typedef struct { double lat, lon; } ff_latlon_t;            // degrees, WGS84
typedef struct { float x, y, z; } ff_vec3_t;                // sensor axes, board frame

float ff_geo_distance_m(ff_latlon_t a, ff_latlon_t b);      // haversine, meters
float ff_geo_bearing_deg(ff_latlon_t from, ff_latlon_t to); // initial bearing, 0=N, CW, [0,360)
// Tilt-compensated compass heading from raw sensors. Returns heading deg [0,360)
// or <0 if |tilt| > 60° (unreliable — caller shows "hold flatter").
float ff_geo_heading_deg(ff_vec3_t mag, ff_vec3_t accel, ff_geo_cal_t const *cal);
float ff_geo_arrow_deg(float bearing_deg, float heading_deg); // screen rotation for arrow, [0,360)
float ff_geo_wrap_deg(float deg);                            // normalize to [0,360)
float ff_geo_angdiff_deg(float a, float b);                  // shortest signed diff [-180,180]

typedef struct { ff_vec3_t hard_offset; float soft_scale[3]; float declination_deg; } ff_geo_cal_t;
// Iterative hard/soft-iron calibration: feed samples while user does figure-eight.
void ff_geo_cal_begin(ff_geo_cal_state_t *st);
void ff_geo_cal_feed(ff_geo_cal_state_t *st, ff_vec3_t mag);
int  ff_geo_cal_progress_pct(ff_geo_cal_state_t const *st);  // 0..100 (coverage of sphere octants)
bool ff_geo_cal_finish(ff_geo_cal_state_t const *st, ff_geo_cal_t *out); // false if coverage < 70%
// Local flat projection for the map (valid at festival scale, <10 km):
void ff_geo_project(ff_latlon_t origin, ff_latlon_t p, float *east_m, float *north_m);
```

## Behavior
- Distance/bearing: standard haversine / atan2 formulas. Declination is added inside `ff_geo_heading_deg` from `cal->declination_deg` (packs may carry it later; default 0, settable via S11).
- Heading fusion v1: tilt compensation via accel (pitch/roll from gravity), no gyro filtering yet. Output smoothed by caller (S06 owns smoothing — geo stays pure).
- Calibration: min/max per axis → hard-iron offset; per-axis scale to sphere → soft-iron approx. Progress = fraction of 3D direction octants seen.

## Acceptance criteria
1. Distance: known pairs within 0.5% (fixture: Legend Valley bowl→Wompy ≈ published test vector; equator/meridian analytic cases exact).
2. Bearing: due N/E/S/W cases return 0/90/180/270 ±0.1°; wrap correct across the antimeridian and near-identical points (returns 0, no NaN).
3. `arrow_deg(bearing,heading)` == wrap(bearing − heading); verified for 8 combinations including wraparound.
4. Heading: synthetic mag+accel fixtures at 0/90/180/270 yaw with 0°, 20°, 40° tilt → within 2°; >60° tilt returns <0.
5. Calibration: synthetic offset+scaled sphere samples recover hard offset within 5% and yield post-cal heading error <3°; <70% coverage → `finish` false.
6. `angdiff(350,10) == +20` and `angdiff(10,350) == −20`.
7. Projection: round-trips points within 1 m at 2 km from origin at Legend Valley latitude.
8. No libm beyond `math.h`; builds clean with `-Wall -Wextra -Werror` as C11.

## Slices
a) distance/bearing/wrap/angdiff · b) heading + tilt comp · c) calibration · d) projection.
