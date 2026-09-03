#include "ff_flare.h"

#include <string.h>

#include "ff_clock.h" /* ff_time_reached — wraparound-safe deadline check;
                        * see ff_flare.h's judgment call (1) for the
                        * INCLUSIVE-at-the-boundary convention this uses. */

/* ------------------------------------------------------------------- */
/* internal helpers                                                     */
/* ------------------------------------------------------------------- */

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
}

/* ------------------------------------------------------------------- */
/* sending — fully independent of takeover_active/locked_node_id        */
/* ------------------------------------------------------------------- */

ff_flare_result_t ff_flare_send_begin(ff_flare_t *f, uint16_t dur_s, uint32_t now_ms)
{
    ff_flare_result_t r = ff_flare_no_result();
    if (!f) {
        return r;
    }

    uint16_t dur = (dur_s == 0u) ? FF_FLARE_DEFAULT_DUR_S : dur_s;

    f->sending = true;
    f->send_expiry_ms = now_ms + (uint32_t)dur * 1000u;

    r.intent = FF_FLARE_INTENT_SEND_FLARE;
    r.dur_s = dur;
    return r;
}

ff_flare_result_t ff_flare_send_cancel(ff_flare_t *f)
{
    ff_flare_result_t r = ff_flare_no_result();
    if (!f || !f->sending) {
        return r; /* not sending: nothing to cancel, no FLARE_END emitted */
    }

    f->sending = false;
    f->send_expiry_ms = 0;

    r.intent = FF_FLARE_INTENT_SEND_FLARE_END;
    return r;
}

/* ------------------------------------------------------------------- */
/* receiving — never touches sending/send_expiry_ms; never touches      */
/* locked_node_id/locked_expiry_ms                                      */
/* ------------------------------------------------------------------- */

ff_flare_result_t ff_flare_on_flare_rx(ff_flare_t *f, uint32_t node_id, bool paired, uint16_t dur_s,
                                        uint32_t now_ms)
{
    ff_flare_result_t r = ff_flare_no_result();
    if (!f || !paired) {
        return r; /* unpaired sender: ignored entirely, no state change */
    }

    /* Newest flare always wins the pending-takeover slot, regardless of
     * `sending` (HIGH finding fix) or an existing lock (MEDIUM finding
     * fix) — see ff_flare.h's "Independent state" section. */
    f->takeover_active = true;
    f->takeover_node_id = node_id;
    f->takeover_expiry_ms = now_ms + (uint32_t)dur_s * 1000u;

    r.should_alert = true; /* haptic override applies unconditionally, incl. quiet hours */
    return r;
}

ff_flare_result_t ff_flare_on_flare_end_rx(ff_flare_t *f, uint32_t node_id)
{
    ff_flare_result_t r = ff_flare_no_result();
    if (!f) {
        return r;
    }

    /* node_id == 0 is never a real sender (see ff_flare_locked_node's "0 =
     * not locked" sentinel) so an explicit != 0 guard keeps a hypothetical
     * node_id == 0 call from spuriously matching an unlocked/no-takeover
     * resting state. Independent fields: either, both, or neither may
     * match. */
    if (f->takeover_active && f->takeover_node_id == node_id) {
        f->takeover_active = false;
        f->takeover_node_id = 0;
        f->takeover_expiry_ms = 0;
    }
    if (f->locked_node_id != 0 && f->locked_node_id == node_id) {
        f->locked_node_id = 0;
        f->locked_expiry_ms = 0;
    }
    return r;
}

/* ------------------------------------------------------------------- */
/* takeover controls                                                    */
/* ------------------------------------------------------------------- */

ff_flare_result_t ff_flare_go(ff_flare_t *f)
{
    ff_flare_result_t r = ff_flare_no_result();
    if (!f || !f->takeover_active) {
        return r;
    }

    /* Explicit user decision: replaces any previous lock outright. */
    f->locked_node_id = f->takeover_node_id;
    f->locked_expiry_ms = f->takeover_expiry_ms;

    f->takeover_active = false;
    f->takeover_node_id = 0;
    f->takeover_expiry_ms = 0;
    return r;
}

ff_flare_result_t ff_flare_dismiss_takeover(ff_flare_t *f)
{
    ff_flare_result_t r = ff_flare_no_result();
    if (!f || !f->takeover_active) {
        return r;
    }

    /* Only the pending takeover; locked_node_id/locked_expiry_ms are not
     * read or written here, present or absent (review's race-case fix —
     * see ff_flare.h's "Intent-aware dismiss/release" section). */
    f->takeover_active = false;
    f->takeover_node_id = 0;
    f->takeover_expiry_ms = 0;
    return r;
}

ff_flare_result_t ff_flare_release_lock(ff_flare_t *f)
{
    ff_flare_result_t r = ff_flare_no_result();
    if (!f || f->locked_node_id == 0) {
        return r;
    }

    /* Only the lock; takeover_active/takeover_node_id/takeover_expiry_ms
     * are not read or written here, present or absent (review's
     * race-case fix — see ff_flare.h's "Intent-aware dismiss/release"
     * section). */
    f->locked_node_id = 0;
    f->locked_expiry_ms = 0;
    return r;
}

/* ------------------------------------------------------------------- */
/* tick — periodic expiry check, three independent deadlines            */
/* ------------------------------------------------------------------- */

ff_flare_result_t ff_flare_tick(ff_flare_t *f, uint32_t now_ms)
{
    ff_flare_result_t r = ff_flare_no_result();
    if (!f) {
        return r;
    }

    if (f->sending && ff_time_reached(now_ms, f->send_expiry_ms)) {
        f->sending = false;
        f->send_expiry_ms = 0;
        r.intent = FF_FLARE_INTENT_SEND_FLARE_END;
    }

    if (f->takeover_active && ff_time_reached(now_ms, f->takeover_expiry_ms)) {
        f->takeover_active = false;
        f->takeover_node_id = 0;
        f->takeover_expiry_ms = 0;
    }

    if (f->locked_node_id != 0 && ff_time_reached(now_ms, f->locked_expiry_ms)) {
        f->locked_node_id = 0;
        f->locked_expiry_ms = 0;
    }

    return r;
}

/* ------------------------------------------------------------------- */
/* accessors                                                            */
/* ------------------------------------------------------------------- */

uint32_t ff_flare_locked_node(ff_flare_t const *f)
{
    if (!f) {
        return 0;
    }
    return f->locked_node_id;
}
