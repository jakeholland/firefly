//
//  CoreStore.swift — the single @MainActor owner of the puck's C
//  contexts, and the seam the whole data flow routes through
//  (docs/specs/A01-companion-app.md, "Data flow", "Threading model",
//  and S5).
//
//  Landed here as a SKELETON so slice B (which owns `Bridge/*`) is not
//  the only place this type could be invented, and so slices D and E
//  can depend on it existing from day one instead of each inventing
//  their own. What is real here: the ONE @MainActor isolation domain
//  the threading model requires for every `ff_*` context, and the
//  subscription shape (three independent `EventHub` subscriptions, S1 —
//  proof that a view model and `CoreStore` can observe the SAME client
//  without stealing each other's events; see `CoreStoreTests`).
//
//  What is NOT here yet, on purpose: no `ff_crew_t` / `ff_feed_t` /
//  `ff_find_t` pointer. That heap allocation, its `ff_*_init` call and
//  its `deinit` teardown belong to slice B's `Bridge/CrewStore.swift`
//  (and friends), per the memory-ownership rules in the spec's "C-core
//  bridge" section — this file only owns the seam, not the bridge.
//
import FireflyMesh
import Foundation

@MainActor
public final class CoreStore {
    /// Mirrors the client's own link state. Slice C's Diagnostics screen
    /// reads this independently of `ConnectViewModel` — the whole reason
    /// this needs its own `EventHub` subscription rather than sharing
    /// the view model's.
    public private(set) var linkState: LinkState = .disconnected

    private var linkObservation: Task<Void, Never>?
    private var nodeObservation: Task<Void, Never>?
    private var deliveryObservation: Task<Void, Never>?

    public init() {}

    /// Subscribe to a client's three streams. Idempotent, like
    /// `ConnectViewModel.observe()` — and safe to call on the SAME
    /// client a view model is already observing, because each of
    /// `linkState()` / `nodeUpdates()` / `deliveryUpdates()` hands back
    /// an independent `EventHub` subscription (S1). Each stream is
    /// captured HERE, synchronously, before its `Task` is created — see
    /// `ConnectViewModel.observe()`'s comment for why that ordering
    /// matters.
    public func observe(client: any MeshtasticClientProtocol) {
        guard linkObservation == nil else { return }

        let links = client.linkState()
        let nodes = client.nodeUpdates()
        let deliveries = client.deliveryUpdates()

        linkObservation = Task { [weak self] in
            for await state in links {
                guard let self else { return }
                self.linkState = state
            }
        }
        nodeObservation = Task { [weak self] in
            for await snapshot in nodes {
                guard let self else { return }
                self.apply(nodeUpdate: snapshot)
            }
        }
        deliveryObservation = Task { [weak self] in
            for await (packetID, state) in deliveries {
                guard let self else { return }
                self.apply(delivery: (packetID, state))
            }
        }
    }

    /// Not a `deinit`: this type is `@MainActor`, same rule as
    /// `ConnectViewModel.stopObserving()`.
    public func stopObserving() {
        linkObservation?.cancel(); linkObservation = nil
        nodeObservation?.cancel(); nodeObservation = nil
        deliveryObservation?.cancel(); deliveryObservation = nil
    }

    /// Slice B fills this in: route into `ff_crew_on_position` /
    /// `ff_crew_on_rssi` / `ff_crew_on_heard` through
    /// `Bridge/CrewStore.swift`. A no-op here on purpose.
    public func apply(nodeUpdate: MeshNodeSnapshot) {}

    /// Slice B/E fills this in: `ff_feed_set_send_status_by_outbox_id`
    /// through `Bridge/InboxBridge.swift`.
    public func apply(delivery: (packetID: UInt32, state: DeliveryState)) {}
}
