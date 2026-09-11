//
//  GPSMapView.swift — Map tab slice: MapKit in Apple's dark style, real
//  festival geography (as far as the loaded `Festpack` states it —
//  polygons/points, never invented), crew pins with the SAME
//  freshness-based `CrewMapPinTreatment` the Field map draws, "you" as
//  the standard MapKit user-location dot + heading cone, and a selected-
//  crew card with distance/bearing/source/age + Find/Message.
//
//  Offline: this app does NOT pre-cache Apple Maps tiles — there is no
//  public API for a third-party `MKMapView`/SwiftUI `Map` to fetch and
//  persist Apple's own basemap tiles for guaranteed offline use
//  (`MKTileOverlay` exists for a THIRD-PARTY tile server, not Apple's
//  own renderer; Apple's Maps & Location Services terms license the OS
//  Settings > Maps > Offline Maps feature, which has no API surface any
//  app can draw from). So when the device has no network path, this
//  view honestly shows "GPS map needs data" instead of a blank/frozen
//  map or a fabricated "cached" claim — see `MapViewModel.offlineChipText`'s
//  own doc comment. The schematic `FieldMapView` is this feature's
//  actual guaranteed-offline path.
//
import FireflyModel
import MapKit
import SwiftUI

struct GPSMapView: View {
    let model: MapViewModel
    let onSelect: (UInt32) -> Void
    let onDeselect: () -> Void
    let onFind: (UInt32) -> Void
    let onMessage: (UInt32) -> Void
    @State private var cameraPosition: MapCameraPosition = .automatic

    var body: some View {
        ZStack(alignment: .top) {
            if let offlineChipText = model.offlineChipText {
                offlineFallback(offlineChipText)
            } else {
                mapContent
            }

            HStack {
                if let offlineChipText = model.offlineChipText {
                    chip(text: offlineChipText, tint: .ffStaleAmber)
                }
                Spacer()
            }
            .padding(12)
        }
        .safeAreaInset(edge: .bottom) {
            if let pin = model.selectedPin {
                selectedCard(pin)
            }
        }
        .accessibilityIdentifier("Screen.Map.GPS")
    }

    @ViewBuilder
    private var mapContent: some View {
        Map(position: $cameraPosition) {
            if let festpack = model.festpack {
                ForEach(festpack.features) { feature in
                    featureContent(feature, festpack: festpack)
                }
            }
            ForEach(model.pins) { pin in
                Annotation(pin.name, coordinate: CLLocationCoordinate2D(latitude: pin.latitude,
                                                                          longitude: pin.longitude)) {
                    crewAnnotation(pin)
                        .onTapGesture { onSelect(pin.id) }
                }
            }
            UserAnnotation()
        }
        .mapStyle(.standard(elevation: .flat, pointsOfInterest: .excludingAll))
        .preferredColorScheme(.dark)
        .onAppear { centerIfNeeded() }
        .onChange(of: model.myCoordinate) { _, _ in centerIfNeeded() }
    }

    @MapContentBuilder
    private func featureContent(_ feature: FestpackFeature, festpack: Festpack) -> some MapContent {
        let color = stageColor(feature, festpack) ?? Color.mapKind(feature.kind)
        let coords = feature.polygon.map { CLLocationCoordinate2D(latitude: $0.latitude, longitude: $0.longitude) }
        if coords.count >= 3 {
            MapPolygon(coordinates: coords)
                .foregroundStyle(color.opacity(0.18))
                .stroke(color, lineWidth: 1.5)
        } else if coords.count == 2 {
            MapPolyline(coordinates: coords)
                .stroke(color, lineWidth: 1.5)
        } else if let point = coords.first {
            Annotation(feature.label, coordinate: point) {
                Circle().fill(color).frame(width: 8, height: 8)
            }
        }
    }

    private func stageColor(_ feature: FestpackFeature, _ festpack: Festpack) -> Color? {
        guard feature.kind == .stage, let stageID = feature.stageID,
              let stage = festpack.stages.first(where: { $0.id == stageID }) else { return nil }
        return Color.mapFeature(hex: stage.colorHex)
    }

