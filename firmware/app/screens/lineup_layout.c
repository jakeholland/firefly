/**
 * lineup_layout.c — see lineup_layout.h. Pure C11, no LVGL.
 */
#include "lineup_layout.h"

#include <math.h>
#include <stdio.h>

void lineup_layout_format_countdown(int16_t mins_until, char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0) {
        return;
    }
    int mins = mins_until;
    if (mins < 0) {
        mins = 0; /* see header: floor rather than print a negative countdown */
    }
    snprintf(out, out_sz, "IN %d MIN", mins);
}

int32_t lineup_layout_bar_fill_px(uint8_t pct_done, int32_t track_w_px)
{
    int pct = pct_done;
    if (pct > 100) {
        pct = 100; /* defensive: ff_now_row_t documents pct_done as already 0-100 */
    }
    if (track_w_px < 0) {
        track_w_px = 0;
    }
    return (int32_t)(((int64_t)track_w_px * (int64_t)pct) / 100);
}

float lineup_layout_chord_half_width_px(float dy)
{
    float r = LINEUP_LAYOUT_PUCK_RADIUS_PX;
    float d = fabsf(dy);
    if (d >= r) {
        return 0.0f;
    }
    return sqrtf(r * r - d * d);
}
