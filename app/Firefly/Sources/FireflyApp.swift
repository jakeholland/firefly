//
//  FireflyApp.swift — the multiplatform shell.
//
//  One target, two platforms (iOS 17+, macOS 14+). The Mac build is not
//  a courtesy port: the iOS Simulator has no Bluetooth at all, so the
//  Mac — with its own CoreBluetooth radio and a USB-serial transport —
//  is where this app is tested against a real Heltec.
//  See docs/specs/A01-companion-app.md.
//
import FireflyMesh
import FireflyModel
import SwiftUI

@main
struct FireflyApp: App {
    /// THE composition root, constructed exactly once for the process.
    ///
    /// `AppGraph` owns the one `AppDependencies` (`.current()`:
    /// `.stub()` in the iOS Simulator, which has no Bluetooth at all;
    /// `.live()` — real client over `BLETransport`, real CoreLocation
    /// providers, real `SettingsStore` — everywhere else), the one set
    /// of `ff_*` C contexts, and every view model built on top of them.
    ///
    /// Nothing below this line may call `AppDependencies.current()` for
    /// itself: doing so builds a SECOND client over a SECOND transport,
    /// which is exactly the bug `RadarView`'s old no-argument `init()`
    /// shipped with (see `AppGraph`'s own header comment).
    @State private var graph: AppGraph
    @State private var connect: ConnectViewModel
    @State private var inbox: InboxViewModel
    @State private var radar: RadarViewModel
    /// "app: festpack from fest-almanac + Lineup".
    @State private var lineup: LineupViewModel
    /// Map tab slice: `AppGraph.makeMapViewModel()`'s one view model —
    /// built once here, same "one view model per destination, built by
    /// the graph, never re-created on redraw" rule `radar`/`inbox`
    /// above already follow.
    @State private var map: MapViewModel
    /// Shared between the Connect and Settings destinations (see
    /// `SettingsViewModel`'s own comment) so both read the same imported
    /// channel rather than two disconnected copies.
    @State private var channelImport: ChannelImportViewModel
    @State private var settings: SettingsViewModel
    /// Non-nil in exactly one case: `graph.dependencies.client` came
    /// back a `DemoMeshtasticClient` — i.e. the iOS Simulator AND
    /// `-FireflyDemo`/`FIREFLY_DEMO=1` (`AppDependencies.current()`'s
    /// own `#if targetEnvironment(simulator)` gate). Recovered by
    /// downcasting `graph.dependencies` rather than branching `init()`
    /// on `DemoLaunch` a second time, so there is exactly ONE place
    /// (`AppDependencies.current()`) that decides whether this process
    /// is running the demo world at all — this is only ever the
    /// SECOND thing to notice that decision, never the first.
    @State private var demoRunner: DemoRunner?
    /// "app: five-tab bar per design" — read once at construction; see
    /// this property's own assignment in `init` for what it means and
    /// `RootView.hasKnownRadio`'s doc comment for how it is used.
    let hasKnownRadio: Bool
    /// M2 — read by two independent `.onChange(of: scenePhase)` handlers
    /// below, each owning its own concern: tracks foreground/background
    /// so an inbound FLARE takes over the screen only while the app is
    /// actually in front (`AppGraph.setForegrounded(_:)`'s own doc
    /// comment), and drives `AppGraph.handleScenePhaseChange(_:)` —
    /// background disconnects (and tears the graph down) when "stay
    /// connected in background" is off, foreground restarts it. A plain
    /// `@Environment`, not a `@State` this file otherwise constructs —
    /// SwiftUI owns this value.
    @Environment(\.scenePhase) private var scenePhase

