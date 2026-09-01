/**
 * ff_idle.c — see ff_idle.h for the contract this implements.
 */
#include "ff_idle.h"

#include <string.h>

/* Wraparound-safe "has now_ms reached deadline_ms yet" — the same
 * convention ff_flare.c / ff_power_fsm.c document: INCLUSIVE at the
 * boundary, so now_ms == deadline_ms already fires. */
static bool ff_idle_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

void ff_idle_init(ff_idle_t *idle)
{
    if (idle == NULL) {
        return;
    }
    memset(idle, 0, sizeof(*idle));
}

ff_idle_state_t ff_idle_tick(ff_idle_t *idle, uint32_t now_ms, bool keep_awake)
{
    if (idle == NULL) {
        return FF_IDLE_STATE_ACTIVE;
    }

    if (keep_awake) {
        /* Force + hold ACTIVE, and continuously re-pin the idle
         * reference to now — so idle time never accrues while this
         * holds, and starts fresh from the instant it releases (see the
         * header's "Keep-awake" note). This also reactivates a state
         * that was previously forced OFF (ff_idle_force_off): a
         * keep-awake source becoming true is itself a wake, e.g. an
         * incoming flare takeover arriving while the screen is off. */
        idle->state = FF_IDLE_STATE_ACTIVE;
        idle->ref_ms = now_ms;
        return idle->state;
    }

    if (idle->state == FF_IDLE_STATE_OFF) {
        /* OFF is sticky against natural ticking — see the header's "OFF
         * is sticky" note. Only ff_idle_input (a real wake) or a newly
         * true keep_awake (handled above) walks it back. */
        return idle->state;
    }

    if (ff_idle_reached(now_ms, idle->ref_ms + FF_IDLE_T_OFF_MS)) {
        idle->state = FF_IDLE_STATE_OFF;
    } else if (ff_idle_reached(now_ms, idle->ref_ms + FF_IDLE_T_DIM_MS)) {
        idle->state = FF_IDLE_STATE_DIM;
    }
    /* else: elapsed idle time has not reached FF_IDLE_T_DIM_MS yet —
     * stays whatever it already was (ACTIVE; DIM can only be reached
     * here, and OFF short-circuited above). */

    return idle->state;
}

void ff_idle_input(ff_idle_t *idle, uint32_t now_ms)
{
    if (idle == NULL) {
        return;
    }
    idle->state = FF_IDLE_STATE_ACTIVE;
    idle->ref_ms = now_ms;
}

void ff_idle_force_off(ff_idle_t *idle)
{
    if (idle == NULL) {
        return;
    }
    idle->state = FF_IDLE_STATE_OFF;
}

ff_idle_state_t ff_idle_state(ff_idle_t const *idle)
{
    if (idle == NULL) {
        return FF_IDLE_STATE_ACTIVE;
    }
    return idle->state;
}
