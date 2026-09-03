/**
 * ff_sigview.c — see ff_sigview.h.
 *
 * Pure C11. Everything but the presence classifier and the
 * ff_target_kind_t vocabulary moved to ff_inbox.c (S24 inbox rework) /
 * the shell — see ff_sigview.h's top comment.
 */
#include "ff_sigview.h"

#include <stddef.h> /* NULL */
#include <stdint.h>

/* ---------------------------------------------------------------------
 * Presence (S22 AC2)
 * ------------------------------------------------------------------- */

ff_sigview_presence_t ff_sigview_presence(ff_freshness_t pos_fresh, uint32_t pos_age_ms, bool have_rssi,
                                          uint32_t rssi_age_ms, uint32_t *out_age_ms)
{
    /* Gather the honest sighting ages. The position leg offers an age ONLY
     * for a measured fix (LIVE/STALE/LOST). NEVER has no fix; ASSERTED is
     * silent on age (issue #33) — neither contributes a sighting age, no
     * matter what pos_age_ms happens to hold. */
    bool     have_pos_age = (pos_fresh == FF_FRESH_LIVE || pos_fresh == FF_FRESH_STALE ||
                         pos_fresh == FF_FRESH_LOST);
    bool     have_any = false;
    uint32_t age = 0;

    if (have_pos_age) {
        age     = pos_age_ms;
        have_any = true;
    }
    if (have_rssi) {
        if (!have_any || rssi_age_ms < age) {
            age = rssi_age_ms;
        }
        have_any = true;
    }

    if (!have_any) {
        return FF_PRESENCE_LINKED; /* paired but never a sighting — no honest age */
    }

    if (out_age_ms != NULL) {
        *out_age_ms = age;
    }
    /* Inclusive toward SEEN at the boundary, matching ff_crew's own
     * LIVE/STALE/LOST convention (age == FF_CREW_LOST_MS is still SEEN). */
    return (age > FF_CREW_LOST_MS) ? FF_PRESENCE_LOST : FF_PRESENCE_SEEN;
}
