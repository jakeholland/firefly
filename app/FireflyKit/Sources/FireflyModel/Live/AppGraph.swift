//
//  AppGraph.swift — the live object graph: ONE client, ONE set of C
//  contexts, ONE GPS uplink, and the view models built on top of them.
//
//  `AppDependencies` (S5) answers "which implementations"; this answers
//  "how many, owned by whom, subscribed when". Both halves are the
//  composition root A01's "Dependency injection" section describes —
//  constructor injection, one composition root, no service locator and
//  no singletons — and keeping them as two types is what lets a test
//  hand the graph a stub `AppDependencies` and get a real graph over it.
//
//  Why this type has to exist at all: before it, `FireflyApp.init` and
//  `RadarView.init()` EACH called `AppDependencies.current()`, so
//  showing the Radar destination built a second `MeshtasticClient` over
//  a second `BLETransport` — two radios' worth of state for one radio,
//  with the screen observing the one that was never connected. A graph
//  constructed once and handed down is the fix, and the reason nothing
//  below this line may call `.current()` for itself.
//
import FireflyMesh
import Foundation

/// The live graph. `@MainActor` because it owns the `ff_*` contexts
/// (through `CoreStore`) and every view model, all of which live in
/// that one isolation domain per A01's threading model.
@MainActor
public final class AppGraph {
    public let dependencies: AppDependencies
    /// The single owner of every `ff_*` context in the process.
    public let core = CoreStore()
    /// `InboxProviding`, backed by the C core — never
    /// `InMemoryInboxStore`, which is test-only from here on.
    public let inboxProvider: CoreInboxProvider
    /// Portnum 269 (FLARE, and FIND's PING).
    public let packetSender: MeshFireflyPacketSender
    private let uplink: PhoneGPSUplink

    private var privateObservation: Task<Void, Never>?
    private var tickLoop: Task<Void, Never>?
    private var started = false
    /// Handed a decoded PONG so FIND's replies list and haptics update.
    /// Set when `makeRadarViewModel` builds one; nil before that, which
    /// is why a PONG arriving with no Radar on screen is dropped rather
    /// than queued — S29's FIND is a live, on-face activity and a reply
    /// to a session nobody is watching has nothing to update.
    private weak var radar: RadarViewModel?

    public init(dependencies: AppDependencies = .current()) {
        self.dependencies = dependencies
        self.inboxProvider = CoreInboxProvider(inbox: core.inbox, crew: core.crew)
        self.packetSender = MeshFireflyPacketSender(client: dependencies.client)
        let client = dependencies.client
        self.uplink = PhoneGPSUplink(
            location: dependencies.location,
            settings: dependencies.store,
            sink: MeshPositionSink(client: client),
            // Read per fix, never captured once: before the handshake
            // there is no node to address, and a fix arriving then is
            // dropped rather than sent to a guessed destination.
            destinationNodeNum: { client.connectedNodeNum })
    }

