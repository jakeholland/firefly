/**
 * ff_nodeinfo_req.h — core/nodeinfo_req: the rate-limit state behind
 * "ask a nameless crew member for their NodeInfo".
 *
 * Bench finding, 2026-09-14: a node admitted to the crew on a
 * TEXT_MESSAGE_APP or POSITION_APP packet (docs/specs/A02-crew-join.md
 * §4.1 / S02-core-crew.md's 2026-09-13 amendment §B — any of NodeInfo,
 * Position, Text or `FF_PORTNUM` admits) stays "New crew member"/`NAME?`
 * until its OWN radio gets around to its next periodic NodeInfo
 * broadcast — Meshtastic's stock interval is on the order of hours. §4.4
 * already specifies the honest placeholder for that wait; this module is
 * the fix for the wait itself: as soon as a nameless member is admitted,
 * ask their radio directly (`NODEINFO_APP`, `want_response = true` —
 * `NodeInfoModule::allocReply`, verified against `meshtastic/firmware`
 * tag `v2.7.26.54e0d8d`, replies with the local node's own `User` to any
 * NODEINFO_APP packet that isn't from itself and carries the bit,
 * independent of the periodic broadcast timer).
 *
 * ## What lives here, and what does not
 *
 * This module owns exactly one decision — "is it OK to send another
 * NodeInfo request to this node right now" — and the bounded, per-node
 * memory that decision needs. It does not send anything, does not know
 * about `ff_crew_t`, `mc_client_t`, or whether a member has a name; the
 * caller (`app/ff_shell.c`'s `shell_try_admit`, and the app's
 * `CrewMembershipEngine`) checks "nameless" itself and calls this only
 * when it already means to ask. Keeping the throttle pure and separate
 * from that decision is what makes it independently unit-testable
 * (AGENTS.md: "measure, not reasoning harder") without a live crew
 * roster or radio in the test.
 *
 * ## Why a rate limit at all
 *
 * A single admission asks once, by construction — `shell_try_admit` is
 * only reached while a sender is not yet a PAIRED roster member, so an
 * ordinary admission fires this exactly once. The throttle is the safety
 * net for the abnormal case: a node that is admitted, un-admitted (hide,
 * or unpaired-slot LRU eviction under §4.3's cap) and re-admitted several
 * times in a short window — e.g. someone toggling hide/unhide, or a busy
 * roster churning strangers in and out of the one remaining unpaired
 * slot. Without a limit, each cycle would fire another over-the-air
 * request at a node that may not have answered the first one yet. Ten
 * minutes was picked to sit comfortably UNDER the ~3h stock NodeInfo
 * interval this feature exists to shortcut, while still being long
 * enough that a legitimate reply (or its absence) has had time to show
 * up before asking again.
 *
 * ## Bounded, not a hash map
 *
 * Sized to `FF_CREW_MAX` (core/include/ff_crew.h): the only callers that
 * ever ask this module for a decision are asking about a node that was
 * JUST admitted to the crew roster, so the number of distinct node ids
 * this module needs to remember at once can never exceed the roster's
 * own cap. A full table (which by that reasoning only happens under
 * roster churn, never steady state) evicts its least-recently-requested
 * entry — same LRU-by-timestamp policy `ff_heard.h` already established
 * for the same reason, wraparound-safe the same way.
 *
 * Pure C11, no I/O, zero heap allocation.
 */
#ifndef FF_NODEINFO_REQ_H
#define FF_NODEINFO_REQ_H

#include <stdbool.h>
#include <stdint.h>

#include "ff_crew.h" /* FF_CREW_MAX — see header comment on sizing */

#ifdef __cplusplus
extern "C" {
#endif

/** Minimum gap between two NodeInfo requests to the SAME node id. */
#define FF_NODEINFO_REQ_RATE_LIMIT_MS ((uint32_t)10u * 60u * 1000u) /* 10 min */

/** How many distinct node ids this module tracks a last-request time
 * for at once. See header comment — bounded by the crew roster's own
 * cap, never a hash map. */
#define FF_NODEINFO_REQ_MAX FF_CREW_MAX

typedef struct {
    uint32_t node_id;
    uint32_t last_sent_ms;
} ff_nodeinfo_req_entry_t;

typedef struct {
    ff_nodeinfo_req_entry_t entries[FF_NODEINFO_REQ_MAX];
    uint8_t                 count;
} ff_nodeinfo_req_t;

/** Reset to empty. NULL-safe. */
void ff_nodeinfo_req_init(ff_nodeinfo_req_t *r);

/**
 * ff_nodeinfo_req_should_send — the one decision this module makes.
 *
 * Returns true iff a NodeInfo request to `node_id` is due at `now_ms`:
 * no request has ever been recorded for it, or the last one was at
 * least `FF_NODEINFO_REQ_RATE_LIMIT_MS` ago (unsigned-subtraction age,
 * wraparound-safe past ~49.7 days like every other clock comparison in
 * this tree). **On a true return, the send is recorded immediately** —
 * this call both answers the question and marks the attempt, so the
 * caller does not need (and must not add) a second bookkeeping call.
 * That means a call that decides to send counts as having sent even if
 * the caller's actual radio call then fails — deliberate: the failure
 * mode of "we asked, got no reply, and won't ask again for ten minutes"
 * is a small, bounded, self-healing wait; the failure mode of retrying
 * a send that is failing for a structural reason (link down, sender not
 * wired up) on every qualifying packet is the one this throttle exists
 * to prevent.
 *
 * A false return touches no state. `node_id == 0` (never a valid
 * Meshtastic node id — the wire protocol reserves it as "unset") and a
 * NULL `r` both return false and touch nothing.
 *
 * A brand-new `node_id` with the table already full evicts the entry
 * with the OLDEST `last_sent_ms` (greatest age), exactly like
 * `ff_heard_note` (ff_heard.c) — see this header's own comment on why
 * that can only happen under roster churn, never steady state.
 */
bool ff_nodeinfo_req_should_send(ff_nodeinfo_req_t *r, uint32_t node_id, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* FF_NODEINFO_REQ_H */
