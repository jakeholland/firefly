/**
 * flare_fmt.c — see flare_fmt.h.
 */
#include "flare_fmt.h"

#include <math.h>
#include <stdio.h>

void ff_flare_fmt_headline(char *out, size_t out_sz, char const *name)
{
    if (out == NULL || out_sz == 0) {
        return;
    }
    if (name != NULL && name[0] != '\0') {
        snprintf(out, out_sz, "%s IS FLARING", name);
    } else {
        snprintf(out, out_sz, "SOMEONE IS FLARING");
    }
}

char const *ff_flare_fmt_compass8(float bearing_deg)
{
    static char const *const points[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

    /* Normalize into [0, 360) — fmodf alone can return a negative result
     * for a negative input (C99 fmodf keeps the sign of its first
     * operand), so a second wraparound fold-up is needed. */
    float a = fmodf(bearing_deg, 360.0f);
    if (a < 0.0f) {
        a += 360.0f;
    }

    /* Each point is a 45deg-wide bucket centered on its own heading;
     * shifting by half a bucket before dividing turns "nearest center"
     * into a plain floor-divide. A boundary value (e.g. exactly 22.5)
     * rolls forward into the next point — see this function's doc
     * comment for why. */
    int idx = (int)((a + 22.5f) / 45.0f) % 8;
    if (idx < 0) {
        idx += 8; /* defensive: idx is never negative for a in [0,360), kept for safety */
    }
    return points[idx];
}

void ff_flare_fmt_countdown(char *out, size_t out_sz, int32_t expires_in_ms)
{
    if (out == NULL || out_sz == 0) {
        return;
    }
    if (expires_in_ms < 0) {
        snprintf(out, out_sz, "--:--");
        return;
    }
    int32_t total_s = expires_in_ms / 1000;
    int32_t mins = total_s / 60;
    int32_t secs = total_s % 60;
    snprintf(out, out_sz, "%d:%02d", (int)mins, (int)secs);
}