    init() {
        // `skipLaunchAutoConnectUnderXCTest: true` — this IS the live
        // process (`AppGraph()`'s own default `dependencies: .current()`,
        // the real composition root), the one `AppGraph.init`'s own doc
        // comment says only `FireflyApp.init()` should opt into: under
        // `xcodebuild test -only-testing:FireflyHardwareTests`/
        // `FireflyAppTests`, THIS process is `Firefly.app` itself,
        // launched as the test's host application, and its own launch
        // auto-connect must not run a second, independent `BLETransport`
        // racing whatever the test itself is doing over BLE (2026-09-11
        // bench investigation).
        //
        // M3 — `-FireflyDemoRestored`: seed an in-memory `HistoryStore`
        // with `DemoHistorySeed` BEFORE `AppGraph.init` runs, so its own
        // restore pass (`HistoryRestorer.restore`) picks the rows up
        // exactly like a real relaunch would — never a parallel "looks
        // restored" fake. Gated the SAME way `AppDependencies.current()`
        // gates every other `DemoLaunch` check (`AppDependencies.swift`'s
        // own comment): only inside `#if targetEnvironment(simulator)`,
        // so a stray launch argument can never turn a real device's
        // history into fictional festival data.
        #if targetEnvironment(simulator)
        var historyOverride: HistoryStore?
        if DemoLaunch.isRestoredRequested() {
            let seeded = HistoryStore.inMemory()
            DemoHistorySeed.seed(into: seeded)
            historyOverride = seeded
        }
        let graph = AppGraph(skipLaunchAutoConnectUnderXCTest: true, historyStore: historyOverride)
        #else
        let graph = AppGraph(skipLaunchAutoConnectUnderXCTest: true)
        #endif
        _graph = State(initialValue: graph)
        let connectVM = graph.makeConnectViewModel()
        _connect = State(initialValue: connectVM)
        // M3 — `client:` passed through explicitly (its default,
        // `StubMeshtasticClient()`, only exists so pre-M3 call sites in
        // tests keep compiling): without this, "Apply to node" would
        // silently write to a disconnected stand-in instead of
        // `graph.dependencies.client`, the one real client the rest of
        // this graph observes.
        let importVM = ChannelImportViewModel(client: graph.dependencies.client)
        _channelImport = State(initialValue: importVM)
        // Slice C's INTEGRATION TASK, now done: this used to construct
        // its own `SettingsStore()` because `AppDependencies.store` was
        // still `InMemorySettingsStore` under both `.stub()` and
        // `.live()`. `.live()` is pointed at the real `SettingsStore`
        // now, so Settings and every other reader of
        // `dependencies.store.bool(.locationSharingEnabled)` — the
        // phone-GPS uplink above all — share ONE instance, instead of
        // agreeing only by `UserDefaults.standard` coincidence.
        //
        // `.makeObserving(...)`, not the plain initializer — that
        // factory's own doc comment on `SettingsViewModel.swift` is
        // where the fix (and the NavigationSplitView remount bug it
        // fixes) is written up; this is the one call site that matters.
        // M3 — `clearHistory:` reaches the SAME `inboxProvider` every
        // screen reads (`AppGraph.inboxProvider`, `PersistingInboxProvider
        // .clearAll()`), never a second, independent path — Settings
        // owns the confirmation UI (`SettingsScreen.swift`), not a
        // second opinion about what "history" means.
        _settings = State(initialValue: SettingsViewModel.makeObserving(store: graph.dependencies.store,
                                                                         channelImport: importVM,
                                                                         client: graph.dependencies.client,
                                                                         clearHistory: { graph.inboxProvider.clearAll() },
                                                                         location: graph.dependencies.location))
        let inboxVM = graph.makeInboxViewModel()
        _inbox = State(initialValue: inboxVM)
        #if os(iOS)
        let haptics: any HapticSignaling = UIKitHapticSignaling()
        #else
        // No Taptic Engine on a Mac — the honest answer, not a gap.
        let haptics: any HapticSignaling = NoHapticSignaling()
        #endif
        let radarVM = graph.makeRadarViewModel(haptics: haptics)
        _radar = State(initialValue: radarVM)
        _lineup = State(initialValue: graph.makeLineupViewModel())
        _map = State(initialValue: graph.makeMapViewModel())
        // "app: five-tab bar per design" — read once, here, from the
        // same persisted state `BLETransport`'s own auto-reconnect
        // already trusts (`AppDependencies.live()`'s `lastPeripheralID`)
        // plus the `-FireflyAutoConnect <name>` debug launch arg
        // (`FireflyAutoConnectLaunch`) — never a fresh read of the
        // live, still-connecting `ConnectViewModel` (`RootView
        // .hasKnownRadio`'s own doc comment has the full reasoning).
        // `.stub()`'s `InMemorySettingsStore` and the demo stack both
        // report nothing persisted here, which is exactly right: a
        // fresh simulator run has no radio to already know about.
        self.hasKnownRadio = graph.dependencies.store.string(.lastPeripheralID) != nil
            || FireflyAutoConnectLaunch.requestedPeripheralName() != nil
        // M2: the FLARE takeover's own haptic pulse (S10: "3 long,
        // overrides quiet hours") — late-injected for the same reason
        // `makeRadarViewModel(haptics:)` takes it as a parameter rather
        // than `AppGraph` picking a platform default itself (`AppGraph`
        // has no UIKit dependency to pick `UIKitHapticSignaling` with).
        graph.flareTakeover.setHaptics(haptics)

        if let demoClient = graph.dependencies.client as? DemoMeshtasticClient,
           let demoLocation = graph.dependencies.location as? DemoLocationProvider,
           let demoHeading = graph.dependencies.heading as? DemoHeadingProvider {
            _demoRunner = State(initialValue: DemoRunner(
                graph: graph, client: demoClient, location: demoLocation, heading: demoHeading,
                connect: connectVM, inbox: inboxVM, radar: radarVM))
        } else {
            _demoRunner = State(initialValue: nil)
        }
    }

