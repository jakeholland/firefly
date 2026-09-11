//
//  NearbyNodesViewModel.swift — the Connect screen's "Nearby" section:
//  heard mesh nodes ranked by signal TIER, never by an invented
//  distance (docs/specs/A01-companion-app.md, Design language).
//
//  A fresh, independent `nodeUpdates()` subscription (S1's multicast
//  rule): `CoreStore` and Radar (slice D) each hold their own too, and
//  this one is the Connect screen's, so falling behind on one never
//  starves another.
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
    }

    private(set) var nodes: [NearbyNode] = []

    private let client: any MeshtasticClientProtocol
    private var observation: Task<Void, Never>?
    private var byNum: [UInt32: MeshNodeSnapshot] = [:]
    /// Session-only. See `toggleCrew(_:)`.
    private var crewNumbers: Set<UInt32> = []

    init(client: any MeshtasticClientProtocol) {
        self.client = client
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

    /// LOCAL AND SESSION-ONLY for M1. Crew MEMBERSHIP is `ff_crew`'s
    /// concept, bound through slice B's `Bridge/CrewStore.swift` — and
    /// `CoreStore.apply(nodeUpdate:)`, the seam that would route a real
    /// add into `ff_crew_on_*`, is still a declared no-op (see
    /// `CoreStore.swift`'s own comment). Toggling this changes only
    /// what THIS SCREEN shows, in THIS launch; it does not persist and
    /// it does not make the node a crew member anywhere the core
    /// knows about. Rendering it as forgettable is the honest answer
    /// until that bridge lands — inventing a fake persistence here
    /// would be the same failure a fabricated position is.
    func toggleCrew(_ num: UInt32) {
        if crewNumbers.contains(num) {
            crewNumbers.remove(num)
        } else {
            crewNumbers.insert(num)
        }
        rebuild()
    }

    private func rebuild() {
        nodes = byNum.values
            .compactMap { snapshot -> NearbyNode? in
                guard let rssi = snapshot.rssiDbm else { return nil }
                return NearbyNode(
                    id: snapshot.num,
                    displayName: Self.displayName(for: snapshot),
                    tier: SignalTierPresentation.tier(rssiDbm: rssi),
                    isCrew: crewNumbers.contains(snapshot.num))
            }
            .sorted { $0.tier.barFill > $1.tier.barFill }
    }

    private static func displayName(for snapshot: MeshNodeSnapshot) -> String {
        snapshot.shortName ?? snapshot.longName ?? String(format: "!%08x", snapshot.num)
    }
}
