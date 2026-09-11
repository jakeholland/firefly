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
    /// M2: the one place a crew member is paired/unpaired/renamed —
    /// writes both `core.crew` (live) and `dependencies.crewPairingStore`
    /// (persisted) together, so Connect's Nearby section and the Crew
    /// section in More can never drift from each other or from what
    /// Radar/Inbox render. See `CrewPairingStore.swift`.
    public let crewPairing: CrewPairingController
    private let uplink: PhoneGPSUplink

    private var privateObservation: Task<Void, Never>?
    private var tickLoop: Task<Void, Never>?
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

    public init(dependencies: AppDependencies = .current(), notifications: any NotificationSending = UNNotificationSending(),
                skipLaunchAutoConnectUnderXCTest: Bool = false) {
        self.dependencies = dependencies
        self.notifications = notifications
        self.skipLaunchAutoConnectUnderXCTest = skipLaunchAutoConnectUnderXCTest
        self.inboxProvider = CoreInboxProvider(inbox: core.inbox, crew: core.crew)
        self.packetSender = MeshFireflyPacketSender(client: dependencies.client)
        self.flareTakeover = FlareTakeoverViewModel(crew: self.core.crew)
        self.crewPairing = CrewPairingController(crew: core.crew, store: dependencies.crewPairingStore)
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
                                    currentFix: { [weak self] in self?.myFix })
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
}
