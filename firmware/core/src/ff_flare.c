#include "ff_flare.h"

#include <string.h>

/* ------------------------------------------------------------------- */
/* internal helpers                                                     */
/* ------------------------------------------------------------------- */

/* Wraparound-safe "has now_ms reached deadline_ms yet" check, matching
 * ff_clock_t's documented convention (firmware/platform/include/ff_clock.h:
 * "callers compare with subtraction, not '<'"). INCLUSIVE at the boundary
 * — see ff_flare.h's judgment call (1). */
static bool ff_flare_deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static ff_flare_result_t ff_flare_no_result(void)
{
    ff_flare_result_t r;
    r.intent = FF_FLARE_INTENT_NONE;
    r.dur_s = 0;
    r.should_alert = false;
    return r;
}

/* ------------------------------------------------------------------- */
/* lifecycle                                                            */
/* ------------------------------------------------------------------- */

void ff_flare_init(ff_flare_t *f)
{
    if (!f) {
        return;
    }
    memset(f, 0, sizeof(*f));
    f->state = FF_FLARE_IDLE;
}

/* ------------------------------------------------------------------- */
/* sending                                                              */
/* ------------------------------------------------------------------- */

ff_flare_result_t ff_flare_send_begin(ff_flare_t *f, uint16_t dur_s, uint32_t now_ms)
{
    ff_flare_result_t r = ff_flare_no_result();
    if (!f) {
        return r;
    }

    uint16_t dur = (dur_s == 0u) ? FF_FLARE_DEFAULT_DUR_S : dur_s;

    f->state = FF_FLARE_SENDING;
    f->node_id = 0;
    f->expiry_ms = now_ms + (uint32_t)dur * 1000u;

    r.intent = FF_FLARE_INTENT_SEND_FLARE;
    r.dur_s = dur;
    return r;
}

ff_flare_result_t ff_flare_send_cancel(ff_flare_t *f)
{
    ff_flare_result_t r = ff_flare_no_result();
    if (!f || f->state != FF_FLARE_SENDING) {
        return r; /* not sending: nothing to cancel, no FLARE_END emitted */
    }

    f->state = FF_FLARE_IDLE;
    f->expiry_ms = 0;

    r.intent = FF_FLARE_INTENT_SEND_FLARE_END;
    return r;
}

/* ------------------------------------------------------------------- */
/* receiving                                                            */
/* ------------------------------------------------------------------- */

ff_flare_result_t ff_flare_on_flare_rx(ff_flare_t *f, uint32_t node_id, bool paired, uint16_t dur_s,
                                        uint32_t now_ms)
{
    ff_flare_result_t r = ff_flare_no_result();
    if (!f || !paired) {
        return r; /* unpaired sender: ignored entirely, no state change */
    }

    /* Newest flare wins: always a fresh, unlocked takeover, from any
     * prior state (see ff_flare.h's judgment calls 3 and 4). */
    f->state = FF_FLARE_RECEIVED;
    f->node_id = node_id;
    f->expiry_ms = now_ms + (uint32_t)dur_s * 1000u;

    r.should_alert = true; /* haptic override applies unconditionally, incl. quiet hours */
    return r;
}

ff_flare_result_t ff_flare_on_flare_end_rx(ff_flare_t *f, uint32_t node_id)
{
    ff_flare_result_t r = ff_flare_no_result();
    if (!f) {
        return r;
    }
    if ((f->state != FF_FLARE_RECEIVED && f->state != FF_FLARE_LOCKED) || f->node_id != node_id) {
        return r; /* no active takeover from exactly this node: ignored */
    }

    f->state = FF_FLARE_IDLE;
    f->node_id = 0;
    f->expiry_ms = 0;
    return r;
}

/* ------------------------------------------------------------------- */
/* takeover controls                                                    */
/* ------------------------------------------------------------------- */

ff_flare_result_t ff_flare_go(ff_flare_t *f)
{
    ff_flare_result_t r = ff_flare_no_result();
    if (!f || f->state != FF_FLARE_RECEIVED) {
        return r;
    }
    f->state = FF_FLARE_LOCKED;
    return r;
}

ff_flare_result_t ff_flare_dismiss(ff_flare_t *f)
{
    ff_flare_result_t r = ff_flare_no_result();
    if (!f || (f->state != FF_FLARE_RECEIVED && f->state != FF_FLARE_LOCKED)) {
        return r;
    }
    f->state = FF_FLARE_IDLE;
    f->node_id = 0;
    f->expiry_ms = 0;
    return r;
}

/* ------------------------------------------------------------------- */
/* tick — periodic expiry check                                        */
/* ------------------------------------------------------------------- */

ff_flare_result_t ff_flare_tick(ff_flare_t *f, uint32_t now_ms)
{
    ff_flare_result_t r = ff_flare_no_result();
    if (!f) {
        return r;
    }

    switch (f->state) {
    case FF_FLARE_SENDING:
        if (ff_flare_deadline_reached(now_ms, f->expiry_ms)) {
            f->state = FF_FLARE_IDLE;
            f->expiry_ms = 0;
            r.intent = FF_FLARE_INTENT_SEND_FLARE_END;
        }
        break;
    case FF_FLARE_RECEIVED:
    case FF_FLARE_LOCKED:
        if (ff_flare_deadline_reached(now_ms, f->expiry_ms)) {
            f->state = FF_FLARE_IDLE;
            f->node_id = 0;
            f->expiry_ms = 0;
        }
        break;
    case FF_FLARE_IDLE:
    default:
        break;
    }
    return r;
}

/* ------------------------------------------------------------------- */
/* accessors                                                            */
/* ------------------------------------------------------------------- */

uint32_t ff_flare_locked_node(ff_flare_t const *f)
{
    if (!f || f->state != FF_FLARE_LOCKED) {
        return 0;
    }
    return f->node_id;
}
