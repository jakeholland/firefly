//
//  NearbyNodesViewModel.swift — the Connect screen's "Nearby" section:
//  paired crew (colour + presence) first, then strangers ranked by
//  signal TIER, never by an invented distance (docs/specs/
//  A01-companion-app.md, Design language; M2's "crew pairing and
//  colours, driven by `ff_crew`").
//
//  A fresh, independent `nodeUpdates()` subscription (S1's multicast
//  rule): `CoreStore` and Radar (slice D) each hold their own too, and
//  this one is the Connect screen's, so falling behind on one never
//  starves another.
//
//  M2 replaces the old session-only `Set<UInt32>` toggle with the real
//  thing: every add/remove goes through `CrewPairingController`, which
//  writes `ff_crew` AND the persisted `CrewPairingStoring` record
//  together — so a paired member here is the SAME paired member Radar's
//  ring, the Inbox and the Crew section in More all read, and it
//  survives a relaunch.
//
import FireflyMesh
import FireflyModel
import Foundation
import Observation

@MainActor
@Observable
final class NearbyNodesViewModel {
    struct NearbyNode: Identifiable, Equatable {
        let id: UInt32
        let displayName: String
        let tier: SignalTierPresentation
        let isCrew: Bool
        /// `ff_crew_member_t.color_idx`, for a paired row only — `nil`
        /// for a stranger, who has no crew colour to render (never a
        /// guessed one).
        let colorIndex: Int?
        /// The HEARD-presence axis (`ff_crew_presence`), for a paired
        /// row only — a stranger's presence is the tier bars already
        /// showing, not a second, redundant tag.
        let presence: PresenceTag?
    }

    private(set) var nodes: [NearbyNode] = []
    /// Set when `addToCrew(_:)` hits `CrewPairingController
    /// .maxCrewSize` — the honest limit message (M2's own acceptance
    /// criterion: "the Add action explains the limit instead of failing
    /// silently"), shown by the Connect screen and cleared on the next
    /// successful pairing action.
    private(set) var limitMessage: String?

    private let client: any MeshtasticClientProtocol
    private let pairing: CrewPairingController
    private var observation: Task<Void, Never>?
    private var byNum: [UInt32: MeshNodeSnapshot] = [:]

    init(client: any MeshtasticClientProtocol, pairing: CrewPairingController) {
        self.client = client
        self.pairing = pairing
        rebuild()
    }

    /// Idempotent, matching `ConnectViewModel.observe()`'s shape — the
    /// stream is captured HERE, before the `Task` that drains it, so
    /// the subscription is live before anything can publish into it.
    func observe() {
        guard observation == nil else { return }
        let stream = client.nodeUpdates()
        observation = Task { [weak self] in
            for await snapshot in stream {
                guard let self else { return }
                self.apply(snapshot)
            }
        }
    }

    /// Not a `deinit` — this type is `@MainActor`, same rule as
    /// `ConnectViewModel.stopObserving()`.
    func stopObserving() {
        observation?.cancel()
        observation = nil
    }

    func apply(_ snapshot: MeshNodeSnapshot) {
        byNum[snapshot.num] = snapshot
        rebuild()
    }

    /// Pairs `num` through the real seam: `ff_crew_set_paired` plus a
    /// persisted record, with a freshly assigned colour (first free
    /// index in roster order). Sets `limitMessage` — never fails
    /// silently — when the crew is already at
    /// `CrewPairingController.maxCrewSize`.
    func addToCrew(_ num: UInt32) {
        switch pairing.pair(nodeID: num) {
        case .paired:
            limitMessage = nil
        case .full(let limit):
            limitMessage = "Crew is full (\(limit)/\(limit) paired). Remove someone in More \u{2192} Crew to add another."
        }
        rebuild()
    }

    func removeFromCrew(_ num: UInt32) {
        pairing.unpair(nodeID: num)
        limitMessage = nil
        rebuild()
    }

    private func rebuild() {
        let now = FireflyClock.nowMillis()
        let rosterOrder = pairing.pairedRecords().map(\.nodeID)

        var crewRows: [NearbyNode] = []
        var strangerRows: [NearbyNode] = []
        for snapshot in byNum.values {
            guard let rssi = snapshot.rssiDbm else { continue }
            let member = pairing.crew.member(nodeID: snapshot.num, now: now)
            let paired = member?.paired ?? false
            let node = NearbyNode(
                id: snapshot.num,
                displayName: Self.displayName(for: snapshot),
                tier: SignalTierPresentation.tier(rssiDbm: rssi),
                isCrew: paired,
                colorIndex: paired ? Int(member?.colorIndex ?? 0) : nil,
                presence: paired ? Self.presenceTag(for: member?.heardPresence) : nil)
            if paired { crewRows.append(node) } else { strangerRows.append(node) }
        }
        // Paired members: roster order (first paired, first shown) —
        // the same canonical ordering the Crew section in More and
        // `CrewPairingRestorer` use, never re-sorted by a transient
        // signal reading. Strangers: strongest signal first, as before.
        crewRows.sort { (rosterOrder.firstIndex(of: $0.id) ?? .max) < (rosterOrder.firstIndex(of: $1.id) ?? .max) }
        strangerRows.sort { $0.tier.barFill > $1.tier.barFill }
        nodes = crewRows + strangerRows
    }

    private static func presenceTag(for heard: HeardPresence?) -> PresenceTag {
        switch heard {
        case .heard: return .heard
        case .stale: return .stale
        case .lost: return .lost
        case .never, .none: return .linked
        }
    }

    private static func displayName(for snapshot: MeshNodeSnapshot) -> String {
        snapshot.shortName ?? snapshot.longName ?? String(format: "!%08x", snapshot.num)
    }
}
