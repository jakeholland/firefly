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

    /// The crew roster — slice B's `Bridge/CrewStore.swift`, heap-owning
    /// one `ff_crew_t`. Confined to this `@MainActor` instance, per the
    /// threading model: the C core has no locks, by design.
    public let crew = CrewStore()
    /// The event feed — slice B's `Bridge/InboxBridge.swift`, heap-owning
    /// one `ff_feed_t`. Same confinement rule as `crew`.
    public let inbox = InboxBridge()

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

    /// Routes a node snapshot into `ff_crew_on_position` /
    /// `ff_crew_on_rssi` / `ff_crew_on_heard` through
    /// `Bridge/CrewStore.swift`. Never fabricates: a field the snapshot
    /// doesn't carry (no position, no direct RSSI) simply isn't fed —
    /// there is no synthesized fallback for any of the three.
    public func apply(nodeUpdate: MeshNodeSnapshot) {
        let now = FireflyClock.nowMillis()

        if let position = nodeUpdate.position {
            let rxTime = position.time.map(FireflyClock.millis(since:)) ?? now
            let meta = CrewStore.PositionMeta(asserted: position.source == .manual,
                                               precisionBits: position.precisionBits)
            crew.onPosition(nodeID: nodeUpdate.num, latitude: position.latitude, longitude: position.longitude,
                             rxTimeMs: rxTime, meta: meta)
        }

        // RSSI/SNR are per-packet and only attributable when the packet
        // came directly (docs/specs/A01-companion-app.md, "NodeDB"): a
        // bare `hopsAway == 0` is what the client layer (slice A) uses
        // to mean DIRECT, never a default for "unknown".
        let direct = nodeUpdate.hopsAway == 0
        if direct, let rssiDbm = nodeUpdate.rssiDbm {
            crew.onRSSI(nodeID: nodeUpdate.num, rssiDbm: rssiDbm)
        }

        let heardAt = nodeUpdate.lastHeard.map(FireflyClock.millis(since:)) ?? now
        crew.onHeard(nodeID: nodeUpdate.num, rxTimeMs: heardAt, direct: direct)
    }

    /// Routes a delivery-state transition into
    /// `ff_feed_set_send_status_by_outbox_id` through
    /// `Bridge/InboxBridge.swift`. `DeliveryState` (FireflyMesh) and
    /// `FeedSendStatus` (this module's Bridge/* vocabulary, which never
    /// imports FireflyMesh — see InboxBridge.swift's top comment) meet
    /// exactly here, the one seam that legitimately depends on both.
    public func apply(delivery: (packetID: UInt32, state: DeliveryState)) {
        let status: FeedSendStatus
        switch delivery.state {
        case .waiting: status = .waiting
        case .sent: status = .sent
        case .delivered: status = .delivered
        case .noAck: status = .noAck
        case .dropped: status = .dropped
        }
        inbox.setSendStatus(outboxID: delivery.packetID, status: status, atMs: FireflyClock.nowMillis())
    }
}
