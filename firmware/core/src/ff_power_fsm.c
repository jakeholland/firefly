/**
 * ff_power_fsm.c — see ff_power_fsm.h for the contract this implements.
 */
#include "ff_power_fsm.h"

#include <string.h>

#include "ff_clock.h" /* ff_time_reached — wraparound-safe deadline check */

void ff_power_fsm_init(ff_power_fsm_t *fsm)
{
    if (fsm == NULL) {
        return;
    }
    memset(fsm, 0, sizeof(*fsm));
}

ff_power_fsm_event_t ff_power_fsm_tick(ff_power_fsm_t *fsm, uint32_t now_ms, bool pwr_pressed)
{
    if (fsm == NULL) {
        return FF_POWER_FSM_EVENT_NONE;
    }

    ff_power_fsm_event_t ev = FF_POWER_FSM_EVENT_NONE;

    /* A raw level change (re)starts the debounce window. Re-arming on
     * EVERY raw change (not just the first one away from the debounced
     * state) means a bounce back to the debounced level mid-window
     * simply cancels itself out below, rather than needing separate
     * cancel logic. */
    if (pwr_pressed != fsm->raw_level) {
        fsm->raw_level = pwr_pressed;
        fsm->raw_change_ms = now_ms;
        fsm->raw_pending = true;
    }

    if (fsm->raw_pending && ff_time_reached(now_ms, fsm->raw_change_ms + FF_POWER_FSM_DEBOUNCE_MS)) {
        fsm->raw_pending = false;
        if (fsm->raw_level != fsm->debounced_pressed) {
            fsm->debounced_pressed = fsm->raw_level;
            if (fsm->debounced_pressed) {
                /* Debounced PRESS edge: start the press cycle. No event
                 * of its own — SHORT/LONG/RELEASE are all decided later,
                 * either on the long-threshold check below (a future
                 * tick) or on the matching release edge. */
                fsm->press_start_ms = now_ms;
                fsm->long_fired = false;
            } else {
                /* Debounced RELEASE edge: exactly one of SHORT_PRESS /
                 * RELEASE, per this header's rule — mutually exclusive,
                 * decided by whether LONG_PRESS already fired this press
                 * cycle. */
                ev = fsm->long_fired ? FF_POWER_FSM_EVENT_RELEASE : FF_POWER_FSM_EVENT_SHORT_PRESS;
                fsm->long_fired = false;
            }
        }
        /* else: the raw level bounced back to what was already
         * debounced (a same-tick or intra-window jitter) — nothing
         * actually changed, no event. */
    }

    /* Long-press check: independent of the debounce-edge branch above,
     * and never contends with it for the same tick's event slot. The
     * only way debounced_pressed just became true is the press-edge
     * branch directly above, which starts press_start_ms == now_ms —
     * so ff_time_reached(now_ms, now_ms + FF_POWER_FSM_LONG_MS) is
     * false for any positive threshold. The only way it just became
     * false is the release-edge branch, which this condition's own
     * `debounced_pressed` guard excludes. So at most one branch ever
     * sets `ev` in a single call. */
    if (fsm->debounced_pressed && !fsm->long_fired &&
        ff_time_reached(now_ms, fsm->press_start_ms + FF_POWER_FSM_LONG_MS)) {
        fsm->long_fired = true;
        ev = FF_POWER_FSM_EVENT_LONG_PRESS;
    }

    return ev;
}

void ff_power_fsm_request_reboot(ff_power_fsm_t *fsm)
{
    if (fsm == NULL) {
        return;
    }
    fsm->reboot_pending = true;
}

bool ff_power_fsm_reboot_ready(ff_power_fsm_t *fsm, bool boot_pressed)
{
    if (fsm == NULL || !fsm->reboot_pending) {
        return false;
    }
    if (boot_pressed) {
        return false; /* still held — reporting now would risk the ROM bootloader on reset */
    }
    fsm->reboot_pending = false; /* one-shot: report ready exactly once */
    return true;
}
