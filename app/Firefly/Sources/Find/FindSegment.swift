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
/// from `FindScreen`'s `onAppear`/`onDisappear`/segment-change. See
/// that file's own header comment for why driving this from
/// `.onChange(of: segment)` rather than each segment's own view
/// appearing/disappearing sidesteps the `NavigationSplitView`
/// detail-column remount hazard `ConnectScreen.swift`/`RadarView.swift`
/// document (Find, not Radar, is the `RootView.Destination` case
/// exposed to it now).
@MainActor
enum FindLifecycle {
    static func apply(segment: FindSegment, radar: any FindSegmentObserving, map: any FindSegmentObserving) {
        if segment == .radar {
            map.stopObserving()
            radar.observe()
        } else {
            radar.stopObserving()
            map.observe()
        }
    }

    /// Stops BOTH — `FindScreen`'s own `.onDisappear`, for when Find
    /// itself loses the tab (another destination selected). Never
    /// called for backgrounding (this type's own doc comment).
    static func stopAll(radar: any FindSegmentObserving, map: any FindSegmentObserving) {
        radar.stopObserving()
        map.stopObserving()
    }
}
