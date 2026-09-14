//
//  CrewNodeInfoRequestThrottle.swift — the rate-limit state behind
//  "ask a nameless crew member for their NodeInfo" (bench finding
//  2026-09-14, `docs/specs/A02-crew-join.md` §4.4): a node admitted to
//  the crew on a TEXT/POSITION/`FF_PORTNUM` packet stays "New crew
//  member"/`NAME?` until its own radio's next periodic NodeInfo
//  broadcast — Meshtastic's stock interval is on the order of hours.
//  `CrewMembershipEngine` asks directly instead, right after admitting
//  a nameless member; this type is the ONE decision behind that ask —
//  "is it OK to request another NodeInfo from this node right now" —
//  kept pure and separate so it is independently testable
//  (AGENTS.md: "measure, not reasoning harder") with no roster, no
//  client and no clock beyond a `Date` the caller supplies.
//
//  Swift mirror of firmware's `ff_nodeinfo_req_t`/
//  `ff_nodeinfo_req_should_send` (`firmware/core/include/
//  ff_nodeinfo_req.h`), same rate and same "recorded on send, not
//  reasoned about twice" contract, deliberately UNBOUNDED here rather
//  than the firmware's `FF_CREW_MAX`-sized LRU table: the C struct is
//  bounded because it is a fixed-size embedded array with zero heap
//  allocation, and the number of distinct node ids this type is ever
//  asked about is already bounded by the SAME fact that bounds the
//  firmware table (only a just-admitted crew member is ever passed
//  here, and the crew roster itself is capped at `FF_CREW_MAX` — A02
//  §4.3). A `Dictionary` keyed by that same small population is not a
//  growth risk worth a second eviction policy to reason about.
//
//  WHY A RATE LIMIT AT ALL. A single admission asks once, by
//  construction — `CrewMembershipEngine.admit(_:)` only runs on a
//  genuinely NEW pairing. The limit is the safety net for the abnormal
//  case: a node admitted, un-admitted (hide, or the unpaired-slot LRU
//  eviction under §4.3's cap) and re-admitted several times in a short
//  window. Ten minutes was picked to sit comfortably under the ~3h
//  stock NodeInfo interval this feature exists to shortcut, while
//  still being long enough that a legitimate reply (or its absence)
//  has had time to show up before asking again — same reasoning as the
//  firmware header's own doc comment.
//
import Foundation

/// A pure, mutable value type: "have we asked this node id for its
/// NodeInfo recently enough that asking again would be noise." Nothing
/// here knows about `ff_crew`, a client, or whether a member has a
/// name — the caller (`CrewMembershipEngine`) decides "nameless" and
/// calls `shouldSend(nodeID:now:)` only when it already means to ask.
public struct CrewNodeInfoRequestThrottle: Sendable, Equatable {
    /// Minimum gap between two NodeInfo requests to the SAME node id —
    /// `FF_NODEINFO_REQ_RATE_LIMIT_MS` on the puck, kept as a
    /// `TimeInterval` here since every clock in this module is a
    /// `Date`.
    public static let rateLimit: TimeInterval = 10 * 60 // 10 minutes

    private var lastSentAt: [UInt32: Date] = [:]

    public init() {}

    /// Returns true iff a NodeInfo request to `nodeID` is due at `now`:
    /// no request has ever been recorded for it, or the last one was at
    /// least `Self.rateLimit` ago. **On a true return, the attempt is
    /// recorded immediately** — this call both answers the question and
    /// marks it, so the caller does not need (and must not add) a
    /// second bookkeeping call. A call that decides to send counts as
    /// having sent even if the caller's actual radio call then fails —
    /// deliberate, same tradeoff `ff_nodeinfo_req_should_send`'s own doc
    /// comment states: the failure mode of "we asked, got no reply, and
    /// won't ask again for ten minutes" is small and self-healing; the
    /// failure mode of retrying a send that is failing for a structural
    /// reason on every qualifying packet is the one this throttle
    /// exists to prevent.
    ///
    /// `nodeID == 0` (never a valid Meshtastic node id — the wire
    /// protocol reserves it as "unset") returns false and records
    /// nothing.
    public mutating func shouldSend(nodeID: UInt32, now: Date) -> Bool {
        guard nodeID != 0 else { return false }
        if let last = lastSentAt[nodeID], now.timeIntervalSince(last) < Self.rateLimit {
            return false
        }
        lastSentAt[nodeID] = now
        return true
    }
}