    /// Idempotent, the same convention every `observe()` in this app
    /// follows. Subscribes `CoreStore` to the client's streams, starts
    /// the phone-GPS uplink, starts the ack-timeout tick, and subscribes
    /// to portnum 269.
    ///
    /// The GPS uplink is started unconditionally and gates ITSELF on
    /// `SettingsKey.locationSharingEnabled` per fix
    /// (`PhoneGPSUplinkPolicy.shouldPush`'s `enabled:`), rather than
    /// being started and stopped as the setting is toggled: the setting
    /// can change while a fix is already in flight, and one gate
    /// evaluated at the moment of each push is the only version of this
    /// that cannot leak a position after the user turned sharing off.
    ///
    /// `async` for one specific reason: `PhoneGPSUplink` is an actor, so
    /// its `start()` — which is where `location.fixes()` is actually
    /// subscribed — can only be reached with an `await`. Firing that off
    /// in a detached `Task` instead would mean the uplink's subscription
    /// races the first fix, and `EventHub` is multicast, NOT replayed
    /// (S1): a fix yielded before the subscription registers is gone.
    /// Awaiting it here makes "the graph has started" mean "every
    /// subscription is live", which is the only version of this that a
    /// test can assert on and a user can rely on.
    public func start() async {
        guard !started else { return }
        started = true
        // `routeDeliveriesToInbox: false` — the view-model path owns the
        // feed's outbox id space here. See
        // `CoreStore.observe(client:routeDeliveriesToInbox:)`.
        core.observe(client: dependencies.client, routeDeliveriesToInbox: false)
        observePrivatePackets()
        await uplink.start()
        tickLoop = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                guard let self else { return }
                // `ff_feed_expire_pending_acks`, the ACK-TIMEOUT half of
                // NO_ACK — it has no event to arrive on, so something has
                // to call it, exactly as `ff_shell_tick` does on the puck
                // (`CoreStore.tick(nowMs:)`'s own doc comment).
                self.core.tick(nowMs: FireflyClock.nowMillis())
            }
        }
    }

    /// PR #265 review, should-fix: nothing in M1 calls this. There is
    /// no `ScenePhase` handling in `FireflyApp.swift`, and the Connect
    /// screen's DISCONNECT button (`ConnectScreen.swift`) calls only
    /// `ConnectViewModel.disconnect()` -> `client.disconnect()` — never
    /// this method. That means the tick loop, the ack-timeout sweep and
    /// the portnum-269 reader all keep running for as long as the
    /// process is alive, even after the radio itself has disconnected
    /// or the app has gone to the background. Deliberate for M1 (the
    /// Settings "stay connected in background" toggle
    /// (`SettingsScreen.swift`) says plainly that background reconnect
    /// isn't built yet either — the two gaps are the same milestone),
    /// not an oversight: tearing the graph down on backgrounding is a
    /// product decision (does a backgrounded app keep tracking crew
    /// positions or not?) that M1 has not made, so this stays reachable
    /// and unused rather than wired to a lifecycle event nobody has
    /// decided the behavior for yet. Tracked for M2.
    public func stop() async {
        started = false
        core.stopObserving()
        privateObservation?.cancel(); privateObservation = nil
        tickLoop?.cancel(); tickLoop = nil
        await uplink.stop()
    }

    /// Decode inbound portnum-269 frames and route each one to whatever
    /// actually consumes it. The DECODE is `FireflyPacket`/`ff_proto`'s
    /// (the client hands over opaque bytes on purpose); a frame
    /// `ff_proto_decode` rejects is dropped silently, the same way the
    /// client drops a malformed protobuf, rather than rendered as
    /// anything.
    private func observePrivatePackets() {
        guard privateObservation == nil else { return }
        let stream = dependencies.client.incomingPrivate()
        privateObservation = Task { [weak self] in
            for await packet in stream {
                guard let self else { return }
                self.handle(private: packet)
            }
        }
    }

    private func handle(private packet: IncomingPrivate) {
        guard let decoded = FireflyPacket.decode(packet.payload) else { return }
        switch decoded {
        case .pong(let nonce, let rssiOfUs, let snrDb):
            // The RSSI that matters for FIND is how THEY heard US — the
            // number inside the PONG body — not this packet's own rx
            // RSSI (how WE heard THEM). Two different measurements, and
            // swapping them is exactly the trap S29's "they hear us at"
            // line exists to avoid, so `packet.rssiDbm` is deliberately
            // NOT what gets passed here.
            radar?.handlePong(fromNodeID: packet.from, nonce: nonce, rssiDbm: rssiOfUs,
                               hasSNR: snrDb != nil, snrDb: Double(snrDb ?? 0))
        case .ping(let nonce):
            // Somebody is FINDing US. Answering is a real S29 behaviour
            // and is deliberately NOT wired in M1: the reply must carry
            // how we hear THEM, which is this packet's own rx RSSI, and
            // an unconditional auto-reply is a transmit decision the
            // owner has not made yet. Tracked, not silently half-done.
            _ = nonce
        case .flare, .flareEnd, .rally, .rallyClear, .status, .ackPing, .retiredReserved01:
            // Inbound FLARE/RALLY/STATUS rendering is M2 (A01's
            // milestones): the feed kinds exist (`ff_feed_kind_t`), but
            // no M1 screen renders one, and pushing them into the inbox
            // with no screen behind them would be inventing rows nobody
            // can open.
            break
        }
    }

    // MARK: - View models

    /// The Radar screen's view model, over the REAL bridges: `ff_crew` +
    /// `ff_radar_compute` for the view, `ff_find` for FIND, and a real
    /// portnum-269 send for its pings.
    public func makeRadarViewModel(haptics: any HapticSignaling = NoHapticSignaling()) -> RadarViewModel {
        let model = RadarViewModel(
            radar: CoreRadarComputing(crew: core.crew, radar: core.radar,
                                       linkIsReady: { [core] in core.linkState == .ready }),
            heading: dependencies.heading,
            location: dependencies.location,
            find: CoreFindSession(find: core.find, sender: packetSender),
            haptics: haptics)
        // Units stay METRIC here, deliberately, and not by reading
        // `SettingsKey.unitsMetric`: `SettingsStoring.bool` cannot tell
        // "the user chose imperial" from "nobody has ever written this
        // key", and nothing writes it yet (M1's Settings units row is
        // not built). `!store.bool(.unitsMetric)` would therefore make
        // every fresh install imperial by accident — a wrong unit on
        // every distance, from an unset default. Wiring this up needs
        // the Settings row AND a tri-state read; tracked, not guessed.
        model.imperial = false
        radar = model
        return model
    }

    /// The Inbox screen's view model, over the C-core provider and with
    /// FLARE actually available (`ThreadViewModel.flareAvailable`) for
    /// the first time — `flareSender` was `nil` in every composition
    /// until a portnum-269 send existed.
    public func makeInboxViewModel() -> InboxViewModel {
        InboxViewModel(provider: inboxProvider, client: dependencies.client, flareSender: packetSender)
    }

    public func makeConnectViewModel() -> ConnectViewModel {
        ConnectViewModel(client: dependencies.client)
    }
}
