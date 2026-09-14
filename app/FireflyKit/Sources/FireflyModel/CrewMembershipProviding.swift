//
//  CrewMembershipProviding.swift — the seam the Start/Crew screens read
//  the joined-member list through (`docs/specs/A02-crew-join.md`, §2.3,
//  §5). Slice C implements the real "admitted since `crewCreatedAt`,
//  newest first" rule (§2.3's `CrewJoinWatcher`) against live
//  auto-membership packets; THIS slice ships a stub that reports
//  whoever `CrewPairingController` already has paired, so the Start
//  "code" screen and the Crew page render something real today instead
//  of an empty placeholder — never fabricated names or join times
//  (CLAUDE.md's honest-data rule).
//
import FireflyCore
import Foundation

/// One row in the Joined / People list — deliberately narrower than
/// `CrewMember` (`Bridge/CrewStore.swift`): this is what BOTH the Start
/// screen's "Joined · N" list and the Crew page's People list need, not
/// every freshness/position field `CrewMember` carries.
public struct CrewJoinedMember: Sendable, Equatable, Identifiable {
    public let id: UInt32
    /// nil = no NodeInfo yet — renders as "New crew member" (§4.4),
    /// never a blank row. Never guessed.
    public let displayName: String?
    public let colorIndex: UInt8
    /// nil = this phone never observed this member join (e.g. restored
    /// from persistence at launch, before any packet — §4.4's "waiting
    /// to hear from them"). Epoch milliseconds when known.
    public let joinedAtMs: UInt64?
    public let heardPresence: HeardPresence

    public init(id: UInt32, displayName: String?, colorIndex: UInt8, joinedAtMs: UInt64?, heardPresence: HeardPresence) {
        self.id = id
        self.displayName = displayName
        self.colorIndex = colorIndex
        self.joinedAtMs = joinedAtMs
        self.heardPresence = heardPresence
    }
}

/// Read-only — nothing on the Start/Crew screens writes membership
/// through this seam; joining/hiding/leaving go through
/// `CrewController`/`CrewPairingController` instead.
@MainActor
public protocol CrewMembershipProviding: AnyObject {
    /// Every current, non-hidden crew member. Order is the provider's
    /// choice — slice C's real implementation sorts newest-first
    /// (§2.3); this stub does not promise an order at all.
    func currentMembers() -> [CrewJoinedMember]
}

/// The stub every Start/Crew screen composition uses until slice C
/// lands: reports `CrewPairingController`'s own paired roster, with NO
/// join-time (there is no "admitted since crewCreatedAt" watcher yet —
/// `joinedAtMs` is honestly `nil` for every row, which renders as
/// "waiting to hear from them" per §4.4, not a fabricated "joined just
/// now").
@MainActor
public final class PairingCrewMembershipProvider: CrewMembershipProviding {
    private let pairing: CrewPairingController
    private let now: () -> UInt32

    public init(pairing: CrewPairingController, now: @escaping () -> UInt32 = FireflyClock.nowMillis) {
        self.pairing = pairing
        self.now = now
    }

    public func currentMembers() -> [CrewJoinedMember] {
        let nowMs = now()
        return pairing.pairedRecords().map { record in
            let member = pairing.crew.member(nodeID: record.nodeID, now: nowMs)
            return CrewJoinedMember(
                id: record.nodeID,
                displayName: (member?.displayName.isEmpty ?? true) ? nil : member?.displayName,
                colorIndex: record.colorIndex,
                joinedAtMs: nil,
                heardPresence: member?.heardPresence ?? .never)
        }
    }
}
