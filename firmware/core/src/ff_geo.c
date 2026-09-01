#include "ff_geo.h"

#include <math.h>
#include <stddef.h> /* NULL */

/* Mean earth radius, meters. Spherical-earth approximation (haversine),
 * good to within 0.5% for terrestrial distances — see spec AC1. */
#define FF_GEO_EARTH_RADIUS_M 6371000.0

/* Avoid relying on the POSIX-only M_PI (undefined under strict -std=c11
 * on some libc's, per AC8: math.h only). */
#define FF_GEO_PI 3.14159265358979323846

#define FF_GEO_DEG2RAD(d) ((d) * (FF_GEO_PI / 180.0))
#define FF_GEO_RAD2DEG(r) ((r) * (180.0 / FF_GEO_PI))

/* Tilt beyond which a compass heading is considered unreliable. */
#define FF_GEO_MAX_RELIABLE_TILT_DEG 60.0f

float ff_geo_wrap_deg(float deg)
{
    float r = fmodf(deg, 360.0f);
    if (r < 0.0f) {
        r += 360.0f;
    }
    /* Normalize -0.0f to +0.0f (e.g. wrap_deg(-360.0f)): both compare equal
     * to 0.0f everywhere this is used, but +0.0f is the friendlier value to
     * print/log. */
    return r + 0.0f;
}

float ff_geo_angdiff_deg(float a, float b)
{
    float d = fmodf(b - a, 360.0f);
    if (d < -180.0f) {
        d += 360.0f;
    } else if (d > 180.0f) {
        d -= 360.0f;
    }
    return d;
}

float ff_geo_arrow_deg(float bearing_deg, float heading_deg)
{
    return ff_geo_wrap_deg(bearing_deg - heading_deg);
}

float ff_geo_distance_m(ff_latlon_t a, ff_latlon_t b)
{
    double lat1 = FF_GEO_DEG2RAD(a.lat);
    double lat2 = FF_GEO_DEG2RAD(b.lat);
    double dlat = FF_GEO_DEG2RAD(b.lat - a.lat);
    double dlon = FF_GEO_DEG2RAD(b.lon - a.lon);

    double sin_dlat2 = sin(dlat * 0.5);
    double sin_dlon2 = sin(dlon * 0.5);
    double h = sin_dlat2 * sin_dlat2 + cos(lat1) * cos(lat2) * sin_dlon2 * sin_dlon2;
    /* Clamp for fp safety: h can drift a hair outside [0,1] for
     * near-identical points, which would otherwise feed sqrt() a tiny
     * negative and produce NaN. */
    if (h < 0.0) {
        h = 0.0;
    } else if (h > 1.0) {
        h = 1.0;
    }
    double c = 2.0 * atan2(sqrt(h), sqrt(1.0 - h));
    return (float)(FF_GEO_EARTH_RADIUS_M * c);
}

float ff_geo_bearing_deg(ff_latlon_t from, ff_latlon_t to)
{
    double lat1 = FF_GEO_DEG2RAD(from.lat);
    double lat2 = FF_GEO_DEG2RAD(to.lat);
    double dlon = FF_GEO_DEG2RAD(to.lon - from.lon);

    double y = sin(dlon) * cos(lat2);
    double x = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dlon);
    double br = FF_GEO_RAD2DEG(atan2(y, x));
    return ff_geo_wrap_deg((float)br);
}

float ff_geo_heading_deg(ff_vec3_t mag, ff_vec3_t accel, ff_geo_cal_t const *cal)
{
    ff_vec3_t hard_offset = {0.0f, 0.0f, 0.0f};
    float soft_scale[3] = {1.0f, 1.0f, 1.0f};
    float declination_deg = 0.0f;

    if (cal) {
        hard_offset = cal->hard_offset;
        soft_scale[0] = cal->soft_scale[0];
        soft_scale[1] = cal->soft_scale[1];
        soft_scale[2] = cal->soft_scale[2];
        declination_deg = cal->declination_deg;
    }

    float mx = (mag.x - hard_offset.x) * soft_scale[0];
    float my = (mag.y - hard_offset.y) * soft_scale[1];
    float mz = (mag.z - hard_offset.z) * soft_scale[2];

    float amag = sqrtf(accel.x * accel.x + accel.y * accel.y + accel.z * accel.z);
    if (amag < 1e-6f) {
        return -1.0f; /* degenerate accel reading, can't determine tilt */
    }
    float anx = accel.x / amag;
    float any = accel.y / amag;
    float anz = accel.z / amag;

    float clamped_z = anz;
    if (clamped_z > 1.0f) {
        clamped_z = 1.0f;
    } else if (clamped_z < -1.0f) {
        clamped_z = -1.0f;
    }
    float tilt_deg = (float)FF_GEO_RAD2DEG(acosf(clamped_z));
    if (fabsf(tilt_deg) > FF_GEO_MAX_RELIABLE_TILT_DEG) {
        return -1.0f;
    }

    /* Build a horizontal (east, north) frame from the accel-derived "up"
     * vector and the (calibrated) magnetometer reading, then project the
     * device's own forward axis (body +y) onto it. This is an exact
     * tilt-compensation solution (not a small-angle approximation) for any
     * roll/pitch combination, as long as mag isn't parallel to gravity. */
    float downx = -anx, downy = -any, downz = -anz;

    float ex = downy * mz - downz * my;
    float ey = downz * mx - downx * mz;
    float ez = downx * my - downy * mx;
    float emag = sqrtf(ex * ex + ey * ey + ez * ez);
    if (emag < 1e-6f) {
        return -1.0f; /* mag reading parallel to gravity: heading unresolvable */
    }
    ex /= emag;
    ey /= emag;
    ez /= emag;
    (void)ez; /* east.z unused below, kept for clarity of the cross product */

    /* north = up x east, up = (anx, any, anz); only the y (forward) axis
     * component of east/north is needed since the device's forward axis is
     * body +y = (0,1,0). */
    float north_y = anz * ex - anx * ez;

    float heading = (float)FF_GEO_RAD2DEG(atan2f(ey, north_y));
    heading += declination_deg;
    return ff_geo_wrap_deg(heading);
}

