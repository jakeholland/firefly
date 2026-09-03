/**
 * ff_idle.c — see ff_idle.h for the contract this implements.
 */
#include "ff_idle.h"

#include <string.h>

#include "ff_clock.h"    /* ff_time_reached — wraparound-safe deadline check */
#include "ff_settings.h" /* FF_BRIGHTNESS_MIN_PCT — ff_idle_brightness_pct's DIM value */

void ff_idle_init(ff_idle_t *idle)
{
    if (idle == NULL) {
        return;
    }
    memset(idle, 0, sizeof(*idle));
}

ff_idle_state_t ff_idle_tick(ff_idle_t *idle, uint32_t now_ms, bool keep_awake, bool sleep_inhibit)
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

    if (idle->state == FF_IDLE_STATE_SLEEP) {
        /* SLEEP is sticky against natural ticking, same reasoning as OFF
         * below (header's "OFF and SLEEP are sticky" note) — it is the
         * highest state there is, so nothing can advance it further.
         * Only ff_idle_input (a real wake) or a newly true keep_awake
         * (handled above) walks it back. */
        return idle->state;
    }

    /* SLEEP's threshold is checked BEFORE the OFF short-circuit below —
     * not folded into that branch — so a single large now_ms jump from
     * ANY state (not only from OFF) lands directly in SLEEP in one
     * call, mirroring the property this file already guaranteed for
     * OFF/DIM ("OFF reached directly without visiting DIM first").
     * Measured from the SAME ref_ms every other threshold here uses
     * (header's top comment).
     *
     * `!sleep_inhibit` gates ONLY this one comparison — the OFF->SLEEP
     * transition — per ff_idle.h's "Sleep inhibit" section: it does not
     * touch ref_ms (unlike keep_awake above) and does not affect the
     * DIM/OFF checks below at all, so a USB-tethered puck still dims and
     * blanks its screen on schedule; it just never advances into SLEEP
     * while inhibited. Nothing else needs to change to satisfy "once
     * inhibit releases, SLEEP is entered as soon as the threshold has
     * elapsed (immediately if it already has)": this comparison is
     * re-evaluated fresh, against the same unmoved ref_ms, on every tick
     * regardless of how sleep_inhibit behaved on prior ticks. */
    if (!sleep_inhibit && ff_time_reached(now_ms, idle->ref_ms + FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS)) {
        idle->state = FF_IDLE_STATE_SLEEP;
        return idle->state;
    }

    if (idle->state == FF_IDLE_STATE_OFF) {
        /* OFF is sticky against reversal to DIM/ACTIVE — see the
         * header's "OFF and SLEEP are sticky" note. The only forward
         * move left from here (SLEEP) was already checked above. */
        return idle->state;
    }

    if (ff_time_reached(now_ms, idle->ref_ms + FF_IDLE_T_OFF_MS)) {
        idle->state = FF_IDLE_STATE_OFF;
    } else if (ff_time_reached(now_ms, idle->ref_ms + FF_IDLE_T_DIM_MS)) {
        idle->state = FF_IDLE_STATE_DIM;
    }
    /* else: elapsed idle time has not reached FF_IDLE_T_DIM_MS yet —
     * stays whatever it already was (ACTIVE; DIM can only be reached
     * here, and OFF/SLEEP short-circuited above). */

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

void ff_idle_short_press(ff_idle_t *idle, uint32_t now_ms, bool keep_awake)
{
    if (idle == NULL) {
        return;
    }
    if (keep_awake) {
        /* A no-op — see this function's doc comment in ff_idle.h for
         * why (a keep_awake source dominates the next ff_idle_tick call
         * regardless, so acting here would only produce a one-tick
         * flicker with no spec guidance either way). */
        return;
    }
    if (idle->state == FF_IDLE_STATE_ACTIVE) {
        ff_idle_force_off(idle);
    } else {
        /* DIM, OFF, or SLEEP: a wake. */
        ff_idle_input(idle, now_ms);
    }
}

ff_idle_state_t ff_idle_state(ff_idle_t const *idle)
{
    if (idle == NULL) {
        return FF_IDLE_STATE_ACTIVE;
    }
    return idle->state;
}

uint8_t ff_idle_brightness_pct(ff_idle_state_t state, uint8_t stored_pct)
{
    switch (state) {
    case FF_IDLE_STATE_DIM:
        return (uint8_t)FF_BRIGHTNESS_MIN_PCT;
    case FF_IDLE_STATE_OFF:
    case FF_IDLE_STATE_SLEEP:
        return 0u;
    case FF_IDLE_STATE_ACTIVE:
    default:
        return stored_pct;
    }
}

void ff_idle_touch_gate_init(ff_idle_touch_gate_t *gate)
{
    if (gate == NULL) {
        return;
    }
    memset(gate, 0, sizeof(*gate));
}

bool ff_idle_touch_gate(ff_idle_t *idle, ff_idle_touch_gate_t *gate, uint32_t now_ms, bool pressed)
{
    if (idle == NULL || gate == NULL) {
        /* Fail open — see ff_idle.h's NULL-safety note on this
         * function: never silently block input over a wiring bug. */
        return pressed;
    }

    if (!pressed) {
        /* Release always resets the latch — whether this is a genuine
         * release or simply "no press happening" (never pressed, or
         * already released last call). Nothing to deliver either way. */
        gate->was_pressed = false;
        gate->swallowing = false;
        return false;
    }

    bool const begin = !gate->was_pressed;
    gate->was_pressed = true;

    if (begin) {
        /* The decision is made ONCE, right here, at press-begin — never
         * re-evaluated for the rest of this gesture (see ff_idle.h's
         * "state matters only at press START" note). */
        gate->swallowing = (ff_idle_state(idle) != FF_IDLE_STATE_ACTIVE);
        if (gate->swallowing) {
            /* The wake itself — same call every other input source on
             * this device makes (ff_idle_input's own doc comment). */
            ff_idle_input(idle, now_ms);
        }
    }

    return !gate->swallowing;
}
