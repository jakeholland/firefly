//
//  AppRuntimeBundle.swift — everything `FireflyApp.init()` used to build
//  inline, as one value `FireflyApp` can hand to `RootView` and, unlike
//  the individual `@State` properties that used to hold each piece
//  separately, can also REBUILD wholesale — the seam the in-app "Try the
//  demo"/"Leave the demo" entry points (`docs/specs/A01-companion-app.md`
//  demo stack, owner ask 2026-09-15: reviewers and puck-less users need
//  a way in with no launch argument) need and the original one-`@State`
//  -per-piece shape did not have.
//
//  WHY A RUNTIME REBUILD, NOT A SECOND PARALLEL "DEMO VIEW MODEL"
//  IMPLEMENTATION. `AppGraph`'s own header comment is the whole
//  architecture: ONE `AppDependencies`, ONE client, ONE set of `ff_*`
//  contexts, every view model built from THAT graph — "nothing below
//  this line may call `AppDependencies.current()` for itself". Demo mode
//  is not a display mode layered on top of the live graph (`DemoRunner`'s
//  own header: it plays a scripted timeline through the SAME
//  `CoreStore`/`ff_crew`/`ff_feed` bridges the live graph uses, and
//  nothing downstream can tell a `DemoMeshtasticClient` from a real
//  one). So "switch to the demo world" can only honestly mean "build a
//  second, independent composition — dependencies, graph, every view
//  model — the same way `FireflyApp.init()` builds the FIRST one, and
//  make it the one `RootView` renders instead." That is what
//  `build(dependencies:...)` below does: it is `FireflyApp.init()`'s own
//  body, minus the one-time-per-process pieces (the `FireflyDebugCrewStateLaunch`
//  bench seam, `-FireflyDemoRestored` history seeding — both belong to
//  how THIS PROCESS started, not to a mid-session switch), factored out
//  so it can run more than once.
//
//  THE ALTERNATIVE THIS REPLACES: persist a `demoModeRequested` flag
//  somewhere durable and terminate/relaunch the process so `init()` runs
//  again from scratch. Rejected — a `-FireflyDemo`-shaped flag has to
//  live in `UserDefaults` to survive a real relaunch, which is exactly
//  the surface `docs/specs/A01-companion-app.md`'s "Demo isolation"
//  section (`.demo()` always gets a disposable in-memory store) says
//  demo mode must never touch; calling `exit()` to force a relaunch is
//  an App Store anti-pattern with no guaranteed clean re-launch, and it
//  would make "Leave the demo" a worse experience than "Try the demo"
//  (a reviewer who taps it would watch the app visibly die and restart
//  to get back to a screen they were just looking at). Rebuilding the
//  graph AND `RootView`'s own identity in place — `FireflyApp.body`'s
//  `.id(runtime.id)`, which forces SwiftUI to discard and reconstruct
//  `RootView`'s `@State` exactly as a fresh window would — gets the same
//  honest "nothing carries over that shouldn't" property (a fresh
//  `RootView`, a fresh `AppGraph`, a fresh `CoreStore`) without ever
//  leaving the process or touching persistence. It is what "relaunch the
//  graph/root view" means in this codebase's own vocabulary, not a
//  euphemism for it.
//
//  WHAT DOES NOT GET TORN DOWN, ON PURPOSE, AND WHY THAT IS AN ACCEPTED
//  GAP RATHER THAN A SILENT ONE. `AppGraph.stop()`'s own comment already
//  documents that it reaches only the graph's OWN subscriptions plus
//  `radar` (the one view model it keeps a reference to for `handlePong`)
//  — `connect`/`inbox`/`lineup`/`map`/`crew`/`settings` are, in that
//  file's own words, "process-lifetime singletons" nothing there stops.
//  `stopObserving()` below closes the gap as far as this app's own
//  public API allows — every view model that exposes a `stopObserving()`
//  (`connect`, `radar`, `inbox`, `membership`, `map`) gets it called on
//  the OUTGOING bundle before the new one is installed — but `crew`
//  (`CrewController`) and `settings`/`lineup` have no such method today,
//  so a `Task` either of them may be running keeps running against a
//  disconnected client until it finishes or `self` cannot be captured
//  weakly and simply idles. Bounded: this switch is a rare, user-driven,
//  at-most-a-handful-of-times-per-session action, not a hot loop — the
//  fix, if it is ever worth taking, is adding `stopObserving()` to the
//  two straggling view models, not redesigning this seam.
//
import FireflyMesh
import FireflyModel
import Foundation
#if canImport(UserNotifications)
import UserNotifications
#endif