    var body: some Scene {
        WindowGroup {
            // One argument per line (SHOULD-FIX 4): every slice that adds
            // a `RootView` dependency appends its own line here instead
            // of editing this call's single line, so sibling slices'
            // hunks land as pure insertions and never collide.
            RootView(
                connect: connect,
                settings: settings,
                channelImport: channelImport,
                client: graph.dependencies.client,
                inbox: inbox,
                radar: radar,
                lineup: lineup,
                scanner: graph.dependencies.scanner,
                demoRunner: demoRunner,
                initialDemoScreen: DemoLaunch.requestedScreen(),
                flareTakeover: graph.flareTakeover,
                pairing: graph.crewPairing,
                // Map tab slice's own hunk — one view model, built once
                // by the graph like every other destination here
                // (`AppGraph.makeMapViewModel()`'s own doc comment), and
                // two thin action closures over `radar`/tab selection
                // rather than plumbing `graph` itself into `RootView`.
                map: map,
                // PR #283 review, BLOCKING 2: this USED to call
                // `graph.core.find.start(targetNodeID:now:)` directly on
                // the raw `FindBridge` — that only flips `ff_find_t
                // .active`; it never actually ticks (sends a ping). The
                // ONLY thing that drives FIND for real is
                // `RadarViewModel.startFind(targetNodeID:)`'s own
                // `findLoop` `Task` (`find.tick(now:)` every second,
                // `CoreFindSession.tick` is what puts a packet on the
                // wire) — nothing else ever starts that loop. Calling
                // the SAME view model FIND already owns here — `radar`,
                // the one `RadarViewModel` this graph built, already in
                // scope — means a FIND started from a Map pin goes
                // through the exact path Radar's own START FIND button
                // does, and Radar's STOP (`stopFind()`/`stopObserving()`
                // on `.onDisappear`) stops it the same way regardless of
                // which screen started it.
                mapFind: { nodeID in radar.startFind(targetNodeID: nodeID) },
                mapMessage: { _ in },
                // "app: five-tab bar per design".
                hasKnownRadio: hasKnownRadio
            )
            .preferredColorScheme(.dark)
            // M2: `AppGraph.setForegrounded(_:)` is the one thing that
            // decides "takeover, or a local notification instead"
            // (`handleInboundFlare`'s own doc comment) — `.active` is the
            // only phase that means "the user can actually see the
            // screen right now"; `.inactive` (a transient state, e.g. an
            // incoming call or the app switcher) is treated the same as
            // `.background` rather than as foregrounded, since neither
            // one means the screen is what the user is looking at.
            .onChange(of: scenePhase) { _, newPhase in
                graph.setForegrounded(newPhase == .active)
            }
            // The graph's own subscriptions (CoreStore over the client's
            // streams, the portnum-269 reader, the phone-GPS uplink and
            // the ack-timeout tick) start with the window and live as
            // long as it does — NOT per screen. A screen's `observe()`
            // is its own, independent subscription (S1); this is the one
            // that has to keep running when no screen is on top of it,
            // because an ack that arrives while Settings is showing is
            // still an ack.
            //
            // ONE chained task, not two independent `.task`s: `DemoRunner
            // .start()` calls `client.connect()`, which immediately plays
            // the whole scripted nodeDB dump — that must never race
            // `graph.start()`'s own `core.observe(client:)` subscription
            // (`EventHub`'s "a subscriber that arrives after this call
            // does not see it" rule). Awaiting `graph.start()` fully
            // first guarantees the subscription is live before demo mode
            // ever calls `connect()`.
            .task {
                await graph.start()
                await demoRunner?.start()
            }
            // M2 — background BLE (docs/specs/A01-companion-app.md):
            // `.active` -> `.background` is the one transition that
            // matters (`AppGraph.handleScenePhaseChange`'s own doc
            // comment gates everything on the "stay connected in
            // background" setting); `.inactive` is a brief mid-transition
            // state on both platforms and deliberately ignored rather
            // than treated as either edge.
            .onChange(of: scenePhase) { _, newPhase in
                Task {
                    switch newPhase {
                    case .active:
                        await graph.handleScenePhaseChange(.foreground)
                    case .background:
                        await graph.handleScenePhaseChange(.background)
                    case .inactive:
                        break
                    @unknown default:
                        break
                    }
                }
            }
        }
        #if os(macOS)
        .defaultSize(width: 420, height: 720)
        #endif
    }
}
