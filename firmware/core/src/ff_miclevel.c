/**
 * ff_miclevel.c — see ff_miclevel.h.
 */
#include "ff_miclevel.h"

#include <math.h>

void ff_miclevel_dc_reset(ff_miclevel_dc_state_t *st)
{
    if (st == NULL) return;
    st->prev_in = 0.0f;
    st->prev_out = 0.0f;
}

float ff_miclevel_dc_remove(ff_miclevel_dc_state_t *st, float x)
{
    if (st == NULL) return x;
    float const y = x - st->prev_in + FF_MICLEVEL_DC_POLE * st->prev_out;
    st->prev_in = x;
    st->prev_out = y;
    return y;
}

float ff_miclevel_to_dbfs(float linear_magnitude)
{
    float const mag = fabsf(linear_magnitude);
    if (mag <= 0.0f) return FF_MICLEVEL_FLOOR_DBFS;
    float const db = 20.0f * log10f(mag / FF_MICLEVEL_FULL_SCALE);
    return (db < FF_MICLEVEL_FLOOR_DBFS) ? FF_MICLEVEL_FLOOR_DBFS : db;
}

void ff_miclevel_frame_compute(float const *samples, size_t n, ff_miclevel_frame_t *out)
{
    if (out == NULL) return;
    if (samples == NULL || n == 0u) {
        out->rms_dbfs = FF_MICLEVEL_FLOOR_DBFS;
        out->peak_dbfs = FF_MICLEVEL_FLOOR_DBFS;
        return;
    }

    double sum_sq = 0.0; /* double accumulator — n can be up to a few hundred samples per frame */
    float peak = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float const s = samples[i];
        sum_sq += (double)s * (double)s;
        float const a = fabsf(s);
        if (a > peak) peak = a;
    }
    float const rms = sqrtf((float)(sum_sq / (double)n));

    out->rms_dbfs = ff_miclevel_to_dbfs(rms);
    out->peak_dbfs = ff_miclevel_to_dbfs(peak);
}

void ff_miclevel_envelope_reset(ff_miclevel_envelope_t *env)
{
    if (env == NULL) return;
    env->value_dbfs = FF_MICLEVEL_FLOOR_DBFS;
}

void ff_miclevel_envelope_update(ff_miclevel_envelope_t *env, float frame_rms_dbfs, uint32_t dt_ms)
{
    if (env == NULL) return;

    /* One-pole exponential move toward frame_rms_dbfs, with the pole
     * derived from whichever time constant applies (rising = attack,
     * falling = release) so a longer dt_ms (a delayed/coalesced frame)
     * still moves proportionally further rather than under-reacting —
     * same "explicit dt, no assumed tick rate" reasoning as
     * ff_batt_filter_push (ff_batt.h). alpha = 1 - exp(-dt/tau); tau
     * here is taken as the FULL attack/release window (not a 63%-style
     * RC tau) since these are simple UI-meter constants, not a modeled
     * physical time constant — see this header's own "Interpretation
     * calls" section. */
    float const tau_ms =
        (frame_rms_dbfs > env->value_dbfs) ? (float)FF_MICLEVEL_ENV_ATTACK_MS : (float)FF_MICLEVEL_ENV_RELEASE_MS;
    float alpha = (tau_ms > 0.0f) ? ((float)dt_ms / tau_ms) : 1.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    if (alpha < 0.0f) alpha = 0.0f;

    env->value_dbfs += alpha * (frame_rms_dbfs - env->value_dbfs);
}
