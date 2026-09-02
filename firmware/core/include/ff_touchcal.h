/**
 * ff_touchcal.h — touch-screen calibration transform (spec S15 slice d).
 *
 * Pure C11, zero deps, no I/O, no LVGL. The measured SPD2010 error on
 * board 2 is a per-axis offset + slight scale (no swap, no mirror, no
 * rotation/skew — see docs/specs/S15d-touch-calibration.md), so the
 * correction is a per-axis affine:
 *
 *     screen_x = ax * raw_x + bx
 *     screen_y = ay * raw_y + by
 *
 * ff_touchcal_solve() fits (ax,bx) and (ay,by) independently by least
 * squares from a handful of (raw -> screen) capture points; ff_touchcal_
 * apply() runs the transform on a live touch and clamps to the panel.
 *
 * The four params live in ff_settings (ff_settings.h) so calibration rides
 * the existing settings persistence seam. This module is LVGL-free and
 * knows nothing about how points are captured or applied — that wiring is
 * the device target's (targets/esp32s3), keeping the boundary clean.
 */
#ifndef FF_TOUCHCAL_H
#define FF_TOUCHCAL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Panel is the 412x412 round glass; screen coords are clamped to
 * [0, res-1] on apply. A raw or fitted value that lands outside the panel
 * (a tap near the bezel, or fit noise) must not escape the addressable
 * range and index off-panel downstream. */
#define FF_TOUCHCAL_RES 412
#define FF_TOUCHCAL_COORD_MIN 0
#define FF_TOUCHCAL_COORD_MAX (FF_TOUCHCAL_RES - 1) /* 411 */

/**
 * ff_touchcal_t — the per-axis affine (4 params) plus a validity flag.
 * When `valid` is false the transform is identity: apply passes raw
 * through (clamped), so an uncalibrated or degenerate device is corrected
 * to exactly nothing rather than to garbage.
 */
typedef struct {
    float ax; /* x scale */
    float bx; /* x offset */
    float ay; /* y scale */
    float by; /* y offset */
    bool valid;
} ff_touchcal_t;

/**
 * ff_cal_point_t — one capture pair: where the controller reported the
 * tap (raw) vs. where the on-screen crosshair actually was (screen).
 */
typedef struct {
    int raw_x;
    int raw_y;
    int screen_x;
    int screen_y;
} ff_cal_point_t;

/**
 * ff_touchcal_identity — set `out` to the identity transform (valid =
 * false, params ax=ay=1, bx=by=0). Apply on this is a pass-through.
 */
void ff_touchcal_identity(ff_touchcal_t *out);

/**
 * ff_touchcal_solve — least-squares fit of the per-axis affine from `n`
 * capture points.
 *
 * Fits (ax,bx) from the (raw_x -> screen_x) pairs and (ay,by) from the
 * (raw_y -> screen_y) pairs, independently. On success writes the four
 * params, sets `out->valid = true`, and returns true.
 *
 * Returns false and leaves `out` at identity (see ff_touchcal_identity)
 * when the input cannot yield a well-conditioned fit:
 *   - n < 2 (a line needs at least two points), or
 *   - no spread in raw_x (every raw_x equal), or
 *   - no spread in raw_y (every raw_y equal).
 * A degenerate capture therefore can NEVER produce a garbage transform —
 * it produces identity, which is honest ("not calibrated").
 *
 * Per-axis: the x and y fits are independent, so a capture with x-spread
 * but no y-spread is still degenerate overall (both axes must be
 * well-conditioned) and returns identity, not a half-valid transform.
 */
bool ff_touchcal_solve(const ff_cal_point_t *pts, int n, ff_touchcal_t *out);

/**
 * ff_touchcal_apply — apply the transform to a raw touch and clamp the
 * result to [FF_TOUCHCAL_COORD_MIN, FF_TOUCHCAL_COORD_MAX].
 *
 * When `c` is NULL or `c->valid` is false this is identity: the raw
 * coords are clamped and returned unchanged. `sx`/`sy` must be non-NULL.
 */
void ff_touchcal_apply(const ff_touchcal_t *c, int rawx, int rawy, int *sx, int *sy);

/**
 * ff_touchcal_flip180 — rotate a screen-space point 180 degrees within a
 * `w`x`h` panel: `*out_x = w - 1 - x`, `*out_y = h - 1 - y`.
 *
 * Pure geometry, no clamping (the input is already screen-space, already
 * in [0, w-1]x[0, h-1] — same contract `ff_touchcal_apply`'s output
 * carries — so the reflection can't escape that range either).
 *
 * Device use (targets/esp32s3/ff_display's `process_coordinates` seam):
 * called AFTER `ff_touchcal_apply`, not instead of it, when
 * `ff_settings_t.screen_flip` is set — the panel's per-unit calibration
 * fit (`ff_touchcal_t`) is measured against raw controller coordinates,
 * which don't change when the CASE is mounted flipped (the touch
 * controller doesn't know or care how the case is oriented); only the
 * mapping from "corrected screen point" to "the glass position the user
 * actually sees at that point" flips. Doing the 180 rotation as a
 * separate, LAST step means a previously-solved calibration stays valid
 * in either orientation with no re-calibration on flip.
 *
 * `out_x`/`out_y` may alias `x`/`y`'s own storage (each output is
 * computed from a snapshotted parameter, not read back mid-computation).
 */
void ff_touchcal_flip180(int x, int y, int w, int h, int *out_x, int *out_y);

#ifdef __cplusplus
}
#endif

#endif /* FF_TOUCHCAL_H */