/// Everything `RootView` needs from one dependency graph, bundled so
/// `FireflyApp` can hold exactly one `@State` for "the graph currently
/// on screen" instead of a dozen separate ones that would otherwise have
/// to be reassigned in lockstep. `let`, not `@Observable`/classes — this
/// type is a plain value that gets REPLACED wholesale on a mode switch,
/// never mutated in place (every property inside it is itself the
/// class/view-model instance that owns its own observable state, same
/// as `FireflyApp`'s old individual `@State`s held).
@MainActor
struct AppRuntimeBundle {
    let graph: AppGraph
    let connect: ConnectViewModel
    let inbox: InboxViewModel
    let radar: RadarViewModel
    let lineup: LineupViewModel
    let map: MapViewModel
    let channelImport: ChannelImportViewModel
    let settings: SettingsViewModel
    let crew: CrewController
    let membership: any CrewMembershipProviding
    let demoRunner: DemoRunner?
    let hasKnownRadio: Bool
    let hasCrew: Bool
    /// `-FireflyDemoScreen <name>`'s parsed value on the FIRST build
    /// (straight from `DemoLaunch.requestedScreen()`, for the existing
    /// screenshot scripts), or `DemoModeAction.requestedScreen` on every
    /// later rebuild this switch performs. `RootView` reads this once,
    /// same as it always has — the field just now has two possible
    /// origins instead of one.
    let requestedScreen: String?
    /// Changes on every `build(...)` call, including the very first one.
    /// `FireflyApp.body` applies this as `RootView`'s own `.id(...)` —
    /// forcing SwiftUI to discard `RootView`'s `@State` (`selection`,
    /// `showCrewOnboarding`, `morePath`, everything) and its `.task`s
    /// along with it, rather than diffing the new view models into a
    /// `RootView` instance that still thinks it is showing whatever tab
    /// the PREVIOUS graph was on. Without this, "Leave the demo" would
    /// leave `selection == .find` pointed at a `RadarViewModel` that no
    /// longer has a `DemoRunner` behind it, showing an honestly-empty
    /// Radar instead of returning to the welcome/connect step this
    /// feature's own acceptance criteria ask for.
    let id = UUID()

    /// `true` exactly when `graph.dependencies.client` is a
    /// `DemoMeshtasticClient` — the SAME test `demoRunner != nil` already
    /// is (see `demoRunner`'s own doc comment on `RootView`), spelled out
    /// as its own property because `DemoModeAction.requested(isDemoMode:)`
    /// needs to ask the question without constructing a `DemoRunner?`
    /// itself.
    var isDemoMode: Bool { demoRunner != nil }

