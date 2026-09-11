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
    let onFind: (UInt32) -> Void
    let onMessage: (UInt32) -> Void

    init(model: MapViewModel, initialSegment: MapSegment = .field, onFind: @escaping (UInt32) -> Void,
         onMessage: @escaping (UInt32) -> Void) {
        _model = State(initialValue: model)
        _segment = State(initialValue: initialSegment)
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
                FieldMapView(projection: model.fieldMapProjection(radiusPx: 160, marginPx: 16),
                             onSelect: { model.select(nodeID: $0) }, selectedCrewID: model.selectedCrewID)
            case .gps:
                GPSMapView(model: model, onSelect: { model.select(nodeID: $0) },
                           onDeselect: { model.select(nodeID: nil) }, onFind: onFind, onMessage: onMessage)
            }
        }
        .background(Color.ffBackground)
        .onAppear { model.observe() }
        .accessibilityIdentifier("Screen.Map")
    }
}
