//
//  FindSegment.swift — Find tab slice (owner decision, 2026-09-13):
//  "combine the Radar and Map tabs into ONE tab named Find. Inside
//  Find, a segmented control at the top: Radar · Map · Field... Radar
//  is the default segment." See docs/specs/A01-companion-app.md,
//  "Navigation", for the decision date and reasoning (one tab-bar
//  question instead of two, bigger targets for gloved hands).
//
//  Standalone, SwiftUI/FireflyModel-free — same reason
//  `MoreScreenNavigation.swift` is its own file (that file's own header
//  comment): `FireflyAppTests` can test the actual start/stop RULE
//  below without pulling in `FindScreen.swift`'s SwiftUI/Theme
//  dependency, or a real `RadarViewModel`/`MapViewModel` at all.
//
enum FindSegment: String, CaseIterable, Identifiable {
    case radar = "Radar"
    case map = "Map"
    case field = "Field"
    var id: String { rawValue }
}

/// The minimal shape `RadarViewModel`/`MapViewModel` already have:
/// both declare exactly these two methods, idempotent, per their own
/// doc comments (`RadarViewModel.observe()`/`.stopObserving()`,
/// `MapViewModel.observe()`/`.stopObserving()`). Naming the shape here
/// instead of importing either type keeps this file dependency-free —
/// `FindScreen.swift` is where the real view models are made to
/// conform (trivial empty extensions; both already have the methods).
///
/// `@MainActor`: both real conformers are `@MainActor @Observable`
/// classes (A01's threading model) — a non-isolated protocol here
/// would make Swift 6's strict concurrency reject their conformance as
/// "crosses into main actor-isolated code and can cause data races".
@MainActor
protocol FindSegmentObserving: AnyObject {
    func observe()
    func stopObserving()
    /// Stops this segment's own pump/subscriptions because ANOTHER Find
    /// segment (or another tab entirely) is now visible — NOT a full
    /// teardown. Defaults to `stopObserving()` for a segment with
    /// nothing else worth preserving (Map has no FIND session of its
    /// own); `RadarViewModel` overrides this to leave an active FIND
    /// session running. Owner decision, 2026-09-13 ("FIND keeps running
    /// across Find segments"): FIND ends only on explicit cancel or on
    /// backgrounding (`AppGraph.stop()`, which calls the REAL
    /// `stopObserving()` directly) — never merely because Radar, or the
    /// Find tab itself, is not what's on screen right now.
    func pauseObserving()
}

extension FindSegmentObserving {
    func pauseObserving() { stopObserving() }
}

/// The start/stop rule: exactly one of `radar`/`map` observes at a
/// time, matching whichever `FindSegment` is showing — "only the
/// visible segment's view model observes/pumps" (owner decision). Map
/// and Field share the ONE `MapViewModel` (same as `MapTabView`'s old
/// internal Field/GPS toggle always did — switching between THOSE two
/// never stopped/started its pump, only entering/leaving Map/Field as a
/// whole did — `MapTabView.swift`'s own header comment), so this is a
/// two-way choice, not three.
///
/// Deliberately NOT responsible for backgrounding/foregrounding.
/// Radar's background lifecycle stays owned by `AppGraph.start()`/
/// `.stop()`, gated on `SettingsKey.backgroundConnectEnabled`
/// (`AppGraphViewModelLifecycleTests.testBackgroundingWithBackgroundConnectOnLeavesRadarRunning`
/// pins "background connect ON means Radar keeps running in the
/// background too") — a real, already-tested product decision this
/// type must not fight with a second, UI-level scenePhase handler that
/// can't see that setting. Map's own background handling
/// (`MapTabView`'s `.onChange(of: scenePhase)`, PR #294) is untouched
/// and still applies whenever Map/Field is the active segment — this
/// type only answers "which segment is on screen right now", called
/// from `RootView.applyFindLifecycle()`, off `RootView`'s own
/// `selection`/`findSegment` state. NOT from `FindScreen`'s own
/// `onAppear`/`onDisappear`: that view is a `RootView.detail(for:)`
/// destination and is hit by the `NavigationSplitView` detail-column
/// remount `ConnectScreen.swift`/`RadarView.swift` document — measured
/// on this branch, it fires `onAppear`, `onAppear`, `onDisappear` at
/// launch, which ran `stopAll()` last and left both view models
/// stopped on the screen the app had just landed on. See
/// `RootView.applyFindLifecycle()`'s own comment for that trace.
///
/// The FOREGROUND half of the same rule lives in `AppGraph.start()`,
/// which restores Radar's pump only if it was actually running when
/// `stop()` tore the graph down (PR #298 review) — a view-level
/// `scenePhase` handler could not do it without racing that `Task`.
@MainActor
enum FindLifecycle {
    /// Owner decision, 2026-09-13 ("FIND keeps running across Find
    /// segments"): switching AWAY from Radar (or away from Find
    /// entirely, in `stopAll` below) uses `pauseObserving()`, never the
    /// full `stopObserving()` — an active FIND session on Radar must
    /// survive a segment switch. Map has no FIND concept of its own, so
    /// its `pauseObserving()` (the protocol's default) is
    /// indistinguishable from `stopObserving()` either way.
    static func apply(segment: FindSegment, radar: any FindSegmentObserving, map: any FindSegmentObserving) {
        if segment == .radar {
            map.pauseObserving()
            radar.observe()
        } else {
            radar.pauseObserving()
            map.observe()
        }
    }

    /// Pauses BOTH — `FindScreen`'s own `.onDisappear`, for when Find
    /// itself loses the tab (another destination selected). Owner
    /// decision, 2026-09-13 reverses part of #298 here too: leaving the
    /// Find tab for Inbox/Lineup/More must NOT end an active FIND
    /// session either — "keep it running while the app is
    /// foregrounded... it ends on explicit cancel or on backgrounding."
    /// Never called for backgrounding (this type's own doc comment) —
    /// that path is `AppGraph.stop()`, which calls `radar.stopObserving()`
    /// directly and DOES end FIND there.
    static func stopAll(radar: any FindSegmentObserving, map: any FindSegmentObserving) {
        radar.pauseObserving()
        map.pauseObserving()
    }
}
