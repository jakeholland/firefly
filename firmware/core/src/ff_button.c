/**
 * ff_button.c — see ff_button.h for the contract this implements.
 */
#include "ff_button.h"

#include <string.h>

#include "ff_clock.h" /* ff_time_reached — wraparound-safe deadline check */

void ff_button_init(ff_button_t *b)
{
    if (b == NULL) {
        return;
    }
    memset(b, 0, sizeof(*b));
}

bool ff_button_tick(ff_button_t *b, uint32_t now_ms, bool level)
{
    if (b == NULL) {
        return false;
    }

    bool fired = false;

    /* A raw level change (re)starts the debounce window — re-arming on
     * every raw change, not just the first away from the debounced
     * state, so a bounce back to the debounced level mid-window simply
     * cancels itself out below (ff_power_fsm.c's identical comment). */
    if (level != b->raw_level) {
        b->raw_level = level;
        b->raw_change_ms = now_ms;
        b->raw_pending = true;
    }

    if (b->raw_pending && ff_time_reached(now_ms, b->raw_change_ms + FF_BUTTON_DEBOUNCE_MS)) {
        b->raw_pending = false;
        if (b->raw_level != b->debounced_pressed) {
            b->debounced_pressed = b->raw_level;
            /* Fire on the debounced PRESS edge only — the release edge
             * carries no event of its own, unlike ff_power_fsm's
             * SHORT_PRESS/RELEASE split, because HOME has no long-press
             * distinction to make: one debounced press, one event. */
            fired = b->debounced_pressed;
        }
        /* else: the raw level bounced back to what was already
         * debounced — nothing actually changed, no event. */
    }

    return fired;
}
