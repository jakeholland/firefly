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
    /// `InboxProviding`, backed by the C core through M3's own
    /// `PersistingInboxProvider` wrapper — never `InMemoryInboxStore`,
    /// which is test-only from here on, and never the bare
    /// `CoreInboxProvider` this used to be: every write flows through
    /// `PersistingInboxProvider` first, so `history` (below) can never
    /// drift from what the live ring actually holds.
    public let inboxProvider: any InboxProviding
    /// M3 — the durable half of message history (docs/specs/
    /// A01-companion-app.md, M3 + Persistence). Owned here, not by
    /// `inboxProvider` alone, because `flushPersistedOutbox()` (below)
    /// needs to read it directly (`pendingOutbox`), and because
    /// `AppGraph.init` reads from it (`loadAllForRestore()`) before
    /// `inboxProvider` — the wrapper — even exists.
    private let historyStore: HistoryStore
    /// PR #281 review, BLOCKING 1: the id sources `makeInboxViewModel()`
    /// hands to `InboxViewModel` (which hands `outboxIDGenerator` on to
    /// every `ThreadViewModel` it opens) — `.shared` by default (every
    /// real launch), overridable ONLY so a test can inject a fresh
    /// instance that behaves exactly like a genuinely independent
    /// process's own `.shared` would, to prove two such lifetimes over
    /// the SAME `historyStore` never collide (`OutboxIDGenerator`'s own
    /// doc comment, `ThreadViewModel.swift`). `init` below seeds
    /// whichever instance it was handed — `.shared` or an override —
    /// from `historyStore`, every launch, before anything can mint a
    /// live id from it (`seedIDGenerators(from:store:outbox:inbound:)`).
    private let outboxIDGenerator: OutboxIDGenerator
    private let inboundFeedIDGenerator: InboundFeedIDGenerator
    /// Portnum 269 (FLARE, and FIND's PING).
    public let packetSender: MeshFireflyPacketSender
    /// M2: the one place a crew member is paired/unpaired/renamed —
    /// writes both `core.crew` (live) and `dependencies.crewPairingStore`
    /// (persisted) together, so Connect's Nearby section and the Crew
    /// section in More can never drift from each other or from what
    /// Radar/Inbox render. See `CrewPairingStore.swift`.
    public let crewPairing: CrewPairingController
    /// "app: festpack from fest-almanac + Lineup" — the Lineup screen's
    /// data source. Demo mode (`dependencies.client is DemoMeshtasticClient`
    /// — the same downcast `FireflyApp.init` uses to recover its own
    /// `DemoRunner`, so there is exactly ONE place that decision is made
    /// twice over rather than a second, independent flag that could
    /// drift from it) gets `DemoFestpackProvider` (Firefly Fields,
    /// bundle-only, per the owner's decision); every other composition
    /// — `.stub()` included, since network access has nothing to do
    /// with whether there is a radio to talk to — gets the real
    /// `AlmanacFestpackProvider`.
    public let festpack: any FestpackProviding
    /// "app: Lineup by-stage grid, day pills, My picks" — superseded
    /// `starredArtists` (`StarredArtistsStoring`, artist-keyed); picks
    /// are per-SET now, see `PicksStore`'s own doc comment.
    public let picks: any PicksStoring
    private let uplink: PhoneGPSUplink

    private var privateObservation: Task<Void, Never>?
    private var tickLoop: Task<Void, Never>?
    /// M3 — flushes `historyStore`'s persisted WAITING items on the
    /// link's next not-ready -> ready edge. See
    /// `observeHistoryOutboxFlush()`'s own doc comment for why this has
    /// to be a graph-level subscription rather than left to whichever
    /// `ThreadViewModel` (if any) happens to be open.
    private var historyOutboxFlushObservation: Task<Void, Never>?
    private var started = false
    /// M2: true once `autoConnectToLastKnownPeripheral()` has been
    /// considered, ever — set on the FIRST `start()` only, deliberately
    /// never reset by `stop()`. Without this, `handleScenePhaseChange
    /// (.foreground)`'s own `start()` call would auto-connect on EVERY
    /// foreground resume, including one that just followed a
    /// `backgroundConnectEnabled == false` disconnect — silently undoing
    /// "off means off, the user taps CONNECT" the moment they glance at
    /// another app and back. This flag confines the auto-connect to true
    /// process launch, matching the M2 task's own wording ("auto-
    /// connecting to it at launch").
    private var hasAttemptedLaunchAutoConnect = false
    /// Handed a decoded PONG so FIND's replies list and haptics update.
    /// Set when `makeRadarViewModel` builds one; nil before that, which
    /// is why a PONG arriving with no Radar on screen is dropped rather
    /// than queued — S29's FIND is a live, on-face activity and a reply
    /// to a session nobody is watching has nothing to update.
    private weak var radar: RadarViewModel?

    // MARK: - M2: inbound FLARE/RALLY/STATUS + PING auto-reply
    // (docs/specs/A01-companion-app.md M2; see `AppGraph+M2Protocol.swift`
    // for the methods that use these — split into its own file so this
    // shared file's own diff stays small, three other M2 slices touching
    // it in the same worktree).

    /// The inbound-FLARE takeover's whole state — one per process, like
    /// every other `ff_*`-backed view model this graph owns.
    public let flareTakeover: FlareTakeoverViewModel
    /// `UNNotificationSending` by default (the live graph) — local
    /// notifications for a backgrounded FLARE/text (task point 4).
    /// Injectable so a test never has to touch the real
    /// `UNUserNotificationCenter` (`UNNotificationSending`'s own doc
    /// comment on why that matters under bare `swift test`).
    let notifications: any NotificationSending
    /// Whether the app is in the foreground right now — `FireflyApp`'s
    /// `ScenePhase` observation is the one caller (`setForegrounded(_:)`).
    /// Starts `true`: the app IS foregrounded at the moment its own
    /// composition root is built, before any `ScenePhase` event has ever
    /// fired.
    var isForegrounded = true
    /// The phone's own last known fix, kept live by `observeMyLocation()`
    /// — shared by `FlareTakeoverViewModel`'s bearing/distance and
    /// inbound RALLY's own distance/bearing text.
    var myFix: LocationFix?
    var locationObservation: Task<Void, Never>?
    /// A second, independent `incomingTexts()` subscription purely for
    /// backgrounded-text notifications — see `observeIncomingTextsForNotifications()`.
    var incomingTextNotificationObservation: Task<Void, Never>?
    /// PONG auto-reply's "one reply per nonce" memory.
    var repliedPongNonces = PongReplyDedup()

    /// `FireflyApp.init()` alone passes `true` — see
    /// `autoConnectToLastKnownPeripheral()`'s own doc comment for why
    /// this needs to be an explicit opt-in rather than `isRunningUnderXCTest`
    /// alone: `AppGraphTests` constructs `AppGraph` directly and also
    /// runs under XCTest, but legitimately wants launch auto-connect to
    /// behave normally under test.
    private let skipLaunchAutoConnectUnderXCTest: Bool

    /// `historyStore:` — M3's own override seam, `nil` by default. When
    /// `nil`, the store is picked the same way `.stub()`/`.demo()`/
    /// `.live()` already distinguish themselves elsewhere in this file:
    /// `dependencies.store is InMemorySettingsStore` is true for both
    /// `.stub()` and every `.demo()`/`.demoBundle()` composition (and
    /// for nothing `.live()` ever builds), so it doubles as "is this a
    /// disposable stack" without a second flag to keep in sync — a
    /// disposable stack gets a disposable, in-memory history store,
    /// matching M3's own demo-isolation rule ("Demo doesn't persist
    /// across launches: in-memory store only") for free. Explicit
    /// callers use this to inject a store that already has rows in it:
    /// `DemoRunner`'s own `-FireflyDemoRestored` seeding does exactly
    /// that (an in-memory store, pre-populated, so the SAME restore code
    /// path a real relaunch takes is what renders it), and so does any
    /// test that wants to simulate "two launches sharing one store".
    public init(dependencies: AppDependencies = .current(), notifications: any NotificationSending = UNNotificationSending(),
                skipLaunchAutoConnectUnderXCTest: Bool = false, historyStore: HistoryStore? = nil,
                outboxIDGenerator: OutboxIDGenerator = .shared, inboundFeedIDGenerator: InboundFeedIDGenerator = .shared) {
        self.dependencies = dependencies
        self.notifications = notifications
        self.skipLaunchAutoConnectUnderXCTest = skipLaunchAutoConnectUnderXCTest
        self.historyStore = historyStore ?? (dependencies.store is InMemorySettingsStore ? .inMemory() : .live())
        self.outboxIDGenerator = outboxIDGenerator
        self.inboundFeedIDGenerator = inboundFeedIDGenerator
        let rawInboxProvider = CoreInboxProvider(inbox: core.inbox, crew: core.crew)
        // M3 — reseed the live ring from storage NOW, before
        // `PersistingInboxProvider` even exists and before ANYTHING can
        // observe a client: pushed straight into the RAW provider so
        // restoring a message is never itself treated as new traffic to
        // re-persist (`PersistingInboxProvider`'s own header comment).
        // Mirrors `CrewPairingRestorer.restore`'s identical ordering
        // rule a few lines down, for the identical reason: a
        // want_config replay's first live event must never race a
        // still-in-progress restore.
        //
        // PR #281 review, BLOCKING 1: `seedIDGenerators` runs BEFORE
        // `HistoryRestorer.restore` for the identical reason — both read
        // `allHistory`, computed exactly once here, but only the
        // generator seeding must land before restore's own `push`es,
        // since neither generator is used by `restore` itself (it
        // replays each message's own already-assigned id, never a fresh
        // one) — ordered first anyway so that even the FIRST live id
        // this process could ever mint, however soon after `init`
        // returns, is already past everything `historyStore` holds.
        let allHistory = self.historyStore.loadAllForRestore()
        Self.seedIDGenerators(from: allHistory, store: self.historyStore,
                               outbox: self.outboxIDGenerator, inbound: self.inboundFeedIDGenerator)
        let restoredMessageIDs = HistoryRestorer.restore(allHistory, into: rawInboxProvider)
        self.inboxProvider = PersistingInboxProvider(wrapping: rawInboxProvider, history: self.historyStore,
                                                      restoredMessageIDs: restoredMessageIDs)
        self.packetSender = MeshFireflyPacketSender(client: dependencies.client)
        self.flareTakeover = FlareTakeoverViewModel(crew: self.core.crew)
        self.crewPairing = CrewPairingController(crew: core.crew, store: dependencies.crewPairingStore)
        self.festpack = dependencies.client is DemoMeshtasticClient
            ? DemoFestpackProvider()
            : AlmanacFestpackProvider(settings: dependencies.store)
        self.picks = PicksStore(store: dependencies.store)
        let client = dependencies.client
        self.uplink = PhoneGPSUplink(
            location: dependencies.location,
            settings: dependencies.store,
            sink: MeshPositionSink(client: client),
            // Read per fix, never captured once: before the handshake
            // there is no node to address, and a fix arriving then is
            // dropped rather than sent to a guessed destination.
            destinationNodeNum: { client.connectedNodeNum })
        // Only safe now: every stored property above is set, so `self`
        // may finally be captured (`setCurrentFix`'s own doc comment).
        self.flareTakeover.setCurrentFix { [weak self] in self?.myFix }
        // M2: replay the persisted paired list onto `core.crew` NOW —
        // before `start()` ever subscribes `core` to the client's
        // `nodeUpdates()` stream (`start()` is a separate, later call;
        // nothing above this line touches the client's streams either).
        // A want_config replay's `CoreStore.apply(nodeUpdate:)` ->
        // `crew.setIdentity` must never be the first thing to see a
        // reconnecting member's slot — restoring PAIRED (and the
        // member's persisted colour) here first is what keeps that flag
        // from ever reading as lost for even one frame.
        // `CrewPairingRestorer`'s own doc comment.
        CrewPairingRestorer.restore(from: dependencies.crewPairingStore, into: core.crew)
    }

    /// `FireflyApp`'s `ScenePhase` observation calls this — the one
    /// source of truth `handleInboundFlare`/the notification path below
    /// read to decide "takeover, or a local notification instead".
    public func setForegrounded(_ active: Bool) { isForegrounded = active }

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
        Self.log("start() called (started=\(started))")
        guard !started else {
            Self.log("start(): already started — no-op")
            return
        }
        started = true
        // `routeDeliveriesToInbox: false` — the view-model path owns the
        // feed's outbox id space here. See
        // `CoreStore.observe(client:routeDeliveriesToInbox:)`.
        core.observe(client: dependencies.client, routeDeliveriesToInbox: false)
        observePrivatePackets()
        observeMyLocation()
        observeIncomingTextsForNotifications()
        observeHistoryOutboxFlush()
        Self.log("start(): awaiting uplink.start()")
        await uplink.start()
        Self.log("start(): uplink.start() returned")
        tickLoop = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                guard let self else { return }
                // `ff_feed_expire_pending_acks`, the ACK-TIMEOUT half of
                // NO_ACK — it has no event to arrive on, so something has
                // to call it, exactly as `ff_shell_tick` does on the puck
                // (`CoreStore.tick(nowMs:)`'s own doc comment).
                self.core.tick(nowMs: FireflyClock.nowMillis())
                // The FLARE takeover's own auto-dismiss (S10: "Auto-end
                // at dur") — same tick-driven shape, same loop.
                self.flareTakeover.tick()
            }
        }
        if !hasAttemptedLaunchAutoConnect {
            hasAttemptedLaunchAutoConnect = true
            let remembered = dependencies.store.string(.lastPeripheralID)
            Self.log("start(): considering launch auto-connect — lastPeripheralID=\(remembered ?? "nil")")
            autoConnectToLastKnownPeripheral()
        }
        Self.log("start() completed — every subscription is live")
    }

    /// Same discipline as `BLETransport.log(_:)`/`MeshtasticClient.log(_:)`:
    /// a raw stderr write, unconditional, so the composition root's own
    /// startup sequence — in particular WHEN (if ever) the launch
    /// auto-connect fires relative to `start()` finishing — is visible
    /// in the same log a `MeshtasticClient`/`BLETransport` capture
    /// already carries, not a separate channel that has to be
    /// cross-referenced by timestamp alone.
    private static func log(_ message: String) {
        let line = "[AppGraph] \(message)\n"
        FileHandle.standardError.write(Data(line.utf8))
    }

    /// True when THIS process is an XCTest host. `XCTestConfigurationFilePath`
    /// is the same environment key XCTest itself sets on every test run,
    /// app-hosted or not (Apple's own documented mechanism, not a
    /// heuristic this repo invented) — true just as much for `swift
    /// test`'s own `AppGraphTests` (a completely different process, never
    /// hosted by `Firefly.app`) as for `xcodebuild test`. Deliberately
    /// NOT gated on by itself — `autoConnectToLastKnownPeripheral()`'s
    /// own doc comment on `skipLaunchAutoConnectUnderXCTest` says why.
    static var isRunningUnderXCTest: Bool {
        ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] != nil
    }

    /// M2 — "remembering the last connected peripheral identifier and
    /// auto-connecting to it at launch" (docs/specs/A01-companion-app.md;
    /// behaviour borrowed from Meshtastic-Apple's own launch-time
    /// auto-connect to the preferred device, re-implemented against this
    /// app's own client seam). Only ever fires anything under the LIVE
    /// stack: `.stub()`/`.demo()` each construct a fresh
    /// `InMemorySettingsStore()` with nothing persisted in it, so
    /// `lastPeripheralID` reads nil there by construction — no auto-
    /// connect noise in a test or the iOS Simulator.
    ///
    /// Fired off as its own `Task`, never awaited inline: a radio that
    /// is not in range yet must not hold up the rest of this method's
    /// own startup (the tick loop, the private-packet reader) — and
    /// `MeshtasticClientProtocol.connect()`'s own retry/timeout already
    /// handles a dead or out-of-range node on its own. `try?` swallows
    /// `MeshtasticClientError.alreadyConnecting` on purpose: if the
    /// Connect screen's own CONNECT button won the race instead, that
    /// attempt is the one that should finish, not this one.
    private func autoConnectToLastKnownPeripheral() {
        if skipLaunchAutoConnectUnderXCTest, Self.isRunningUnderXCTest {
            // Follow-up to the 2026-09-11 bench-reproduced power-cycle
            // investigation (`docs/specs/A01-companion-app.md`, M2):
            // `BLEHardwareTests` constructs and drives its OWN
            // `BLETransport`/`MeshtasticClient` directly, never through
            // this graph — but `xcodebuild test -only-testing:
            // FireflyHardwareTests` still launches `Firefly.app` itself
            // as the test's HOST application, and `FireflyApp`'s own
            // `.task { await graph.start() }` ran this graph's launch
            // auto-connect in that SAME process the whole time: a
            // second, entirely independent `BLETransport` connecting to
            // `SettingsKey.lastPeripheralID`'s remembered peripheral
            // (`Meshtastic_06b0`, on this bench) and racing whatever the
            // test itself was doing over BLE, confusing the test's own
            // log with a second `[AppGraph]`/`[BLETransport]` sequence
            // that has nothing to do with the test. Only
            // `FireflyApp.init()` passes `skipLaunchAutoConnectUnderXCTest:
            // true` (this type's own doc comment) — every test that
            // constructs `AppGraph` directly, including this file's own
            // auto-connect tests, defaults to `false` and is untouched.
            Self.log("autoConnectToLastKnownPeripheral(): running under XCTest (XCTestConfigurationFilePath set) " +
                     "and skipLaunchAutoConnectUnderXCTest is set — not connecting")
            return
        }
        guard dependencies.store.string(.lastPeripheralID) != nil else {
            Self.log("autoConnectToLastKnownPeripheral(): nothing remembered — not connecting")
            return
        }
        Self.log("autoConnectToLastKnownPeripheral(): firing client.connect() as its own Task")
        Task { [dependencies] in
            do {
                try await dependencies.client.connect()
                Self.log("autoConnectToLastKnownPeripheral(): client.connect() returned successfully")
            } catch {
                // `try?` below still swallows this — logged here first so
                // a `.alreadyConnecting` loss to the Connect screen's own
                // manual CONNECT (or any other failure) is visible rather
                // than silently disappearing into the `try?`.
                Self.log("autoConnectToLastKnownPeripheral(): client.connect() threw \(error)")
            }
        }
    }

    /// M2 — the "stay connected in background" setting actually gating
    /// something (`SettingsScreen.swift`'s toggle; PR #265 review,
    /// should-fix, tracked for M2 on `stop()`'s own doc comment below).
    /// `FireflyApp.swift` calls this from its `ScenePhase` observer.
    public enum LifecyclePhase: Sendable { case foreground, background }

    public func handleScenePhaseChange(_ phase: LifecyclePhase) async {
        switch phase {
        case .background:
            // ON: do nothing — BLETransport's own reconnect-on-loss plus
            // CoreBluetooth's `bluetooth-central` background mode keep
            // the link (and this graph) alive with the screen off.
            // OFF: tear the graph down AND disconnect, right now — "off
            // = disconnect when backgrounded" (the M2 task's own words).
            guard !dependencies.store.backgroundConnectEnabled else { return }
            await stop()
        case .foreground:
            // Idempotent (`start()`'s own guard): a no-op if the graph
            // never stopped (the setting was on), and a genuine restart
            // if it did. Deliberately does NOT reconnect the client by
            // itself when the setting was off — that would silently
            // undo "off means off"; the user taps CONNECT again, same
            // as M1.
            await start()
        }
    }

    /// PR #265 review, should-fix: M1 shipped this reachable and unused
    /// — no `ScenePhase` handling in `FireflyApp.swift`, and the Connect
    /// screen's DISCONNECT button (`ConnectScreen.swift`) called only
    /// `ConnectViewModel.disconnect()` -> `client.disconnect()`, never
    /// this method — because tearing the graph down on backgrounding was
    /// a product decision M1 had not made yet (does a backgrounded app
    /// keep tracking crew positions or not?). M2 makes it: this is now
    /// the exact thing `handleScenePhaseChange(.background)` calls when
    /// `backgroundConnectEnabled` is off — "off = disconnect when
    /// backgrounded", the M2 task's own words.
    public func stop() async {
        started = false
        core.stopObserving()
        privateObservation?.cancel(); privateObservation = nil
        stopObservingMyLocation()
        stopObservingIncomingTextsForNotifications()
        historyOutboxFlushObservation?.cancel(); historyOutboxFlushObservation = nil
        tickLoop?.cancel(); tickLoop = nil
        await uplink.stop()
        // The graph's own subscriptions stopping is not enough on its
        // own — the client (and BLETransport's own reconnect-on-loss
        // loop underneath it) would otherwise keep the radio link open
        // and reconnecting forever, exactly the thing turning this
        // setting off is supposed to prevent.
        await dependencies.client.disconnect()
    }

    /// Decode inbound portnum-269 frames and route each one to whatever
    /// actually consumes it. The DECODE is `FireflyPacket`/`ff_proto`'s
    /// (the client hands over opaque bytes on purpose); a frame
    /// `ff_proto_decode` rejects is dropped silently, the same way the
    /// client drops a malformed protobuf, rather than rendered as
    /// anything.
    // NIT (PR #275 review): no per-packet `await MainActor.run` here on
    // purpose, and this is not an oversight to "fix" later. `AppGraph`
    // is `@MainActor` (this file's own class declaration above), and a
    // `Task { ... }` created from `@MainActor`-isolated code (this
    // method) inherits that isolation for its WHOLE lifetime — not just
    // its first line. So every iteration of `for await packet in
    // stream`, and `self.handle(private: packet)` in particular, already
    // runs ON the main actor as a direct, synchronous call — there is no
    // hop to add per packet on this radio-traffic hot path, and adding
    // one would only add latency for nothing.
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
            // Somebody is FINDing US (S29 PR 2) — a puck's own spec'd
            // behaviour, and the app must answer exactly as honestly:
            // one PONG, direct-addressed, carrying the RSSI/SNR OUR OWN
            // radio measured on THIS packet.
            replyToPing(from: packet.from, nonce: nonce, rssiDbm: packet.rssiDbm, snrDb: packet.snrDb)
        case .flare(let durationS):
            handleInboundFlare(from: packet.from, to: packet.to, durationS: durationS)
        case .flareEnd:
            handleInboundFlareEnd(from: packet.from)
        case .rally(let latitude, let longitude, let name):
            handleInboundRally(from: packet.from, to: packet.to, latitude: latitude, longitude: longitude, name: name)
        case .rallyClear:
            handleInboundRallyClear(from: packet.from)
        case .status(let text):
            handleInboundStatus(from: packet.from, to: packet.to, text: text)
        case .ackPing, .retiredReserved01:
            // ACK_PING is reserved for v1.5 (no encoder exists yet, S04);
            // RESERVED_01 is the permanently-retired PULSE shape (S04's
            // Amendments) — both decode successfully and both are
            // honestly nothing to do, same as the puck's own
            // `app/ff_wiring.c`.
            break
        }
    }

    // MARK: - M3: id generator seeding (PR #281 review, BLOCKING 1)

    /// Raises `outbox`/`inbound`'s floor so neither can ever mint an id
    /// already sitting in `store` — the fix for the review's BLOCKING
    /// finding: `OutboxIDGenerator`/`InboundFeedIDGenerator` used to
    /// restart from the SAME fixed base on every launch, so a second
    /// session's very first send/receive could alias a first session's,
    /// letting `HistoryStore.record`'s upsert silently overwrite an
    /// unrelated persisted row, or letting a live status update
    /// (`markSent`/`setStatus(outboxID:)`) land on a restored item's row
    /// instead of the live message that actually earned it.
    ///
    /// Two sources are combined with `max`, never either alone:
    ///  1. The highest id of each id-space actually present in
    ///     `allHistory` right now (this launch's own `loadAllForRestore`
    ///     snapshot, passed in rather than re-queried).
    ///  2. `store`'s own persisted watermark (`HistoryStore.watermark
    ///     (for:)`) — durable independently of which rows currently
    ///     exist, so a row `HistoryStore.prune()` already evicted
    ///     (oldest-BY-TIMESTAMP, not oldest-by-id) cannot silently lower
    ///     the floor a past launch already proved was necessary.
    /// The combined floor is then written straight back as the new
    /// watermark (3) — even a launch that sends or receives nothing
    /// still raises the durable floor to at least what `allHistory`
    /// alone already proves, so the NEXT launch is never left relying on
    /// today's rows surviving pruning.
    ///
    /// Ids are partitioned by their OWN top bit, never by
    /// `FeedMessage.direction` — `InboundFeedIDGenerator` sets the top
    /// bit on every id it mints and `OutboxIDGenerator` never does
    /// (both types' own doc comments), and that structural partition is
    /// what actually determines which generator a given id belongs to,
    /// regardless of what this app happened to record as that message's
    /// `direction`.
    private static func seedIDGenerators(from allHistory: [(ConversationKind, FeedMessage)], store: HistoryStore,
                                          outbox: OutboxIDGenerator, inbound: InboundFeedIDGenerator) {
        let topBit: UInt64 = 0x8000_0000_0000_0000
        let maxOutboundID = allHistory.map(\.1.id).filter { $0 & topBit == 0 }.max()
        let maxInboundID = allHistory.map(\.1.id).filter { $0 & topBit != 0 }.max()

        let outboxFloor = Swift.max(maxOutboundID.map { $0 &+ 1 } ?? 1,
                                     store.watermark(for: HistoryStore.outboxWatermarkKey))
        let inboundFloor = Swift.max(maxInboundID.map { $0 &+ 1 } ?? topBit,
                                      store.watermark(for: HistoryStore.inboundWatermarkKey))

        outbox.seed(atLeast: outboxFloor)
        inbound.seed(atLeast: inboundFloor)
        store.raiseWatermark(for: HistoryStore.outboxWatermarkKey, to: outboxFloor)
        store.raiseWatermark(for: HistoryStore.inboundWatermarkKey, to: inboundFloor)
    }

    // MARK: - M3: persisted outbox flush

    /// WAITING items that survived a relaunch (`HistoryStore
    /// .pendingOutbox`) have no live `ThreadViewModel` watching them —
    /// that type's own in-memory `outbox` array only ever holds what IT
    /// personally queued THIS session (`ThreadViewModel.swift`'s own
    /// header comment). This is the flush that owns them instead: a
    /// fresh, independent `client.linkState()` subscription (S1 — this
    /// graph's own, never shared with `core`'s or any view model's), the
    /// same "flushed automatically the next time the link reaches ready"
    /// rule a live thread's own outbox follows, applied once, at the
    /// composition-root level, to whatever persisted WAITING items exist
    /// regardless of which screen — if any — is open. No double-send
    /// risk against a live `ThreadViewModel`'s own flush: a restored
    /// WAITING item was never in any `ThreadViewModel`'s session-local
    /// array to begin with (that array starts empty every launch), so
    /// the two queues can never overlap.
    private func observeHistoryOutboxFlush() {
        guard historyOutboxFlushObservation == nil else { return }
        let links = dependencies.client.linkState()
        historyOutboxFlushObservation = Task { [weak self] in
            var wasReady = false
            for await state in links {
                guard let self else { return }
                let ready = (state == .ready)
                if ready, !wasReady { await self.flushPersistedOutbox() }
                wasReady = ready
            }
        }
    }

    /// Bounded (`ThreadViewModel.outboxCap`), oldest first — the same
    /// drop-oldest FIFO discipline a live thread's own outbox follows,
    /// applied here to whatever `historyStore` still has WAITING.
    /// `item.destination` is read straight off the restored row (never
    /// re-derived): it is exactly `meshBroadcastAddress` for a
    /// whole-crew send, matching `ff_feed_item_t.to_node`'s own
    /// "0 = broadcast" convention one layer up.
    private func flushPersistedOutbox() async {
        for (_, item) in historyStore.pendingOutbox(cap: ThreadViewModel.outboxCap) {
            let dest = item.destination ?? meshBroadcastAddress
            let wantAck = (dest != meshBroadcastAddress)
            do {
                let packetID = try await dependencies.client.sendText(item.text, to: dest, wantAck: wantAck)
                inboxProvider.markSent(outboxID: item.id, packetID: packetID, at: Date())
            } catch {
                inboxProvider.setStatus(outboxID: item.id, state: .dropped, at: Date())
            }
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
        // M2: was hard-`false` here (METRIC always) — see git history on
        // this line for the reasoning that used to justify it, now moot.
        // `SettingsKey.unitsMetric`'s bool could never distinguish "the
        // user chose imperial" from "nobody has ever written this key",
        // so a naive `!store.bool(.unitsMetric)` would have made every
        // fresh install imperial by accident. `resolvedImperial()`
        // (`SettingsStoring.swift`'s "Units preference" section) is the
        // tri-state fix: `.system` — the real default — follows the
        // phone's own locale instead of guessing, and only an explicit
        // Settings-row choice ever overrides it. This is a one-time read
        // at view-model construction (every view model in this graph is
        // built once, in `FireflyApp.init` — `RootView`'s own header
        // comment), the same timing every other settings-backed field
        // here already uses; a change made mid-session on the Settings
        // screen takes effect on the next launch, not live — a narrower
        // gap than the bug this replaces, and tracked here rather than
        // silent.
        model.imperial = dependencies.store.resolvedImperial()
        radar = model
        // `makeConnectViewModel()`'s own doc comment below has the full
        // story (the NavigationSplitView detail-column remount that
        // orphans a screen-owned `.onAppear`/`.onDisappear` subscription
        // for the rest of the process). `RadarView`, `InboxContainerView`
        // and `SettingsScreen` are the same shape as `ConnectScreen` was
        // — a process-lifetime singleton shown as one of that split
        // view's `detail(for:)` destinations (`RootView.swift`) — so
        // they get the identical fix: `observe()` started HERE, once,
        // rather than left to a screen's own appear/disappear to
        // establish or tear down.
        model.observe()
        return model
    }

    /// The Inbox screen's view model, over the C-core provider and with
    /// FLARE actually available (`ThreadViewModel.flareAvailable`) for
    /// the first time — `flareSender` was `nil` in every composition
    /// until a portnum-269 send existed.
    public func makeInboxViewModel() -> InboxViewModel {
        let model = InboxViewModel(provider: inboxProvider, client: dependencies.client, flareSender: packetSender,
                                    currentFix: { [weak self] in self?.myFix },
                                    outboxIDGenerator: outboxIDGenerator, inboundFeedIDGenerator: inboundFeedIDGenerator)
        // Same fix as `makeRadarViewModel(haptics:)` just above, and for
        // the identical reason — see `makeConnectViewModel()`'s doc
        // comment for the full NavigationSplitView remount story this is
        // immune to by construction now. `ThreadViewModel`, which THIS
        // view model hands out per `openThread(_:)` call, is unaffected
        // and correctly stays screen-owned (`ThreadView.swift`'s own
        // `.onAppear`/`.onDisappear`) — a thread pushed via a nested
        // `NavigationStack` is genuinely per-navigation state, not a
        // `detail(for:)` destination subject to this remount at all.
        model.observe()
        return model
    }

    /// "app: festpack from fest-almanac + Lineup" — same `observe()`-once
    /// construction convention as every other screen (`makeInboxViewModel()`
    /// just above, `makeRadarViewModel(haptics:)`), so the Lineup tab's
    /// own `festpackUpdates()` subscription lives with the graph, not
    /// with whichever `detail(for:)` remount happens to show it next.
    public func makeLineupViewModel() -> LineupViewModel {
        let model = LineupViewModel(festpackProvider: festpack, picksStore: picks)
        model.observe()
        return model
    }

    public func makeConnectViewModel() -> ConnectViewModel {
        // `store:` — SHOULD-FIX 5 (PR #272 review): the "Forget this
        // node" action needs a real settings seam to clear
        // `SettingsKey.lastPeripheralID` through. `.stub()`/`.demo()`
        // each hand this an `InMemorySettingsStore()` with nothing
        // persisted (`autoConnectToLastKnownPeripheral()`'s own doc
        // comment), so FORGET is harmlessly disabled there too
        // (`canForgetNode`).
        let model = ConnectViewModel(client: dependencies.client, store: dependencies.store)
        // BUGFIX (app: fix live connect path never reaching CONNECTED on
        // macOS) — `observe()` started HERE, once, for the life of the
        // graph, exactly like `core.observe(client:...)`'s own client
        // subscriptions a few lines up in `start()`: this view model's
        // ONE `AsyncStream` from `client.linkState()` (idempotent to
        // (re-)start — `ConnectViewModel.observe()`'s own guard) is now
        // never left to `ConnectScreen`'s own `.onAppear`/`.onDisappear`
        // to establish or tear down.
        //
        // It used to be exactly that: `ConnectScreen.onAppear { connect
        // .observe() ... }` / `.onDisappear { connect.stopObserving()
        // ... }`, the same convention every other per-screen subscription
        // in this app follows (`NearbyNodesViewModel.observe()`,
        // `InboxViewModel`'s own). Link state is NOT like those — a
        // screen's own node/inbox feed is legitimately meaningless while
        // that screen is off-screen, but "is the radio connected" is
        // true or false for the WHOLE app, the moment `connect()` is
        // called from anywhere (a manual CONNECT tap, or `AppGraph`'s own
        // launch auto-connect to the remembered peripheral, well before
        // any screen has appeared at all).
        //
        // Bench-reproduced (2026-09-11, signed macOS build, launched
        // plain via `open`, no launch arguments): `NavigationSplitView`'s
        // detail column remounts ONCE at launch while the sidebar's own
        // `List(selection:)` settles its initial selection —
        // `ConnectScreen.onAppear` fires, THEN `.onDisappear` fires
        // (cancelling the subscription `stopObserving()`'s own doc
        // comment describes), and `.onAppear` never fires again even
        // though the window stays open on the Connect tab for the rest
        // of the session. `MeshtasticClient` itself connects and reaches
        // `.ready` perfectly — `BLETransport`'s own log shows the whole
        // sequence complete (`didConnect` through `central.connect
        // completed`), the want_config handshake both phases — but with
        // no subscriber left alive to hear ANY of it, `ConnectViewModel
        // .link` never leaves `.disconnected` and the Connect screen
        // reads "NOT CONNECTED" for the rest of the process, no matter
        // how many times CONNECT is tapped afterward. Owning this
        // subscription here instead — the composition root's own job per
        // this file's header comment ("subscribed when") — makes it
        // immune to that remount, or to any other screen-level lifecycle
        // churn, by construction.
        model.observe()
        return model
    }

    /// Map tab slice: crew from `core.crew` (the SAME roster Radar
    /// reads — never a second `CrewStore`), the phone's own fix/heading
    /// from `dependencies`, and a `MapFestpackSource`. The parallel
    /// festpack-foundation slice (PR #285) has now landed `FestpackProviding`
    /// on `main` — this is the "one-line swap" `MapFestpack.swift`'s own
    /// header comment anticipated: demo builds still get
    /// `DemoMapFestpackSource` (Firefly Fields must stay independent of
    /// network/real-pack availability, same call `festpack` above
    /// makes for Lineup), and every other build gets
    /// `FestpackProvidingMapAdapter` wrapping the SAME `festpack`
    /// instance `makeLineupViewModel()` reads — one provider, one
    /// composition root, never a second independent fetch. Same
    /// `dependencies.client is DemoMeshtasticClient` downcast `festpack`
    /// above and `FireflyApp.init` both already use, so there is
    /// exactly one place this decision is made, not a second flag that
    /// could drift from it.
    public func makeMapViewModel() -> MapViewModel {
        let festpackSource: any MapFestpackSource = dependencies.client is DemoMeshtasticClient
            ? DemoMapFestpackSource()
            : FestpackProvidingMapAdapter(provider: festpack)
        let model = MapViewModel(crew: core.crew, location: dependencies.location, heading: dependencies.heading,
                                  festpackSource: festpackSource, connectivity: NetworkConnectivityMonitor(),
                                  imperial: { [dependencies] in dependencies.store.resolvedImperial() })
        // PR #283 review, BLOCKING 3: deliberately NOT `model.observe()`
        // here, unlike `makeRadarViewModel`/`makeInboxViewModel`/
        // `makeConnectViewModel` just above. Those three are each
        // genuinely process-lifetime state (`makeConnectViewModel()`'s
        // own doc comment draws the exact line: link state is true/false
        // for the WHOLE app, not meaningful only while one screen is on
        // top). `MapViewModel`'s 1 Hz `pinRefreshLoop` plus its
        // location/heading/connectivity subscriptions exist ONLY to
        // refresh the Map tab's OWN UI (pin age text, the festpack
        // projection, the offline chip) — no other screen consumes any
        // of it — so this is the "genuinely screen-scoped" category
        // `NearbyNodesViewModel`/`ThreadViewModel`/`DiagnosticsViewModel`
        // already follow: `MapTabView`'s own `.onAppear { model.observe()
        // }` / `.onDisappear { model.stopObserving() }` (Map/MapTabView.swift)
        // starts and stops it, so the loop — and the battery it costs —
        // stops the moment the Map tab is no longer visible, not just at
        // app exit.
        return model
    }
}
