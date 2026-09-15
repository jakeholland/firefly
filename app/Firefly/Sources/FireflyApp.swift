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
import Foundation
import SwiftUI
#if os(iOS)
import UIKit
#endif

#if os(iOS)
/// A03 §3.1 — **the hook iOS actually guarantees runs on a background
/// relaunch.** A SwiftUI `.task` is not: a CoreBluetooth relaunch has no
/// scene, so nothing attached to a scene's content ever runs.
///
/// Everything it does lives in `AppGraph.handleDidFinishLaunching
/// (isForegrounded:)` — platform-free, and therefore actually testable
/// (`FireflyAppTests`/`AppGraphTests` are macOS; this file is not). What
/// stays here is the two things only UIKit can answer: that this method
/// was called at all, and what `applicationState` says (§3.2 — the only
/// foreground signal that exists before a scene does).
///
/// The graph is handed over by `FireflyApp.init()`, which SwiftUI runs
/// BEFORE this method. If that ever stopped being true the consequence
/// is bounded and not silent: `graph` reads nil, the log line below says
/// so, and `FireflyApp.init()`'s own `prepareForRestoration()` call —
/// §3.1's deliberate second path — has already constructed the manager.
final class FireflyAppDelegate: NSObject, UIApplicationDelegate {
    @MainActor static var graph: AppGraph?

    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        guard let graph = FireflyAppDelegate.graph else {
            FileHandle.standardError.write(Data("[FireflyApp] didFinishLaunching with no graph yet\n".utf8))
            return true
        }
        // `.active` only — `.inactive` is a transient mid-transition
        // state on a normal launch and is treated as "not on screen"
        // everywhere else in this app (the scene-phase observers below).
        // A background relaunch reports `.background`, which is the
        // reading that matters here.
        graph.handleDidFinishLaunching(isForegrounded: application.applicationState == .active)
        return true
    }
}
#endif

@main
struct FireflyApp: App {
    /// THE composition root, constructed exactly once for the process —
    /// and, since "app: Try the demo" (docs/specs/A01-companion-app.md
    /// demo stack, owner ask 2026-09-15), rebuildable AT RUNTIME by
    /// `enterDemoMode()`/`leaveDemoMode()` below without leaving the
    /// process. See `AppRuntimeBundle.swift`'s own header comment for
    /// why a wholesale rebuild — not a second, parallel "demo view" — is
    /// the only honest way to do that, and why this single `@State`
    /// replaces what used to be a dozen separate ones (`graph`,
    /// `connect`, `inbox`, `radar`, `lineup`, `map`, `channelImport`,
    /// `settings`, `crew`, `membership`, `demoRunner`, `hasKnownRadio`,
    /// `hasCrew` all now live inside it, reassigned together in one
    /// step, which is what keeps a mode switch from ever showing a
    /// `RootView` built from half the OLD graph and half the NEW one).
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
    @State private var runtime: AppRuntimeBundle
    /// A02 §1.8 — `onOpenURL`'s parsed `firefly://crew…` payload.
    @State private var incomingCrewLink: CrewScanPayload?
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
    /// A03 §3.1 — see `FireflyAppDelegate`'s own doc comment. Held as a
    /// property wrapper because that is the only way to install an
    /// application delegate in a SwiftUI lifecycle app; nothing reads it.
    #if os(iOS)
    @UIApplicationDelegateAdaptor(FireflyAppDelegate.self) private var appDelegate
    #endif
    /// A03 §3.11.3 — the notification-centre delegate. Held as `@State`
    /// because `UNUserNotificationCenter` retains its delegate WEAKLY:
    /// an object created in `init()` and not stored anywhere would be
    /// deallocated immediately and every tap would route nowhere.
    #if canImport(UserNotifications)
    @State private var notificationTaps = NotificationTapRouter()
    #endif

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
        // history into fictional festival data. Cold-launch only — a
        // runtime mode switch (`enterDemoMode()`/`leaveDemoMode()` below)
        // never passes a `historyStore` override, on purpose (see
        // `AppRuntimeBundle.build`'s own doc comment on that parameter).
        //
        // Bench seam (`FireflyDebugCrewStateLaunch`, `#if DEBUG` only) —
        // applied HERE, before `AppGraph.init` replays the paired list
        // onto `ff_crew` and reads the crew profile, because both of
        // those are what these flags exist to change. A launch with
        // neither flag does nothing and logs nothing. Also cold-launch
        // only, for the same reason: these are bench overrides describing
        // how THIS PROCESS started, not something a runtime switch should
        // ever replay.
        for line in FireflyDebugCrewStateLaunch.apply() {
            FileHandle.standardError.write(Data((line + "\n").utf8))
        }
        #if targetEnvironment(simulator)
        var historyOverride: HistoryStore?
        if DemoLaunch.isRestoredRequested() {
            let seeded = HistoryStore.inMemory()
            DemoHistorySeed.seed(into: seeded)
            historyOverride = seeded
        }
        #else
        let historyOverride: HistoryStore? = nil
        #endif
        let runtime = AppRuntimeBundle.build(dependencies: .current(), historyStore: historyOverride,
                                              requestedScreen: DemoLaunch.requestedScreen(),
                                              hapticsFactory: Self.makeHaptics)
        _runtime = State(initialValue: runtime)
        // A03 §3.1 — both launch paths, in the order §3.1 specifies.
        //
        // `FireflyAppDelegate` is the PRIMARY: it is the only one of the
        // two Apple documents as running on a background relaunch. This
        // handover has to happen here, in `init()`, because SwiftUI runs
        // it before `didFinishLaunchingWithOptions`. `AppRuntimeBundle
        // .build` already called `graph.prepareForRestoration()` — the
        // BACKSTOP half of §3.1 — for us; it is idempotent with the
        // delegate's own call (at most one `CBCentralManager` is ever
        // constructed per graph; `BLECentralStore`'s own doc comment) and
        // a no-op on macOS's and the Simulator's stacks alike.
        #if os(iOS)
        FireflyAppDelegate.graph = runtime.graph
        #endif