    @ViewBuilder
    private func crewAnnotation(_ pin: CrewMapPin) -> some View {
        let color = Color.mapCrew(colorIndex: pin.colorIndex)
        ZStack {
            switch pin.treatment {
            case .live:
                Circle().fill(color).frame(width: 20, height: 20)
            case .staleRing, .lostRing:
                Circle().strokeBorder(style: StrokeStyle(lineWidth: 2, dash: [3, 2])).foregroundStyle(color)
                    .frame(width: 20, height: 20)
            case .asserted:
                RoundedRectangle(cornerRadius: 3).fill(color).frame(width: 18, height: 18)
            case .imprecise:
                Circle().strokeBorder(color.opacity(0.7), lineWidth: 2).frame(width: 40, height: 40)
            }
            if let initial = pin.initial, pin.treatment != .imprecise {
                Text(String(initial)).font(.system(size: 10, weight: .bold))
                    .foregroundStyle(pin.treatment == .live ? Color.white : color)
            }
        }
        .overlay(alignment: .bottom) {
            // The "colour + age chip" the design canvas calls for —
            // every pin shows an age, never just a bare dot.
            Text(pin.ageText)
                .font(.system(size: 8, design: .monospaced))
                .padding(.horizontal, 4).padding(.vertical, 1)
                .background(Color.ffBackground.opacity(0.85), in: Capsule())
                .foregroundStyle(color)
                .offset(y: 14)
        }
        .frame(minWidth: 44, minHeight: 44)
        .contentShape(Rectangle())
    }

    private func selectedCard(_ pin: CrewMapPin) -> some View {
        let (source, age) = MapViewModel.selectedCardText(for: pin)
        return VStack(alignment: .leading, spacing: 6) {
            HStack {
                Circle().fill(Color.mapCrew(colorIndex: pin.colorIndex)).frame(width: 10, height: 10)
                Text(pin.name).font(.headline).foregroundStyle(Color.ffInk)
                Spacer()
                Button { onDeselect() } label: {
                    Image(systemName: "xmark.circle.fill").foregroundStyle(Color.ffMuted)
                }
                .frame(minWidth: 44, minHeight: 44)
            }
            Text("\(source) · \(age)")
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(Color.ffMuted)
            if let distanceBearing = model.distanceBearingText(for: pin, imperial: false) {
                Text(distanceBearing)
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(Color.ffAmber)
            }
            HStack {
                Button("FIND") { onFind(pin.id) }
                    .buttonStyle(.borderedProminent).tint(.ffAmber)
                Button("MESSAGE") { onMessage(pin.id) }
                    .buttonStyle(.bordered)
            }
            .frame(minHeight: 44)
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.ffSurface)
        .accessibilityIdentifier("Map.SelectedCrewCard")
    }

    private func offlineFallback(_ text: String) -> some View {
        VStack(spacing: 12) {
            Image(systemName: "wifi.slash").font(.largeTitle).foregroundStyle(Color.ffMuted)
            Text(text)
                .font(.system(.body, design: .monospaced))
                .foregroundStyle(Color.ffMuted)
                .multilineTextAlignment(.center)
            Text("Use the FIELD map — it needs no network.")
                .font(.caption)
                .foregroundStyle(Color.ffMuted)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color.ffBackground)
    }

    private func chip(text: String, tint: Color) -> some View {
        Text(text)
            .font(.system(.caption2, design: .monospaced)).bold()
            .padding(.horizontal, 10).padding(.vertical, 5)
            .background(tint, in: Capsule())
            .foregroundStyle(Color.ffBackground)
    }

    private func centerIfNeeded() {
        guard case .automatic = cameraPosition else { return }
        let latitude: Double
        let longitude: Double
        if let mine = model.myCoordinate {
            latitude = mine.latitude
            longitude = mine.longitude
        } else if let venue = model.festpack?.meta.venue {
            latitude = venue.latitude
            longitude = venue.longitude
        } else {
            return // nothing honest to center on yet
        }
        let coordinate = CLLocationCoordinate2D(latitude: latitude, longitude: longitude)
        cameraPosition = .region(MKCoordinateRegion(center: coordinate,
                                                      span: MKCoordinateSpan(latitudeDelta: 0.01, longitudeDelta: 0.01)))
    }
}
