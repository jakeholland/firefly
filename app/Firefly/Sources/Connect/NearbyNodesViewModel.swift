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

// MARK: - NEARBY RADIOS row building (Connect screen redesign)
//
// Owner feedback from the first real-radio run: "the connect button
// needs to be on the line item or something, screen needs a little
// work". `RadioListRow`/`RadioListBuilder` are the NEARBY RADIOS
// picker's own version of the pattern `NearbyNodesViewModel` above
// already uses for the NEARBY crew section: raw event/scan data goes in
// (`ConnectViewModel` + the picker's own `[DiscoveredPeripheral]`),
// display-ready rows with a per-row action come out, pure and testable
// without SwiftUI. Lives in this file rather than a new one — the
// owner's task grouped it with `NearbyNodesViewModel` as "the Connect
// view models" to touch.

/// One NEARBY RADIOS row, ready for `ConnectScreen` to render verbatim.
struct RadioListRow: Identifiable, Equatable {
    /// A row's status chip — CONNECTED (theme amber border, per the
    /// owner's design canvas), CONNECTING (still mid-handshake, but
    /// already abortable — `ConnectViewModel.rowAction`'s own doc
    /// comment on why DISCONNECT is reachable the whole time), or
    /// REMEMBERED (persisted for next launch, not currently linked).
    /// Every OTHER discovered peripheral carries `.none` — no chip at
    /// all, just CONNECT.
    enum Status: Equatable {
        case connected
        case connecting
        case remembered
        case none

        var chipText: String? {
            switch self {
            case .connected: return "CONNECTED"
            case .connecting: return "CONNECTING…"
            case .remembered: return "REMEMBERED"
            case .none: return nil
            }
        }
        /// The theme-amber border the owner's design canvas gives a
        /// node card while it is "the" radio — connected OR actively
        /// connecting to it, never merely remembered.
        var isHighlighted: Bool {
            self == .connected || self == .connecting
        }
    }

    let id: String
    /// The best name known for this radio — its own `NodeInfo` long
    /// name once want_config has reported one, else its BLE advertised
    /// name, else its raw node id, else (nothing at all is known — the
    /// remembered-but-never-scanned case) a plain placeholder. Never a
    /// bare UUID: `MeshPeripheralDiscovery`'s `id` is a `CBPeripheral`
    /// identifier, meaningless to a human.
    let title: String
    /// Whatever identity `title` did NOT already use, joined the same
    /// way `ConnectViewModel.headerStatusText` joins its own parts —
    /// nil when there is nothing left to add.
    let subtitle: String?
    let rssiDbm: Int?
    let status: Status
    /// Drives the FORGET row action — independent of `status`: a
    /// remembered radio that is currently CONNECTING is still the one
    /// FORGET should clear, not only while idle.
    let isRemembered: Bool
    let action: ConnectViewModel.RadioRowAction
}

@MainActor
enum RadioListBuilder {
    /// Builds the NEARBY RADIOS list: at most ONE synthetic "active
    /// radio" row for whichever peripheral `connect` currently
    /// considers remembered/connecting/connected — merged with a
    /// live scan match when one exists — followed by every OTHER
    /// scanned peripheral, each offering its own CONNECT.
    ///
    /// The active row is built from `connect`'s OWN state rather than
    /// only from `discovered`, on purpose: a real Meshtastic radio
    /// stops BLE-advertising once connected, so a scan can never
    /// re-find the very radio the app is already talking to — and the
    /// iOS Simulator's demo mode has no scanner at all
    /// (`PeripheralDiscovery.swift`'s `StubPeripheralDiscovery`). Both
    /// cases still need a row: "which radio am I on?" must never
    /// depend on the scan having anything in it right now.
    static func rows(discovered: [DiscoveredPeripheral], connect: ConnectViewModel) -> [RadioListRow] {
        var rows: [RadioListRow] = []
        var coveredIDs: Set<String> = []

        let remembered = connect.rememberedPeripheralID
        // Shown whenever there is something to say about "the" radio:
        // either the link is actively doing something with it, or it is
        // simply the one remembered from a previous session (even out
        // of the current scan — REMEMBERED must stay reachable, with
        // FORGET, regardless of whether a scan happens to be running).
        let showActiveRow = connect.link != .disconnected || remembered != nil
        if showActiveRow {
            let id = remembered ?? "connected-radio"
            let scanMatch = remembered.flatMap { rid in discovered.first(where: { $0.id == rid }) }
            let bleName = connect.connectedRadio?.bleName ?? scanMatch?.name
            let longName = connect.connectedRadio?.longName
            let nodeID = connect.connectedNodeIDHex
            let rssi = connect.connectedRadio?.rssiDbm ?? scanMatch?.rssiDbm

            // Same priority `ConnectViewModel.headerStatusText` uses —
            // the most specific identity known leads; everything else
            // known becomes the subtitle, never repeated as both.
            var identity: [String] = []
            if let longName { identity.append(longName) }
            if let bleName { identity.append(bleName) }
            if let nodeID { identity.append(nodeID) }
            let title = identity.first ?? "Remembered radio"
            let subtitleParts = identity.dropFirst()

            let isRemembered = remembered != nil
            let status: RadioListRow.Status
            switch connect.link {
            case .ready: status = .connected
            case .connecting, .handshaking, .reconnecting: status = .connecting
            case .disconnected, .failed: status = isRemembered ? .remembered : .none
            }

            rows.append(RadioListRow(
                id: id,
                title: title,
                subtitle: subtitleParts.isEmpty ? nil : subtitleParts.joined(separator: " · "),
                rssiDbm: rssi,
                status: status,
                isRemembered: isRemembered,
                action: connect.rowAction(isActivePeripheral: true)))
            coveredIDs.insert(id)
        }

        for peripheral in discovered where !coveredIDs.contains(peripheral.id) {
            rows.append(RadioListRow(
                id: peripheral.id,
                title: peripheral.name ?? peripheral.id,
                subtitle: nil,
                rssiDbm: peripheral.rssiDbm,
                status: .none,
                isRemembered: false,
                action: connect.rowAction(isActivePeripheral: false)))
        }
        return rows
    }
}
