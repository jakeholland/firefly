/**
 * ff_layout.c — see ff_layout.h.
 */
#include "ff_layout.h"

#include <math.h>

bool ff_layout_rect_in_circle(ff_layout_rect_t rect, float cx, float cy, float radius)
{
    if (radius < 0.0f) {
        return false;
    }

    float const corners_x[4] = {rect.x1, rect.x2, rect.x1, rect.x2};
    float const corners_y[4] = {rect.y1, rect.y1, rect.y2, rect.y2};
    float const r2 = radius * radius;

    for (int i = 0; i < 4; i++) {
        float dx = corners_x[i] - cx;
        float dy = corners_y[i] - cy;
        if ((dx * dx + dy * dy) > r2) {
            return false;
        }
    }
    return true;
}

float ff_layout_chord_half_width(float dy, float radius)
{
    if (radius < 0.0f) {
        return 0.0f;
    }
    float ady = fabsf(dy);
    if (ady >= radius) {
        return 0.0f;
    }
    return sqrtf(radius * radius - ady * ady);
}

float ff_layout_bezel_margin_x(float top_y, float h, float band_cx, float cx, float cy, float radius,
                               float safety_px)
{
    float dy_top = top_y - cy;
    float dy_bottom = (top_y + h) - cy;
    float far_dy = (fabsf(dy_top) > fabsf(dy_bottom)) ? dy_top : dy_bottom;

    /* The band is symmetric about `band_cx`; the circle is centred at
     * `cx`. Whichever side of the band is FARTHER from `cx` binds, and
     * it is farther by exactly |cx - band_cx| — so charge that to the
     * usable half-width once, alongside the safety slack. */
    float half_w = ff_layout_chord_half_width(far_dy, radius) - safety_px - fabsf(cx - band_cx);
    if (half_w < 0.0f) {
        half_w = 0.0f;
    }
    float margin = band_cx - half_w;
    if (margin < 0.0f) {
        margin = 0.0f;
    }
    return margin;
}

float ff_layout_safe_margin_x(float top_y, float h, float center, float radius, float safety_px)
{
    return ff_layout_bezel_margin_x(top_y, h, center, center, center, radius, safety_px);
}

float ff_layout_centered_band_max_width(float cy, float h, float radius, float safety_px)
{
    /* Whichever edge of the band is farther from center-y is the one
     * that binds — see this function's doc comment for why `cy` alone
     * is the wrong input. `h` is treated as a magnitude; a negative
     * height is the same band as its absolute value. */
    float half_h = fabsf(h) / 2.0f;
    float top = cy - half_h;
    float bottom = cy + half_h;
    float far_dy = (fabsf(top) > fabsf(bottom)) ? top : bottom;

    float half_w = ff_layout_chord_half_width(far_dy, radius) - safety_px;
    if (half_w < 0.0f) {
        return 0.0f;
    }
    return half_w * 2.0f;
}