        // A03 §3.11.3 — install the notification delegate NOW. A tap
        // that launched the app is delivered right after launch, so
        // wiring this later is the same as not wiring it at all. It
        // also has to be a delegate at all before `willPresent` can
        // stop foreground notifications being swallowed
        // (`NotificationTapRouter`'s own header). Re-pointed at whichever
        // graph is current by `enterDemoMode()`/`leaveDemoMode()` below —
        // the router itself is built once and never rebuilt, since
        // `UNUserNotificationCenter` retaining its delegate weakly means
        // a fresh instance on every switch would need re-installing too,
        // for no benefit over just updating where the one delegate routes.
        #if canImport(UserNotifications)
        let taps = NotificationTapRouter()
        taps.onDeepLink = { [graph = runtime.graph] url in graph.deepLinks.handle(url) }
        taps.install()
        _notificationTaps = State(initialValue: taps)
        #endif
    }

    #if os(iOS)
    private static func makeHaptics() -> any HapticSignaling { UIKitHapticSignaling() }
    #else
    // No Taptic Engine on a Mac — the honest answer, not a gap.
    private static func makeHaptics() -> any HapticSignaling { NoHapticSignaling() }
    #endif

    /// "Try the demo" (`CrewConnectPuckView`'s connect-step button,
    /// Settings' demo row) — the in-app entry point TestFlight reviewers
    /// and App Review use in place of a real Meshtastic radio. A no-op
    /// while already in demo mode (`DemoModeAction.requested(isDemoMode:)`
    /// never returns `.enterDemo` then), so a stray second tap before the
    /// button/row can re-render as "Leave the demo" does nothing rather
    /// than tearing down and rebuilding the SAME world.
    func enterDemoMode() async { await switchRuntime(to: .requested(isDemoMode: runtime.isDemoMode)) }

    /// "Leave the demo" (Settings' demo row, once inside it) — tears the
    /// demo world down and rebuilds the real stack
    /// (`AppDependencies.nonDemo()`: `.live()` on a device, `.stub()` in
    /// the Simulator with no `-FireflyDemo` launch argument of its own).
    /// The user's REAL state is untouched by construction, not by care
    /// taken here: demo mode's own dependencies (`.demoBundle()`) are
    /// disposable, in-memory, and never shared with `.live()`'s
    /// `SettingsStore`/`HistoryStore`/Keychain-backed stores in the first
    /// place (`docs/specs/A01-companion-app.md`'s "Demo isolation"
    /// section), so there is nothing demo-shaped to unwind on the way
    /// out — this just builds the ordinary real composition fresh, the
    /// same one a cold launch with no demo flag would.
    func leaveDemoMode() async { await switchRuntime(to: .requested(isDemoMode: runtime.isDemoMode)) }

    /// Both entry points above funnel through here: stop the OUTGOING
    /// bundle (`AppRuntimeBundle.stopObserving()`), build the requested
    /// one, re-point the notification router at its graph, and install
    /// it. `RootView`'s `.id(runtime.id)` (in `body`, below) is what
    /// actually makes this feel like a relaunch to the rest of the app —
    /// see `AppRuntimeBundle`'s own header comment for why a wholesale
    /// rebuild, not an in-place mutation, is the only honest way to
    /// switch dependency graphs at runtime.
    private func switchRuntime(to action: DemoModeAction) async {
        let outgoing = runtime
        await outgoing.stopObserving()
        let incoming = AppRuntimeBundle.build(dependencies: action.dependencies,
                                               requestedScreen: action.requestedScreen,
                                               hapticsFactory: Self.makeHaptics)
        #if os(iOS)
        FireflyAppDelegate.graph = incoming.graph
        #endif
        #if canImport(UserNotifications)
        notificationTaps.onDeepLink = { [graph = incoming.graph] url in graph.deepLinks.handle(url) }
        #endif
        runtime = incoming
    }

    var body: some Scene {
        WindowGroup {
            // One argument per line (SHOULD-FIX 4): every slice that adds
            // a `RootView` dependency appends its own line here instead
            // of editing this call's single line, so sibling slices'
            // hunks land as pure insertions and never collide.
            RootView(
                connect: runtime.connect,
                settings: runtime.settings,
                channelImport: runtime.channelImport,
                client: runtime.graph.dependencies.client,
                inbox: runtime.inbox,
                radar: runtime.radar,
                lineup: runtime.lineup,
                scanner: runtime.graph.dependencies.scanner,
                demoRunner: runtime.demoRunner,
                initialDemoScreen: runtime.requestedScreen,
                flareTakeover: runtime.graph.flareTakeover,
                pairing: runtime.graph.crewPairing,
                // Map tab slice's own hunk — one view model, built once
                // by the graph like every other destination here
                // (`AppGraph.makeMapViewModel()`'s own doc comment), and
                // two thin action closures over `radar`/tab selection
                // rather than plumbing `graph` itself into `RootView`.
                map: runtime.map,
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
                mapFind: { nodeID in runtime.radar.startFind(targetNodeID: nodeID) },
                mapMessage: { _ in },
                // "app: five-tab bar per design".
                hasKnownRadio: runtime.hasKnownRadio,
                // A02 §2/§3/§5/§6.1.
                crew: runtime.crew,
                membership: runtime.membership,
                hasCrew: runtime.hasCrew,
                incomingCrewLink: $incomingCrewLink,
                // A03 §3.11.3 — where a tapped notification wants to go.
                deepLinks: runtime.graph.deepLinks,
                // A03 §3.10 — the graph's ONE notification seam, read by
                // Diagnostics for its authorization state.
                notifications: runtime.graph.notifications,
                // "app: Try the demo" — the connect-step button
                // (`CrewConnectPuckView`) and Settings' demo row both
                // read/act through these two rather than reaching
                // `AppRuntimeBundle`/`AppDependencies` themselves, same
                // "the screen only ever sees the one seam it needs"
                // convention every other closure on this call already
                // follows (`mapFind`/`mapMessage` just above).
                isDemoMode: runtime.isDemoMode,
                onTryDemo: { Task { await enterDemoMode() } },
                onLeaveDemo: { Task { await leaveDemoMode() } }
            )
            // `AppRuntimeBundle.id`'s own doc comment: forces SwiftUI to
            // discard and reconstruct `RootView`'s `@State` (and restart
            // every `.task` chained below) on a mode switch, rather than
            // diffing new view models into a `RootView` that still
            // thinks it is showing whatever the PREVIOUS graph was on.
            .id(runtime.id)
            .preferredColorScheme(.dark)
            // A02 §1.8 — `firefly://crew?v=1&code=…&name=…`. Anything
            // that doesn't classify as a crew link/bare code is dropped
            // silently here (Join's own scanner already renders "That's
            // not a Firefly crew code" for a payload a human actually
            // typed/scanned; a malformed system Open URL call has no
            // screen to show that message ON).
            // ONE `.onOpenURL`, not two (REVIEW, PR #310 rebase): A02
            // §1.8's crew links and A03 §3.11.3's notification routing
            // tokens share the `firefly` scheme, and SwiftUI does not
            // promise to run every `onOpenURL` in a hierarchy — a second
            // one is a coin toss over which link type works. The two
            // vocabularies are disjoint by construction
            // (`FireflyDeepLink.route(for:)` returns nil for anything but
            // `find`/`thread`, and `CrewScanPayload.classify` returns
            // `.unrecognized` for those), so trying A03's router first
            // and falling through is total and unambiguous.
            .onOpenURL { url in
                guard !runtime.graph.deepLinks.handle(url) else { return }
                switch CrewScanPayload.classify(url.absoluteString) {
                case .crewLink(let link):
                    incomingCrewLink = .crewLink(link)
                case .bareCode(let code):
                    incomingCrewLink = .bareCode(code)
                case .meshtasticChannelLink, .unrecognized:
                    break
                }
            }
            // M2: `AppGraph.setForegrounded(_:)` is the one thing that
            // decides "takeover, or a local notification instead"
            // (`handleInboundFlare`'s own doc comment) — `.active` is the
            // only phase that means "the user can actually see the
            // screen right now"; `.inactive` (a transient state, e.g. an
            // incoming call or the app switcher) is treated the same as
            // `.background` rather than as foregrounded, since neither
            // one means the screen is what the user is looking at.
            .onChange(of: scenePhase) { _, newPhase in
                runtime.graph.setForegrounded(newPhase == .active)
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
            //
            // `.task(id: runtime.id)`, NOT a bare `.task` — this is the
            // one thing that actually starts the newly-built graph after
            // a demo-mode switch, and MEASURED (a device screen recording
            // pinned this down, plus explicit `os_log` tracing across
            // several bench runs), a bare `.task` on a view chain that
            // also carries `.id(runtime.id)` does NOT reliably restart
            // when that id changes: `RootView`'s own `@State` does reset
            // (a fresh `CrewWelcome`/`showCrewOnboarding` render proves
            // that much), but `.task`'s own cancel-and-relaunch tracks
            // this MODIFIER's position in the tree, not `.id()` applied
            // several modifiers earlier in the same chain. Without this,
            // `switchRuntime` swaps `runtime` and rebuilds a real,
            // independent `AppGraph`/`DemoRunner` — logged and confirmed
            // — but NOTHING ever calls `.start()` on it: the app sits
            // forever on whatever `RootLaunchPlan.findWithCrewWelcome`
            // raised (a demo `hasKnownRadio`/`hasCrew` are honestly both
            // false pre-`DemoRunner.start()`), because `RootView
            // .runInitialDemoScreen()`'s own `await demoRunner
            // .waitUntilStarted()` polls `isStarted` forever and it is
            // never flipped. `id:` is the explicit, documented seam
            // SwiftUI gives for exactly this — "restart this task when
            // this value changes" — rather than a hope riding on an
            // ancestor's `.id()`.
            .task(id: runtime.id) {
                // A03 §3.2 — SEED the foreground flag from the initial
                // `scenePhase` before anything can deliver a packet.
                // `.onChange(of: scenePhase)` below does not fire for an
                // initial value, and `AppGraph.isForegrounded` now
                // starts `false` (the honest default for a launch that
                // may have begun in the background). Without this seed a
                // NORMAL launch would sit at `false` until the user
                // backgrounded and returned, and a FLARE arriving in
                // between would notify instead of taking over.
                //
                // This `.task` is attached to the scene's own content,
                // so reaching it means a scene exists — which is
                // precisely the signal a CoreBluetooth background
                // relaunch does NOT have, and why `false` has to be the
                // default rather than something seeded here.
                runtime.graph.setForegrounded(scenePhase == .active)
                await runtime.graph.start()
                await runtime.demoRunner?.start()
                #if DEBUG
                // `-FireflyDebugNotify <kind>` — the notification-tap
                // repro seam (`FireflyDebugNotifyLaunch`'s own header).
                // DEBUG-only on BOTH sides: the parser returns nil in a
                // Release build and these call sites do not exist there
                // either.
                //
                // Only the PERMISSION is taken here, while the app is
                // foregrounded and the system alert can be answered
                // (§3.11.5's own rule, and the reason this cannot happen
                // from the background hook below). The notification
                // itself is scheduled when the app BACKGROUNDS.
                if FireflyDebugNotifyLaunch.requestedKind() != nil,
                   let sender = runtime.graph.notifications as? UNNotificationSending {
                    _ = await sender.requestAuthorization()
                    await sender.registerCategories()
                }
                #endif
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
                        await runtime.graph.handleScenePhaseChange(.foreground)
                    case .background:
                        await runtime.graph.handleScenePhaseChange(.background)
                        #if DEBUG
                        // The repro seam's actual schedule, fired from
                        // the BACKGROUND transition rather than a fixed
                        // delay after launch. Measured, not guessed: a
                        // launch-relative delay races the test's own
                        // Home press (a slow install/launch delivers the
                        // notification while the app is still in front,
                        // where the banner auto-dismisses with nothing
                        // left to tap) and produced two different
                        // flakes before this moved here. Scheduling off
                        // the event that MUST have happened first
                        // removes the race instead of widening a
                        // timeout against it.
                        if let kind = FireflyDebugNotifyLaunch.requestedKind(),
                           let plan = FireflyDebugNotifyLaunch.plan(for: kind),
                           let sender = runtime.graph.notifications as? UNNotificationSending {
                            await sender.scheduleDebugNotification(plan, after: 3)
                        }
                        #endif
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
