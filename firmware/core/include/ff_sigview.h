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
 *     value type. The state machine that once lived in `ff_sigview_t`
 *     (target_select/clear/reset_after_send, rally confirm) has moved to
 *     the shell; this header keeps only the enum.
 *
 * For the live per-conversation Signals/Inbox model — ordering, identity
 * join, target state — see `ff_inbox.h`, not this file.
 *
 * ## Honesty rule this module is bound by (CLAUDE.md, [[firefly-touch-cal-default]])
 * **Presence is a freshness value, never a guessed "online"/"now".**
 *
 * 2026-09-07 amendment (presence-heard-vs-position, owner-verified "why
 * are we LOST?" investigation — docs/specs/S02-core-crew.md's own
 * amendment): `ff_sigview_presence` used to derive SEEN/LOST from a
 * measured POSITION age and/or a direct-packet RSSI age — which meant a
 * member heard constantly over NodeInfo/telemetry, but indoors with no
 * GPS fix (or simply not due for their next position broadcast — up to
 * 15 min on Meshtastic's stock default), read "LOST" even though the
 * radio was plainly still hearing them. That conflated two different
 * facts: "how old is their last known position" (`ff_crew_freshness`,
 * unchanged, position-only) and "is the radio still hearing this
 * person" (`ff_crew_presence`, core/ff_crew.h — ANY packet of any kind,
 * gated on nothing but a receive). This function now derives SEEN/LOST
 * from THAT axis alone — the one the "LOST" word on an Inbox row or the
 * CREW page has always meant to the person reading it. A member never
 * heard from at all still classifies as `FF_PRESENCE_LINKED` (paired,
 * no sighting) — NOT a fabricated recent time.
 *
 * Pure C11, no I/O, no LVGL, zero heap allocation.
 */
#ifndef FF_SIGVIEW_H
#define FF_SIGVIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "ff_crew.h" /* ff_crew_presence_t — ff_sigview_presence's heard-presence input */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Presence category for a quiet crew member — the honest "how recently is
 * there evidence of this person" axis the screen renders dimmed next to a
 * quiet member. Derived by `ff_sigview_presence`; see the honesty note in
 * this header's top comment.
 *
 *  - FF_PRESENCE_SEEN   — the radio has heard ANY packet from this member
 *                         (`ff_crew_presence` HEARD or STALE — within
 *                         FF_CREW_HEARD_LOST_MS); the row carries the
 *                         heard age in `age_ms`, which the screen formats
 *                         ("SEEN 6 MIN").
 *  - FF_PRESENCE_LOST   — has been heard before, but the freshest packet
 *                         is older than FF_CREW_HEARD_LOST_MS
 *                         (`ff_crew_presence` LOST); `age_ms` still
 *                         carries the (old) real heard age.
 *  - FF_PRESENCE_LINKED — paired, but NO packet has ever arrived from
 *                         this node (`ff_crew_presence` NEVER). There is
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
 * its real HEARD evidence (`ff_crew_presence`, core/ff_crew.h — ANY
 * packet, never position-gated), and (for SEEN/LOST) report the heard
 * age via `out_age_ms`.
 *
 * Inputs:
 *   - `heard`      : the member's heard-presence classification
 *                    (`ff_crew_presence(m, now_ms)`).
 *   - `heard_age_ms`: age of the freshest packet heard from this member
 *                    (`now_ms - m->last_heard_ms`); meaningful ONLY when
 *                    `heard != FF_CREW_PRESENCE_NEVER` — ignored
 *                    otherwise, same as this function's caller (there is
 *                    no honest age to pass when nothing has ever been
 *                    heard).
 *   - `out_age_ms` : optional; on SEEN/LOST set to `heard_age_ms`
 *                    verbatim; left untouched on LINKED. May be NULL.
 *
 * Result:
 *   - FF_CREW_PRESENCE_NEVER          -> FF_PRESENCE_LINKED.
 *   - FF_CREW_PRESENCE_HEARD / STALE  -> FF_PRESENCE_SEEN.
 *   - FF_CREW_PRESENCE_LOST           -> FF_PRESENCE_LOST.
 * `ff_crew_presence` has already applied the inclusive-toward-STALE
 * boundary convention, so this is a direct passthrough, not a second
 * threshold comparison — exactly one place decides where the boundary
 * sits (core/ff_crew.h's FF_CREW_HEARD_LIVE_MS/FF_CREW_HEARD_LOST_MS).
 */
ff_sigview_presence_t ff_sigview_presence(ff_crew_presence_t heard, uint32_t heard_age_ms, uint32_t *out_age_ms);

#ifdef __cplusplus
}
#endif

#endif /* FF_SIGVIEW_H */
