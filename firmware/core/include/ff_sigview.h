/**
 * ff_sigview.h — core/sigview: honest presence classification, plus the
 * shared send-target-kind vocabulary.
 *
 * Originally (S22 slice a) this module was the whole Signals-face
 * view-model: it merged the feed and crew roster into an ordered row
 * list AND owned the send-target state machine. The S24 inbox rework
 * (docs/specs/S24-signals-inbox-spec.md) replaced that row-list/target
 * machinery with `ff_inbox_t` (ff_inbox.h) — a per-conversation model
 * that supersedes the old flat "every feed item + every quiet member"
 * projection. What's LIVE here now is only the two pieces `ff_inbox.c`
 * (and app-level target-tracking code) still reuses directly rather than
 * reimplementing:
 *
 *   - `ff_sigview_presence` / `ff_sigview_presence_t` — the honest
 *     presence classifier (SEEN / LOST / LINKED from real evidence only).
 *     `ff_inbox_t` calls this for its own presence field rather than
 *     duplicating the logic; see ff_inbox.h's top comment.
 *   - `ff_target_kind_t` (`FF_TARGET_WHOLE_CREW` / `FF_TARGET_MEMBER`) —
 *     the send-scope vocabulary the app-level send machinery (S22 slice d
 *     confirm-armed logic, now living in the shell) still uses as a
 *     value type. The state machine that used to live in `ff_sigview_t`
 *     (target_select/clear/reset_after_send, rally confirm) has moved to
 *     the shell; this header keeps only the enum.
 *
 * For the live per-conversation Signals/Inbox model — ordering, identity
 * join, target state — see `ff_inbox.h`, not this file.
 *
 * ## Honesty rule this module is bound by (CLAUDE.md, [[firefly-touch-cal-default]])
 * **Presence is a freshness value, never a guessed "online"/"now".**
 * `ff_sigview_presence` derives its result only from real evidence — a
 * measured position age (`ff_crew_freshness`) and/or a direct-packet RSSI
 * age. A member never heard from classifies as `FF_PRESENCE_LINKED`
 * (paired but no sighting) — NOT a fabricated recent time. An ASSERTED
 * position (Meshtastic LOC_MANUAL, issue #33) is silent on age and so
 * contributes no sighting age here.
 *
 * Pure C11, no I/O, no LVGL, zero heap allocation.
 */
#ifndef FF_SIGVIEW_H
#define FF_SIGVIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "ff_crew.h" /* ff_freshness_t — ff_sigview_presence's pos_fresh input */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Presence category for a quiet crew member — the honest "how recently is
 * there evidence of this person" axis the screen renders dimmed next to a
 * quiet member. Derived by `ff_sigview_presence`; see the honesty note in
 * this header's top comment.
 *
 *  - FF_PRESENCE_SEEN   — recent evidence exists (a measured position
 *                         and/or a direct packet within FF_CREW_LOST_MS);
 *                         the row carries the sighting age in `age_ms`,
 *                         which the screen formats ("SEEN 6 MIN").
 *  - FF_PRESENCE_LOST   — evidence exists but the freshest sighting is
 *                         older than FF_CREW_LOST_MS; `age_ms` still
 *                         carries the (old) real age.
 *  - FF_PRESENCE_LINKED — paired, but NO sighting has ever arrived (no
 *                         measured position, no direct packet). There is
 *                         no honest age; `age_ms` is 0 and meaningless.
 */
typedef enum {
    FF_PRESENCE_SEEN,
    FF_PRESENCE_LOST,
    FF_PRESENCE_LINKED,
} ff_sigview_presence_t;

/** Which send target a send acts on. WHOLE_CREW is the default and the
 * zero value, so a zero-initialized target holder targets the whole crew.
 * The state machine that once lived on `ff_sigview_t` (select / clear /
 * reset_after_send, rally-confirm arming) now lives in the shell
 * (`app/ff_shell.c`); this enum is the shared value type it still uses. */
typedef enum {
    FF_TARGET_WHOLE_CREW = 0,
    FF_TARGET_MEMBER,
} ff_target_kind_t;

/**
 * ff_sigview_presence — classify a member's presence as of `now_ms` from
 * its real evidence, and (for SEEN/LOST) report the freshest sighting age
 * via `out_age_ms`.
 *
 * Inputs are the honest, already-computed signals so the function is pure
 * and each branch is directly testable without constructing a whole
 * member:
 *   - `pos_fresh`     : the member's position freshness
 *                       (`ff_crew_freshness`).
 *   - `pos_age_ms`    : age of that position fix; a MEANINGFUL sighting
 *                       age ONLY when `pos_fresh` is LIVE / STALE / LOST.
 *                       For NEVER (no fix) and ASSERTED (silent on age,
 *                       issue #33) the position leg offers no age and is
 *                       ignored regardless of this value.
 *   - `have_rssi`     : true iff a direct packet has ever been heard
 *                       (`m->rssi_dbm != INT16_MIN`).
 *   - `rssi_age_ms`   : age of that last direct packet; used only when
 *                       `have_rssi`.
 *   - `out_age_ms`    : optional; on SEEN/LOST set to the freshest (min)
 *                       of the available sighting ages; left untouched on
 *                       LINKED. May be NULL.
 *
 * Result:
 *   - no evidence at all               -> FF_PRESENCE_LINKED.
 *   - freshest sighting <= FF_CREW_LOST_MS -> FF_PRESENCE_SEEN.
 *   - freshest sighting  > FF_CREW_LOST_MS -> FF_PRESENCE_LOST.
 * (The <=/> split matches ff_crew's own inclusive-toward-STALE boundary
 * convention: an age of exactly FF_CREW_LOST_MS is still SEEN.)
 */
ff_sigview_presence_t ff_sigview_presence(ff_freshness_t pos_fresh, uint32_t pos_age_ms, bool have_rssi,
                                          uint32_t rssi_age_ms, uint32_t *out_age_ms);

#ifdef __cplusplus
}
#endif

#endif /* FF_SIGVIEW_H */
