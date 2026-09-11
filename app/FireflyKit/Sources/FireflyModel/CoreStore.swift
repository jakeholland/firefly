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
    /// Radar's smoothing state (`ff_radar_smooth_t`). Owned here, with
    /// the two contexts it computes against, so the whole app has ONE
    /// radar session rather than one per screen construction — the bug
    /// `RadarView`'s old no-argument `init()` had, which built a second
    /// `AppDependencies.current()` (and therefore a second client) every
    /// time the destination was shown.
    public let radar = RadarBridge()
    /// The FIND session (`ff_find_t`). Same one-owner rule: a second
    /// `ff_find_t` would be a second session, and S29 allows exactly one.
    public let find = FindBridge()

    private var linkObservation: Task<Void, Never>?
    private var nodeObservation: Task<Void, Never>?
    private var deliveryObservation: Task<Void, Never>?
    /// See `observe(client:routeDeliveriesToInbox:)`.
    private var routeDeliveriesToInbox = true

    public init() {}

    /// Subscribe to a client's three streams. Idempotent, like
    /// `ConnectViewModel.observe()` — and safe to call on the SAME
    /// client a view model is already observing, because each of
    /// `linkState()` / `nodeUpdates()` / `deliveryUpdates()` hands back
    /// an independent `EventHub` subscription (S1). Each stream is
    /// captured HERE, synchronously, before its `Task` is created — see
    /// `ConnectViewModel.observe()`'s comment for why that ordering
    /// matters.
    ///
    /// `routeDeliveriesToInbox` exists because two id spaces meet at
    /// this seam and only one of them can own the feed's outbox keys.
    /// `DeliveryEvent.waiting`/`.sent`/`.dropped` carry the CLIENT's
    /// `OutboxID` (minted inside `MeshtasticClient.sendText`), while the
    /// items in `inbox` were pushed with `ThreadViewModel`'s OWN
    /// `OutboxIDGenerator` ids — both count up from 1, in `UInt32` and
    /// `UInt64` respectively, and `ff_feed_item_t.outbox_id` is 32 bits.
    /// So routing both into the same `ff_feed_t` would let the client's
    /// outbox id 1 silently resolve `ThreadViewModel`'s message 1: the
    /// exact aliasing bug PR #261's review already caught once.
    ///
    /// In the live graph (`AppGraph`) the view-model path owns those
    /// keys — it is the path that actually pushed the items — so this is
    /// passed `false` there and the packet-id-keyed half
    /// (`.delivered`/`.noAck`, which `InboxViewModel`/`ThreadViewModel`
    /// forward to the same bridge) is the only delivery routing that
    /// runs. It defaults to `true` so a caller with no view models at
    /// all — `CoreStoreTests`, or any future headless consumer that owns
    /// both ends of the id space — keeps the full routing.
    public func observe(client: any MeshtasticClientProtocol, routeDeliveriesToInbox: Bool = true) {
        guard linkObservation == nil else { return }
        self.routeDeliveriesToInbox = routeDeliveriesToInbox

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
            for await event in deliveries {
                guard let self else { return }
                self.apply(delivery: event)
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
    ///
    /// KNOWN GAP (issue #273, filed from the PR #268 review's
    /// SHOULD-FIX #3): unlike the puck firmware (`ff_shell.c`'s
    /// `shell_ev_rx_meta`, which gates its `ff_crew_on_*` calls on
    /// `sender->paired` and tracks everyone else in the bounded,
    /// LRU-evictable `ff_heard_t` instead), this calls `crew.onPosition`/
    /// `crew.onRSSI`/`crew.onHeard` unconditionally for EVERY node
    /// snapshot — there is no Swift-side `ff_heard` bridge to route
    /// unpaired strangers to instead. `CrewStore`'s own bounded-LRU
    /// eviction (2026-09-11 S02 amendment, issue #266) keeps this from
    /// wedging pairing shut the way it could pre-#266, but a future
    /// pairing/Nearby UI built on `CrewStore.members(now:).filter { !
    /// $0.paired }` would still inherit the "shrinks to zero as you pair
    /// real friends" ceiling `ff_heard_t` exists on the firmware side to
    /// avoid — see #273 before building one.
    public func apply(nodeUpdate: MeshNodeSnapshot) {
        let now = FireflyClock.nowMillis()

        // Identity first: the roster slot has to exist and carry
        // whatever names the mesh actually reported before any of the
        // three event setters below renders against it. Nothing is
        // synthesized — a node the radio has told us nothing but a
        // number about keeps an empty name and a '\0' initial.
        if nodeUpdate.shortName != nil || nodeUpdate.longName != nil {
            crew.setIdentity(nodeID: nodeUpdate.num, shortName: nodeUpdate.shortName,
                              longName: nodeUpdate.longName)
        }

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

    /// Routes one `DeliveryEvent` (FireflyMesh) into `Bridge/
    /// InboxBridge.swift`'s three `ff_feed_*` setters, mirroring
    /// `firmware/app/ff_shell.c` (`shell_send_or_queue_text`,
    /// `shell_ev_routing_ack`) exactly rather than funnelling every
    /// transition through one call keyed on one id (PR #261 review,
    /// finding 1 — `DeliveryEvent`'s own doc comment has the full
    /// per-case rationale):
    ///  - `.waiting`/`.dropped` -> `setSendStatus(outboxID:)` — no packet
    ///    id exists (or ever will) for either.
    ///  - `.sent` -> `markSent(outboxID:packetID:wantAck:)` — the ONLY
    ///    place `packetID`/`wantAck` get stamped onto the item, so a
    ///    later `setAck`/`tick(nowMs:)` can find it again.
    ///  - `.delivered`/`.noAck` -> `setAck(packetID:ok:)` — keyed on
    ///    `packetID` alone, gated by the C library's own SENT+want_ack
    ///    precondition, never `outboxID`.
    /// `FireflyMesh.{OutboxID,PacketID}` and this module's own
    /// `Bridge.{OutboxID,PacketID}` are deliberately separate types
    /// (InboxBridge.swift's top comment: Bridge/* never imports
    /// FireflyMesh) — this is the one seam that legitimately depends on
    /// both, so the unwrap-and-rewrap happens only here.
    public func apply(delivery: DeliveryEvent) {
        // `FireflyModel.` qualification below is required, not
        // decorative: `FireflyMesh.OutboxID`/`PacketID` (this switch's
        // own case payloads) and this module's OWN `OutboxID`/`PacketID`
        // (InboxBridge.swift, same module as this file) share bare
        // names, so an unqualified `OutboxID(...)`/`PacketID(...)` here
        // would be an ambiguous-type-name compile error, not a silent
        // pick of the wrong one — the two vocabularies really do meet
        // only at this one seam.
        guard routeDeliveriesToInbox else { return }
        let now = FireflyClock.nowMillis()
        switch delivery {
        case .waiting(let outboxID):
            inbox.setSendStatus(outboxID: FireflyModel.OutboxID(outboxID.rawValue), status: .waiting, atMs: now)
        case .sent(let outboxID, let packetID, let wantAck):
            inbox.markSent(outboxID: FireflyModel.OutboxID(outboxID.rawValue),
                            packetID: FireflyModel.PacketID(packetID.rawValue), wantAck: wantAck, atMs: now)
        case .delivered(let packetID):
            inbox.setAck(packetID: FireflyModel.PacketID(packetID.rawValue), ok: true, atMs: now)
        case .noAck(let packetID):
            inbox.setAck(packetID: FireflyModel.PacketID(packetID.rawValue), ok: false, atMs: now)
        case .dropped(let outboxID):
            inbox.setSendStatus(outboxID: FireflyModel.OutboxID(outboxID.rawValue), status: .dropped, atMs: now)
        }
    }

    /// The ACK-TIMEOUT half of NO_ACK (`ff_feed_expire_pending_acks`) —
    /// a tick-driven sweep with no per-message key, exactly as
    /// `ff_shell_tick` runs it every tick regardless of link state
    /// (ff_shell.c's own comment on that call site). Not folded into
    /// `apply(delivery:)`: this has no `DeliveryEvent` to arrive on: no
    /// event ever tells this app "45 seconds have now passed", so the
    /// caller (the app's own tick/heartbeat loop) must call this
    /// directly, the same way `ff_shell_tick` calls the C function.
    public func tick(nowMs: UInt32) {
        inbox.expirePendingAcks(now: nowMs, timeoutMs: CoreStore.outboxAckTimeoutMs)
    }

    /// Mirrors `FF_OUTBOX_ACK_TIMEOUT_MS` (firmware/app/include/ff_shell.h)
    /// — that header is app-layer (`firmware/app`), not part of
    /// `firmware/core` this package symlinks in, so the value is
    /// duplicated here rather than imported. Keep the two in sync by
    /// hand if that constant ever changes.
    public static let outboxAckTimeoutMs: UInt32 = 45_000
}
