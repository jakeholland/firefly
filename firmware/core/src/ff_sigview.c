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
 * Presence (S22 AC2; re-based onto heard-presence 2026-09-07 — see
 * ff_sigview.h's top comment)
 * ------------------------------------------------------------------- */

ff_sigview_presence_t ff_sigview_presence(ff_crew_presence_t heard, uint32_t heard_age_ms, uint32_t *out_age_ms)
{
    if (heard == FF_CREW_PRESENCE_NEVER) {
        return FF_PRESENCE_LINKED; /* paired but never a packet — no honest age */
    }

    if (out_age_ms != NULL) {
        *out_age_ms = heard_age_ms;
    }

    return (heard == FF_CREW_PRESENCE_LOST) ? FF_PRESENCE_LOST : FF_PRESENCE_SEEN;
}
