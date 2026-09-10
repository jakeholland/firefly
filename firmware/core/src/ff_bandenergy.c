/**
 * ff_bandenergy.c — see ff_bandenergy.h.
 */
#include "ff_bandenergy.h"

#include <math.h>
#include <string.h>

#include "ff_miclevel.h" /* FF_MICLEVEL_FLOOR_DBFS, ff_miclevel_to_dbfs */

/* One-pole (RC) lowpass coefficient for corner frequency `fc_hz` at this
 * module's fixed sample rate — `alpha = dt / (RC + dt)`, `RC = 1 /
 * (2*pi*fc)`, algebraically rearranged to avoid a division-by-a-
 * division: `alpha = (dt*2*pi*fc) / (1 + dt*2*pi*fc)`. Computed once per
 * `ff_bandenergy_frame_compute` call (not per-sample — all 320 samples
 * in a frame share the same coefficient), so this is three sinf-free
 * multiplications, not a per-sample cost. */
static float lp_alpha(float fc_hz)
{
    float const dt = 1.0f / (float)FF_BANDENERGY_SAMPLE_RATE_HZ;
    float const w = dt * 2.0f * (float)M_PI * fc_hz;
    return w / (1.0f + w);
}

void ff_bandenergy_reset(ff_bandenergy_t *st)
{
    if (st == NULL) return;
    memset(st, 0, sizeof(*st));
    /* `primed = false` (the zero value) is the honest reset state — see
     * this struct's own doc comment (ff_bandenergy.h): the first real
     * sample seeds lp60/lp200/lp2000 directly rather than easing up from
     * an assumed-0 starting point. */
}

void ff_bandenergy_frame_compute(ff_bandenergy_t *st, float const *samples, size_t n, ff_bandenergy_frame_t *out)
{
    if (out == NULL) return;
    out->low_dbfs = FF_MICLEVEL_FLOOR_DBFS;
    out->mid_dbfs = FF_MICLEVEL_FLOOR_DBFS;
    if (st == NULL || samples == NULL || n == 0u) return;

    float const a60 = lp_alpha(FF_BANDENERGY_LOW_LO_HZ);
    float const a200 = lp_alpha(FF_BANDENERGY_LOW_HI_HZ); /* == FF_BANDENERGY_MID_LO_HZ — one shared stage */
    float const a2000 = lp_alpha(FF_BANDENERGY_MID_HI_HZ);

    if (!st->primed) {
        st->lp60 = samples[0];
        st->lp200 = samples[0];
        st->lp2000 = samples[0];
        st->primed = true;
    }

    double low_sq_sum = 0.0;
    double mid_sq_sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        float const x = samples[i];
        st->lp60 += a60 * (x - st->lp60);
        st->lp200 += a200 * (x - st->lp200);
        st->lp2000 += a2000 * (x - st->lp2000);

        float const low_sample = st->lp200 - st->lp60;
        float const mid_sample = st->lp2000 - st->lp200;
        low_sq_sum += (double)low_sample * (double)low_sample;
        mid_sq_sum += (double)mid_sample * (double)mid_sample;
    }

    float const low_rms = sqrtf((float)(low_sq_sum / (double)n));
    float const mid_rms = sqrtf((float)(mid_sq_sum / (double)n));
    out->low_dbfs = ff_miclevel_to_dbfs(low_rms);
    out->mid_dbfs = ff_miclevel_to_dbfs(mid_rms);
}