    /// `FireflyApp.init()`'s entire composition body, factored out so it
    /// can run more than once per process. Builds a full, independent
    /// `AppGraph` plus every view model `RootView` needs from
    /// `dependencies` — never anything already alive in a bundle being
    /// replaced, so the caller (`FireflyApp`) is the one place that has
    /// to remember to tear the OLD bundle down (`stopObserving()` below)
    /// before installing this one.
    ///
    /// - Parameters:
    ///   - dependencies: which stack this bundle runs — `.current()` for
    ///     the process's own cold-launch composition,
    ///     `DemoModeAction.dependencies` for a runtime switch.
    ///   - notifications: forwarded to `AppGraph.init` unchanged; a
    ///     parameter (rather than always `UNNotificationSending()`) only
    ///     so a future test can inject a fake the same way `AppGraph`
    ///     itself already allows.
    ///   - skipLaunchAutoConnectUnderXCTest: forwarded to `AppGraph.init`
    ///     unchanged — see that initializer's own doc comment. `true` on
    ///     every call `FireflyApp` makes (both the cold-launch build and
    ///     a runtime switch: this IS the live process either way, never
    ///     an app-hosted test's own composition).
    ///   - historyStore: M3's `-FireflyDemoRestored` override seam,
    ///     forwarded unchanged — `nil` on every runtime switch (the
    ///     history-restore treatment is a cold-launch-only screenshot
    ///     seam; entering demo mid-session has no "restored" story to
    ///     tell).
    ///   - requestedScreen: this bundle's own `requestedScreen` — see
    ///     that property's doc comment for where each caller's value
    ///     comes from.
    ///   - hapticsFactory: builds the platform haptics
    ///     (`UIKitHapticSignaling`/`NoHapticSignaling`) — a closure
    ///     rather than `FireflyApp` computing it once and passing the
    ///     same instance to every `build(...)` call, so this file stays
    ///     free of the `#if os(iOS)` split (`FireflyApp.swift` already
    ///     owns that line) while still giving every rebuild a working
    ///     haptics engine of its own, matching what `init()` always did.
    static func build(dependencies: AppDependencies,
                       notifications: any NotificationSending = UNNotificationSending(),
                       skipLaunchAutoConnectUnderXCTest: Bool = true,
                       historyStore: HistoryStore? = nil,
                       requestedScreen: String?,
                       hapticsFactory: () -> any HapticSignaling) -> AppRuntimeBundle {
        let graph = AppGraph(dependencies: dependencies, notifications: notifications,
                              skipLaunchAutoConnectUnderXCTest: skipLaunchAutoConnectUnderXCTest,
                              historyStore: historyStore)
        graph.prepareForRestoration()

        let connectVM = graph.makeConnectViewModel()
        let importVM = ChannelImportViewModel(client: graph.dependencies.client)
        // REVIEW FIX (PR #331 independent review, BLOCKING) —
        // `CrewStoreSelection`'s own header comment: this used to wire the
        // REAL, persistent `CrewSnapshotKeychainStore()`/`CrewHiddenStore()`
        // unconditionally, for the demo composition too. Pinned by
        // `CrewStoreSelectionTests`.
        let crewVM = CrewController(
            client: graph.dependencies.client,
            profileStore: graph.crewProfileStore,
            snapshotStore: CrewStoreSelection.snapshotStore(for: dependencies),
            hiddenStore: CrewStoreSelection.hiddenStore(for: dependencies))
        crewVM.onProfileChanged = { [graph] in graph.syncCrewMembershipWithProfile() }

        let lineupVM = graph.makeLineupViewModel()
        let indexProvider: any AlmanacIndexProviding = graph.dependencies.client is DemoMeshtasticClient
            ? DemoAlmanacIndexProvider()
            : AlmanacIndexProvider()
        let settingsVM = SettingsViewModel.makeObserving(store: graph.dependencies.store,
                                                          channelImport: importVM,
                                                          client: graph.dependencies.client,
                                                          clearHistory: { graph.inboxProvider.clearAll() },
                                                          location: graph.dependencies.location,
                                                          lineup: lineupVM,
                                                          indexProvider: indexProvider)
        let inboxVM = graph.makeInboxViewModel()
        let haptics = hapticsFactory()
        let radarVM = graph.makeRadarViewModel(haptics: haptics)
        let mapVM = graph.makeMapViewModel()
        graph.flareTakeover.setHaptics(haptics)

        let hasKnownRadio = graph.dependencies.store.string(.lastPeripheralID) != nil
            || FireflyAutoConnectLaunch.requestedPeripheralName() != nil
        let hasCrew = crewVM.hasCrew

        let demoRunner: DemoRunner?
        if let demoClient = graph.dependencies.client as? DemoMeshtasticClient,
           let demoLocation = graph.dependencies.location as? DemoLocationProvider,
           let demoHeading = graph.dependencies.heading as? DemoHeadingProvider {
            demoRunner = DemoRunner(graph: graph, client: demoClient, location: demoLocation, heading: demoHeading,
                                     connect: connectVM, inbox: inboxVM, radar: radarVM)
        } else {
            demoRunner = nil
        }

        return AppRuntimeBundle(graph: graph, connect: connectVM, inbox: inboxVM, radar: radarVM,
                                 lineup: lineupVM, map: mapVM, channelImport: importVM, settings: settingsVM,
                                 crew: crewVM, membership: graph.crewMembership, demoRunner: demoRunner,
                                 hasKnownRadio: hasKnownRadio, hasCrew: hasCrew, requestedScreen: requestedScreen)
    }

    /// Tears down everything this bundle owns that would otherwise keep
    /// running against a client the app no longer shows anywhere.
    /// `graph.stop()` disconnects the client and cancels every
    /// subscription `AppGraph` itself owns, PLUS `radar`'s (the one view
    /// model it keeps its own weak reference to — `makeRadarViewModel`'s
    /// own `radar = model` line) and `crewMembership`'s — see that
    /// method's own comment. `connect`/`inbox`/`map` are, in that same
    /// comment's own words, owned by `FireflyApp`, not the graph, so
    /// this is the one place left to stop them; `stopObserving()` is
    /// harmless to call twice (`ConnectViewModel.observe()`'s own
    /// "already observing — no-op" logging shows the pattern), so
    /// calling it here even though `graph.stop()` will ALSO reach radar
    /// internally costs nothing and keeps this list honestly complete on
    /// its own rather than relying on a reader to know which three of
    /// the four `graph.stop()` skips. Called on the OUTGOING bundle, by
    /// `FireflyApp`, before a mode switch installs its replacement — see
    /// this type's own header comment for what this does NOT reach
    /// (`crew`, `settings`, `lineup` have no `stopObserving()` today).
    func stopObserving() async {
        connect.stopObserving()
        inbox.stopObserving()
        map.stopObserving()
        await graph.stop()
    }
}
