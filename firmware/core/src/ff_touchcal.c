/**
 * ff_touchcal.c — per-axis affine touch calibration. See ff_touchcal.h.
 *
 * Pure C11: the only header beyond the module's own is <math.h> (used for
 * fabs/lroundf). No I/O, no globals — the caller owns all storage.
 */
#include "ff_touchcal.h"

#include <math.h>
#include <stddef.h>

/* Below this the normal-equation denominator (n*Σx² − (Σx)²) is treated as
 * "no spread". With integer pixel coords that determinant is 0 exactly when
 * every raw value is equal and otherwise a positive integer >= 1, so any
 * small positive epsilon separates the degenerate case cleanly; a hair
 * above zero keeps it from ever accepting an all-same-value capture. */
#define FF_TOUCHCAL_MIN_DENOM 1e-6

void ff_touchcal_identity(ff_touchcal_t *out)
{
    if (out == NULL) {
        return;
    }
    out->ax = 1.0f;
    out->bx = 0.0f;
    out->ay = 1.0f;
    out->by = 0.0f;
    out->valid = false;
}

/* Least-squares slope/intercept of screen = a*raw + b over n samples.
 * Returns false (leaving *a,*b untouched) if raw has no spread. */
static bool ff_fit_axis(const int *raw, const int *screen, int n, float *a, float *b)
{
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    for (int i = 0; i < n; i++) {
        double x = (double)raw[i];
        double y = (double)screen[i];
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    double dn = (double)n;
    double denom = dn * sxx - sx * sx; /* = n*Σx² − (Σx)²; 0 iff no x-spread */
    if (fabs(denom) < FF_TOUCHCAL_MIN_DENOM) {
        return false;
    }
    double slope = (dn * sxy - sx * sy) / denom;
    double intercept = (sy - slope * sx) / dn;
    *a = (float)slope;
    *b = (float)intercept;
    return true;
}

bool ff_touchcal_solve(const ff_cal_point_t *pts, int n, ff_touchcal_t *out)
{
    if (out == NULL) {
        return false;
    }
    ff_touchcal_identity(out);

    if (pts == NULL || n < 2) {
        return false;
    }

    /* Gather the per-axis columns onto small fixed buffers so the fit is a
     * plain 1-D least squares. Cap n defensively; a calibration capture is
     * a handful of points (5 in the spec's flow). */
    enum { FF_TOUCHCAL_MAX_PTS = 32 };
    if (n > FF_TOUCHCAL_MAX_PTS) {
        n = FF_TOUCHCAL_MAX_PTS;
    }
    int rx[FF_TOUCHCAL_MAX_PTS], sx[FF_TOUCHCAL_MAX_PTS];
    int ry[FF_TOUCHCAL_MAX_PTS], sy[FF_TOUCHCAL_MAX_PTS];
    for (int i = 0; i < n; i++) {
        rx[i] = pts[i].raw_x;
        sx[i] = pts[i].screen_x;
        ry[i] = pts[i].raw_y;
        sy[i] = pts[i].screen_y;
    }

    float ax, bx, ay, by;
    /* Both axes must be well-conditioned. A half-valid transform (good x,
     * flat y) would still mis-map every touch, so treat it as degenerate
     * and keep identity. */
    if (!ff_fit_axis(rx, sx, n, &ax, &bx)) {
        return false;
    }
    if (!ff_fit_axis(ry, sy, n, &ay, &by)) {
        return false;
    }

    out->ax = ax;
    out->bx = bx;
    out->ay = ay;
    out->by = by;
    out->valid = true;
    return true;
}

static int ff_clamp_coord(long v)
{
    if (v < FF_TOUCHCAL_COORD_MIN) {
        return FF_TOUCHCAL_COORD_MIN;
    }
    if (v > FF_TOUCHCAL_COORD_MAX) {
        return FF_TOUCHCAL_COORD_MAX;
    }
    return (int)v;
}

void ff_touchcal_apply(const ff_touchcal_t *c, int rawx, int rawy, int *sx, int *sy)
{
    if (sx == NULL || sy == NULL) {
        return;
    }
    if (c == NULL || !c->valid) {
        /* Identity: pass raw through, still clamped to the panel so an
         * off-panel raw value can't escape the addressable range. */
        *sx = ff_clamp_coord((long)rawx);
        *sy = ff_clamp_coord((long)rawy);
        return;
    }
    long ox = lroundf(c->ax * (float)rawx + c->bx);
    long oy = lroundf(c->ay * (float)rawy + c->by);
    *sx = ff_clamp_coord(ox);
    *sy = ff_clamp_coord(oy);
}

void ff_touchcal_flip180(int x, int y, int w, int h, int *out_x, int *out_y)
{
    if (out_x == NULL || out_y == NULL) {
        return;
    }
    /* Snapshot before writing, so out_x/out_y aliasing x/y's own storage
     * (a caller doing `ff_touchcal_flip180(x, y, w, h, &x, &y)`) is safe. */
    int const in_x = x;
    int const in_y = y;
    *out_x = w - 1 - in_x;
    *out_y = h - 1 - in_y;
}
