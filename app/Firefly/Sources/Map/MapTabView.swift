//
//  MapTabView.swift — Find tab slice (owner decision, 2026-09-13): the
//  Field/GPS content that used to be its own "Map" tab with its own
//  internal segmented control. That control is gone — `FindScreen`'s
//  ONE segmented control (Radar · Map · Field) now drives `segment`
//  from outside, as a plain parameter, so this view no longer owns
//  which of Field/GPS it shows. Everything else — the one
//  `MapViewModel` this destination renders (`AppGraph.makeMapViewModel()`
//  builds it once, same "one view model per destination, built by the
//  graph" rule every other screen in this app follows) and its
//  `onAppear`/`onDisappear`/`scenePhase` lifecycle below — is UNCHANGED.
//
import FireflyModel
import SwiftUI

enum MapSegment: String, CaseIterable, Identifiable {
    case field = "Field"
    case gps = "GPS"
    var id: String { rawValue }
}

struct MapTabView: View {
    @State private var model: MapViewModel
    /// Which of Field/GPS to show — chosen by `FindScreen`'s own
    /// segmented control now, not this view's own (removed) Picker.
    /// A plain `let`, not `@Binding`: nothing in here ever needs to
    /// WRITE it any more, only read it.
    let segment: MapSegment
    /// `SettingsViewModel.colorblindPalette`, read fresh by `RootView`
    /// on every redraw — same convention `RadarView`/`ConnectScreen`
    /// already follow for their own crew colors (PR #283 review,
    /// SHOULD-FIX 6).
    let colorblind: Bool
    let onFind: (UInt32) -> Void
    let onMessage: (UInt32) -> Void
    @Environment(\.scenePhase) private var scenePhase
    /// Whether this view is the tab actually on screen right now, as
    /// opposed to a deselected tab the `TabView` is still holding on to.
    /// Kept by `.onAppear`/`.onDisappear` — the only two events that
    /// genuinely define it — and read by the `scenePhase` observer
    /// below, whose own comment has the measurement that made it
    /// necessary.
    @State private var isOnScreen = false

    init(model: MapViewModel, segment: MapSegment, colorblind: Bool,
         onFind: @escaping (UInt32) -> Void, onMessage: @escaping (UInt32) -> Void) {
        _model = State(initialValue: model)
        self.segment = segment
        self.colorblind = colorblind
        self.onFind = onFind
        self.onMessage = onMessage
    }

    var body: some View {
        VStack(spacing: 0) {
            switch segment {
            case .field:
                FieldMapView(model: model, onSelect: { model.select(nodeID: $0) },
                             selectedCrewID: model.selectedCrewID, colorblind: colorblind)
            case .gps:
                GPSMapView(model: model, onSelect: { model.select(nodeID: $0) },
                           onDeselect: { model.select(nodeID: nil) }, onFind: onFind, onMessage: onMessage,
                           colorblind: colorblind)
            }
        }
        .background(Color.ffBackground)
        // PR #283 review, BLOCKING 3: `model.observe()` used to be a
        // harmless no-op here (`AppGraph.makeMapViewModel()` already
        // called it eagerly at graph construction, so the 1 Hz
        // recompute loop plus location/heading/connectivity
        // subscriptions ran for the WHOLE app session regardless of
        // which tab was on screen — a battery-conscious-polling
        // failure). `makeMapViewModel()` no longer starts it, so THIS
        // is now the one place it starts/stops, same
        // `.onAppear`/`.onDisappear` convention `ConnectScreen`'s own
        // screen-scoped `nearby` view model already follows.
        .onAppear { isOnScreen = true; model.observe() }
        .onDisappear { isOnScreen = false; model.stopObserving() }
        // Hardening QA pass: `.onDisappear` does NOT fire when the app
        // is backgrounded, and this app declares `UIBackgroundModes`
        // `bluetooth-central` + `location` — so with location sharing
        // on it genuinely keeps running rather than being suspended.
        // Backgrounding with the Map tab on screen therefore left the
        // 1 Hz `pinRefreshLoop` (plus the location/heading/connectivity
        // subscriptions and an `NWPathMonitor`) running against a map
        // nobody can see, for as long as the phone stayed in a pocket.
        // That is the same defect `AppGraph.stop()` now fixes for
        // Radar; it cannot fix it here, because the graph deliberately
        // keeps no reference to this screen-scoped view model
        // (`makeMapViewModel()`'s own doc comment).
        //
        // The foreground restart is gated on `isOnScreen`, and that
        // guard is MEASURED, not reasoned (PR #294 review). The
        // assumption it replaces — "if the tab is not showing, this
        // view is not in the hierarchy and nothing here runs at all" —
        // is only half true, and the false half is the dangerous one.
        // Measured on an iPhone 17 Pro simulator (iOS 26.5), instrumented
        // build, backgrounding via another app and foregrounding again:
        //
        //  * Map tab NEVER visited: `.onChange` does not fire at all.
        //    (The assumption holds here.)
        //  * Map tab visited and then switched away from: `.onAppear`/
        //    `.onDisappear` fire as expected, and this view then STAYS
        //    in the `TabView`'s hierarchy — `.onChange(of: scenePhase)`
        //    fires for both `.background` and `.active`. Restarting
        //    unconditionally on `.active` therefore woke the 1 Hz
        //    `pinRefreshLoop`, three subscriptions and the
        //    `NWPathMonitor` back up for a Map that is NOT on screen,
        //    with nothing to stop them again until the next
        //    background — reintroducing, off-screen, exactly the leak
        //    this block exists to close.
        //
        // `isOnScreen` is the honest record of which of those two cases
        // this view is in, kept by the very modifiers that define it.
        //
        // `.background` only, never `.inactive`: the latter also fires
        // for an app-switcher peek, a pulled-down Control Center and a
        // system alert, and tearing down three subscriptions plus an
        // `NWPathMonitor` for a half-second glance is its own churn.
        // This is the same edge `FireflyApp`'s own
        // `handleScenePhaseChange` uses, for the same reason.
        .onChange(of: scenePhase) { _, phase in
            switch phase {
            case .background: model.stopObserving()
            case .active: if isOnScreen { model.observe() }
            default: break
            }
        }
        // "app: Find tab — Radar · Map · Field segments" — `.contain`
        // is load-bearing here, not decorative: without it, this
        // VStack's OWN identifier ("Screen.Map") swallows
        // `FieldMapView`/`GPSMapView`'s own `"Screen.Map.Field"`/
        // `"Screen.Map.GPS"` identifier (measured empirically via a
        // UI-test diagnostic dump on `FindScreen`'s identical shape —
        // `FindScreen.swift`'s own comment on its own `.contain`).
        // Neither of these two identifiers was ever asserted alongside
        // its parent's before this change (the old five-tab test never
        // visited Map at all), so this was a live bug nothing had
        // caught yet.
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("Screen.Map")
    }
}
