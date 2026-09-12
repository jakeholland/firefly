//
//  MapTabView.swift — Map tab slice: the Field/GPS segmented control
//  the owner's design canvas calls for. Owns the one `MapViewModel`
//  this destination renders (`AppGraph.makeMapViewModel()` builds it
//  once, same "one view model per destination, built by the graph"
//  rule every other screen in this app follows).
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
    @State private var segment: MapSegment
    /// `SettingsViewModel.colorblindPalette`, read fresh by `RootView`
    /// on every redraw — same convention `RadarView`/`ConnectScreen`
    /// already follow for their own crew colors (PR #283 review,
    /// SHOULD-FIX 6).
    let colorblind: Bool
    let onFind: (UInt32) -> Void
    let onMessage: (UInt32) -> Void

    init(model: MapViewModel, initialSegment: MapSegment = .field, colorblind: Bool,
         onFind: @escaping (UInt32) -> Void, onMessage: @escaping (UInt32) -> Void) {
        _model = State(initialValue: model)
        _segment = State(initialValue: initialSegment)
        self.colorblind = colorblind
        self.onFind = onFind
        self.onMessage = onMessage
    }

    var body: some View {
        VStack(spacing: 0) {
            Picker("Map view", selection: $segment) {
                ForEach(MapSegment.allCases) { Text($0.rawValue.uppercased()).tag($0) }
            }
            .pickerStyle(.segmented)
            .padding(12)
            .accessibilityIdentifier("Map.Segment")

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
        .onAppear { model.observe() }
        .onDisappear { model.stopObserving() }
        .accessibilityIdentifier("Screen.Map")
    }
}
