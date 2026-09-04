/**
 * ff_multitap.c — see ff_multitap.h for the contract this implements.
 */
#include "ff_multitap.h"

#include <string.h>

#include "ff_clock.h" /* ff_time_reached — wraparound-safe deadline check */

void ff_multitap_init(ff_multitap_t *m)
{
    if (m == NULL) {
        return;
    }
    memset(m, 0, sizeof(*m));
}

bool ff_multitap_press(ff_multitap_t *m, uint32_t now_ms)
{
    if (m == NULL) {
        return false;
    }

    if (m->count > 0) {
        /* Rule 0 — bounce reject (ff_multitap.h's FF_MULTITAP_BOUNCE_MS
         * doc comment): a gap this short since the last press is the
         * SAME physical press bouncing, not a genuine second one.
         * Mutates nothing and returns immediately — evaluated before
         * rule 1 below, since a bounce this close can never also be "too
         * long" a gap. */
        if (!ff_time_reached(now_ms, m->last_ms + FF_MULTITAP_BOUNCE_MS)) {
            return false;
        }

        bool const gap_too_long = ff_time_reached(now_ms, m->last_ms + FF_MULTITAP_MAX_GAP_MS);
        bool const window_expired = ff_time_reached(now_ms, m->first_ms + FF_MULTITAP_WINDOW_MS);
        if (gap_too_long || window_expired) {
            /* The run is stale — THIS press starts a fresh one rather
             * than extending or being dropped (ff_multitap.h rule 1). */
            m->count = 0;
        }
    }

    if (m->count == 0) {
        m->first_ms = now_ms;
    }
    m->count++;
    m->last_ms = now_ms;

    if (m->count >= FF_MULTITAP_COUNT) {
        /* Reached (>= guards against a corrupted count that somehow
         * skipped exactly FF_MULTITAP_COUNT; the normal path always
         * hits it exactly). Reset to idle so a 6th press starts a brand
         * new run of 1, per spec. */
        m->count = 0;
        return true;
    }
    return false;
}

bool ff_multitap_pending(ff_multitap_t const *m, uint32_t now_ms)
{
    if (m == NULL || m->count == 0) {
        return false;
    }
    if (ff_time_reached(now_ms, m->last_ms + FF_MULTITAP_MAX_GAP_MS)) {
        return false;
    }
    if (ff_time_reached(now_ms, m->first_ms + FF_MULTITAP_WINDOW_MS)) {
        return false;
    }
    return true;
}
