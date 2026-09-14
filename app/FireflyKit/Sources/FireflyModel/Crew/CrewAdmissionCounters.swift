//
//  CrewAdmissionCounters.swift — A02 slice E's "Crew diagnostics" row
//  (`docs/specs/A02-crew-join.md` §6.5): plain counts of what
//  `CrewMembershipEngine.admits(_:)` has decided since this process
//  started, broken out by the SAME clauses `tryAdmit` already gates on
//  (§4.1). Read-only bookkeeping, never a second opinion: nothing here
//  feeds back into whether a node is admitted, and nothing here is
//  persisted — a relaunch (or a demo reset) starts every count at
//  zero, honestly, because that IS when the counting restarted.
//
//  Why NOT one flat "refused" total kept incrementally: §6.5 asks for
//  "admitted / refused BY REASON", and a diagnostics row that could only
//  ever say "3 refused" with no reason attached would be exactly the
//  kind of number this repo's honesty rule warns against — technically
//  true, useless for telling a mis-set channel apart from a hidden
//  crewmate. `refusedTotal` is derived, never stored twice.
//
public struct CrewAdmissionCounters: Sendable, Equatable {
    /// A NEW node was let onto the crew — `CrewMembershipEngine.admit`'s
    /// `.paired` case. Deliberately NOT incremented for a packet from a
    /// member who is already crew (`admits(_:)`'s fast `isCrew` path):
    /// that would count ordinary traffic, not admissions, and the
    /// diagnostics row asks for "admitted" in the everyday sense —
    /// how many people this session let in.
    public var admitted: Int = 0
    /// Clause 1 — arrived on a node that is not us and not already
    /// crew, but with no decrypted rx metadata (a nodeDB replay, or a
    /// snapshot this radio never actually heard on the crew channel).
    public var refusedNotDecrypted: Int = 0
    /// Clause 2 — the crew channel's index on THIS radio is not yet
    /// known (`.noCrew`/`.resolving`), so no comparison could be made
    /// at all. Kept separate from `refusedWrongChannel` because the fix
    /// is different: "wait" versus "this genuinely arrived elsewhere".
    public var refusedChannelNotResolved: Int = 0
    /// Clause 2 — the channel index WAS known, and this packet's index
    /// simply was not it.
    public var refusedWrongChannel: Int = 0
    /// Clause 3 — the packet's sender is this radio itself, or this
    /// radio does not yet know its own node number.
    public var refusedSelfOrUnknownRadio: Int = 0
    /// Clause 5 — arrived over MQTT, which a crew (people who are HERE)
    /// deliberately never trusts (§4.1's own threat-model note).
    public var refusedViaMQTT: Int = 0
    /// Clause 6 — decrypted, on the right channel, from someone real,
    /// but not one of the four admitting portnums.
    public var refusedWrongPortnum: Int = 0
    /// The sender is on this phone's own hide list (§4.5) — refused
    /// before any of the clauses above are even consulted.
    public var refusedHidden: Int = 0
    /// Every clause passed, but `FF_CREW_MAX` was already full (§4.3) —
    /// the node is recorded honestly as "not tracked", never dropped
    /// silently.
    public var refusedRosterFull: Int = 0

    public init() {}

    /// The diagnostics row's one headline "refused" figure — every
    /// clause added together, computed rather than tracked as an
    /// independent counter so it can never drift out of sync with the
    /// per-reason breakdown above.
    public var refusedTotal: Int {
        refusedNotDecrypted + refusedChannelNotResolved + refusedWrongChannel + refusedSelfOrUnknownRadio
            + refusedViaMQTT + refusedWrongPortnum + refusedHidden + refusedRosterFull
    }
}