void ff_geo_cal_begin(ff_geo_cal_state_t *st)
{
    if (!st) {
        return;
    }
    st->min.x = st->min.y = st->min.z = INFINITY;
    st->max.x = st->max.y = st->max.z = -INFINITY;
    st->octant_mask = 0;
    st->sample_count = 0;
}

void ff_geo_cal_feed(ff_geo_cal_state_t *st, ff_vec3_t mag)
{
    if (!st) {
        return;
    }

    if (mag.x < st->min.x) st->min.x = mag.x;
    if (mag.y < st->min.y) st->min.y = mag.y;
    if (mag.z < st->min.z) st->min.z = mag.z;
    if (mag.x > st->max.x) st->max.x = mag.x;
    if (mag.y > st->max.y) st->max.y = mag.y;
    if (mag.z > st->max.z) st->max.z = mag.z;
    st->sample_count++;

    /* Octant coverage relative to the running center estimate. This is
     * necessarily approximate early in the ritual (the center estimate is
     * still moving), but converges as more of the sphere is sampled — by
     * the end of a full figure-eight it correctly reflects the sample
     * directions relative to the final center. */
    float cx = (st->min.x + st->max.x) * 0.5f;
    float cy = (st->min.y + st->max.y) * 0.5f;
    float cz = (st->min.z + st->max.z) * 0.5f;

    unsigned char idx = 0;
    if (mag.x >= cx) idx |= 1u;
    if (mag.y >= cy) idx |= 2u;
    if (mag.z >= cz) idx |= 4u;
    st->octant_mask = (unsigned char)(st->octant_mask | (unsigned char)(1u << idx));
}

int ff_geo_cal_progress_pct(ff_geo_cal_state_t const *st)
{
    if (!st) {
        return 0;
    }
    int count = 0;
    unsigned char m = st->octant_mask;
    while (m) {
        count += (int)(m & 1u);
        m = (unsigned char)(m >> 1u);
    }
    return (count * 100) / 8;
}

bool ff_geo_cal_finish(ff_geo_cal_state_t const *st, ff_geo_cal_t *out)
{
    if (!st || !out) {
        return false;
    }
    if (ff_geo_cal_progress_pct(st) < 70) {
        return false;
    }

    float rx = (st->max.x - st->min.x) * 0.5f;
    float ry = (st->max.y - st->min.y) * 0.5f;
    float rz = (st->max.z - st->min.z) * 0.5f;

    const float eps = 1e-6f;
    if (rx < eps) rx = eps;
    if (ry < eps) ry = eps;
    if (rz < eps) rz = eps;
    float avg_radius = (rx + ry + rz) / 3.0f;

    out->hard_offset.x = (st->min.x + st->max.x) * 0.5f;
    out->hard_offset.y = (st->min.y + st->max.y) * 0.5f;
    out->hard_offset.z = (st->min.z + st->max.z) * 0.5f;
    out->soft_scale[0] = avg_radius / rx;
    out->soft_scale[1] = avg_radius / ry;
    out->soft_scale[2] = avg_radius / rz;
    out->declination_deg = 0.0f;
    return true;
}

void ff_geo_project(ff_latlon_t origin, ff_latlon_t p, float *east_m, float *north_m)
{
    double lat0_rad = FF_GEO_DEG2RAD(origin.lat);
    double dlat = FF_GEO_DEG2RAD(p.lat - origin.lat);
    double dlon = FF_GEO_DEG2RAD(p.lon - origin.lon);

    double north = dlat * FF_GEO_EARTH_RADIUS_M;
    double east = dlon * FF_GEO_EARTH_RADIUS_M * cos(lat0_rad);

    if (east_m) {
        *east_m = (float)east;
    }
    if (north_m) {
        *north_m = (float)north;
    }
}

void ff_geo_unproject(ff_latlon_t origin, float east_m, float north_m, ff_latlon_t *out)
{
    if (out == NULL) {
        return;
    }
    /* Exact inverse of ff_geo_project above: north = dlat_rad * R,
     * east = dlon_rad * R * cos(lat0). */
    double lat0_rad = FF_GEO_DEG2RAD(origin.lat);
    double dlat = (double)north_m / FF_GEO_EARTH_RADIUS_M;
    double coslat0 = cos(lat0_rad);
    double dlon = (coslat0 != 0.0) ? ((double)east_m / (FF_GEO_EARTH_RADIUS_M * coslat0)) : 0.0;

    out->lat = origin.lat + FF_GEO_RAD2DEG(dlat);
    out->lon = origin.lon + FF_GEO_RAD2DEG(dlon);
}
