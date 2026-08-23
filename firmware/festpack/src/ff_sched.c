/**
 * ff_sched.c — see ff_sched.h for the full contract (module placement
 * rationale, festival-day/now_min semantics, midnight-crossing rule,
 * inclusive "now" window).
 */
#include "ff_sched.h"

#include <string.h>

/* Effective end-of-set minute, folding a midnight-crossing set (end_min
 * < start_min) forward by one day so all comparisons against `now_min`
 * (which may itself exceed 1440 for late-night queries) are plain
 * integer comparisons. */
static int16_t sched_effective_end(fp_set_t const *s)
{
    if (s->end_min < s->start_min) {
        return (int16_t)(s->end_min + FF_SCHED_DAY_SPAN_MIN);
    }
    return s->end_min;
}

static bool sched_times_known(fp_set_t const *s)
{
    return s->start_min >= 0 && s->end_min >= 0;
}

uint8_t ff_sched_now_playing(fp_pack_t const *p, uint16_t day_doy, int16_t now_min,
                              ff_now_row_t out[], uint8_t max)
{
    uint8_t n = 0;
    for (uint16_t i = 0; i < p->n_sets && n < max; i++) {
        fp_set_t const *s = &p->sets[i];
        if (s->day_doy != day_doy) continue;
        if (!sched_times_known(s)) continue;

        int16_t end = sched_effective_end(s);
        if (now_min < s->start_min || now_min > end) continue;

        int16_t dur = (int16_t)(end - s->start_min);
        uint8_t pct;
        if (dur <= 0) {
            /* Degenerate zero-length set (start_min == end_min): there is
             * no meaningful "in progress", so treat it as already done
             * rather than divide by zero. */
            pct = 100;
        } else {
            int32_t scaled = (int32_t)(now_min - s->start_min) * 100;
            int32_t p100 = scaled / dur;
            if (p100 > 100) p100 = 100; /* defensive; unreachable given the range check above */
            if (p100 < 0) p100 = 0;
            pct = (uint8_t)p100;
        }

        out[n].set = s;
        out[n].mins_left = (int16_t)(end - now_min);
        out[n].pct_done = pct;
        n++;
    }
    return n;
}

bool ff_sched_next_starred(fp_pack_t const *p, uint16_t day_doy, int16_t now_min, ff_next_t *out)
{
    fp_set_t const *best = NULL;
    for (uint16_t i = 0; i < p->n_sets; i++) {
        fp_set_t const *s = &p->sets[i];
        if (!s->starred) continue;
        if (s->day_doy != day_doy) continue;
        if (!sched_times_known(s)) continue;
        if (s->start_min <= now_min) continue; /* not strictly future */

        if (best == NULL || s->start_min < best->start_min) {
            best = s;
        }
    }

    if (best == NULL) {
        out->set = NULL;
        out->mins_until = 0;
        return false;
    }

    out->set = best;
    out->mins_until = (int16_t)(best->start_min - now_min);
    return true;
}

void ff_sched_toggle_star(fp_pack_t *p, uint16_t set_idx)
{
    if (set_idx >= p->n_sets) return;
    p->sets[set_idx].starred = !p->sets[set_idx].starred;
}

uint16_t ff_sched_day_sets(fp_pack_t const *p, uint16_t day_doy,
                            fp_set_t const *out[], uint16_t max)
{
    uint16_t n = 0;
    for (uint16_t i = 0; i < p->n_sets && n < max; i++) {
        if (p->sets[i].day_doy != day_doy) continue;
        out[n++] = &p->sets[i];
    }
    return n;
}

bool ff_sched_day_tbd(fp_pack_t const *p, uint16_t day_doy)
{
    bool any = false;
    for (uint16_t i = 0; i < p->n_sets; i++) {
        if (p->sets[i].day_doy != day_doy) continue;
        any = true;
        if (sched_times_known(&p->sets[i])) {
            return false;
        }
    }
    return any;
}

void ff_sched_alarm_init(ff_sched_alarm_t *st)
{
    memset(st, 0, sizeof(*st));
}

static bool sched_bit_get(uint8_t const *bits, uint16_t idx)
{
    return (bits[idx / 8] >> (idx % 8)) & 1u;
}

static void sched_bit_set(uint8_t *bits, uint16_t idx)
{
    bits[idx / 8] |= (uint8_t)(1u << (idx % 8));
}

fp_set_t const *ff_sched_alarm_tick(ff_sched_alarm_t *st, fp_pack_t const *p,
                                     uint16_t day_doy, int16_t now_min)
{
    uint16_t best_idx = UINT16_MAX;

    for (uint16_t i = 0; i < p->n_sets; i++) {
        fp_set_t const *s = &p->sets[i];
        if (!s->starred) continue;
        if (s->day_doy != day_doy) continue;
        if (!sched_times_known(s)) continue;
        if (sched_bit_get(st->fired, i)) continue;

        int16_t mins_until = (int16_t)(s->start_min - now_min);
        if (mins_until > 15) continue; /* not due yet */

        int16_t end = sched_effective_end(s);
        if (now_min > end) continue; /* already over; don't fire a stale alert */

        if (best_idx == UINT16_MAX || s->start_min < p->sets[best_idx].start_min) {
            best_idx = i;
        }
    }

    if (best_idx == UINT16_MAX) return NULL;

    sched_bit_set(st->fired, best_idx);
    return &p->sets[best_idx];
}
