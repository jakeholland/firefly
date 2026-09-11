//
//  FieldMapProjection.swift — Map tab slice: projects a `MapFestpack` +
//  crew pins + "you" onto the SCHEMATIC Field map's screen space, using
//  the puck's own `ff_map`/`ff_geo` object code end to end
//  (`Bridge/MapBridge.swift`, `Bridge/GeoBridge.swift`) — never a
//  from-scratch reimplementation of the puck's projection math. This is
//  what `docs/specs/S09-map-face.md` calls the "fixed-fit v1" camera:
//  bounding box of feature anchor points -> a circle, north-up, single
//  scale+offset shared by every drawn thing.
//
//  Origin for `ff_geo_project`: the festpack's own venue coordinate
//  (`MapFestpack.meta.venue`) — one fixed anchor every point in a build
//  (features, crew, you) projects against, so nothing can disagree
//  about where "here" is (same rationale `ff_map_xform_t`'s own doc
//  comment gives for a single shared scale+offset).
//
//  Schematic, honestly labeled: this is NOT a literal map — see
//  `FieldMapView.swift` (app/Firefly/Sources/Map) for the "SCHEMATIC"
//  label this projection's output is always shown under.
//
import Foundation

/// One drawable feature, already projected to screen px (center-relative
/// — circle center = (0, 0), same convention `ff_map_xform_t` uses).
public struct FieldMapFeature: Sendable, Equatable, Identifiable {
    public let id: String
    public let kind: MapFestpackFeatureKind
    public let label: String
    public let colorHex: UInt32
    public let renderKind: MapFeatureRenderKind
    /// Screen points, already circle-clipped (`ff_map_clip_point_to_circle`)
    /// — 0 for `.omit`, 1 for `.labelOnly`/`.stageStub`, 2 for `.line`,
    /// >=3 for `.polygon`. Matches the feature's own `polygon.count`
    /// 1:1 — nothing padded, nothing dropped.
    public let points: [(x: Float, y: Float)]

    public static func == (lhs: FieldMapFeature, rhs: FieldMapFeature) -> Bool {
        lhs.id == rhs.id && lhs.kind == rhs.kind && lhs.label == rhs.label && lhs.colorHex == rhs.colorHex
            && lhs.renderKind == rhs.renderKind
            && lhs.points.count == rhs.points.count
            && zip(lhs.points, rhs.points).allSatisfy { $0.x == $1.x && $0.y == $1.y }
    }
}

public struct FieldMapCrewDot: Sendable, Equatable {
    public let pin: CrewMapPin
    public let x: Float
    public let y: Float
}

public struct FieldMapYou: Sendable, Equatable {
    public let x: Float
    public let y: Float
    public let headingDegrees: Double?
}

/// The whole schematic-map render input for one build — everything
/// `FieldMapView` needs, already in screen px.
public struct FieldMapProjection: Sendable, Equatable {
    public let radiusPx: Float
    public let features: [FieldMapFeature]
    public let crew: [FieldMapCrewDot]
    public let you: FieldMapYou?

    public static func == (lhs: FieldMapProjection, rhs: FieldMapProjection) -> Bool {
        lhs.radiusPx == rhs.radiusPx && lhs.features == rhs.features && lhs.crew == rhs.crew && lhs.you == rhs.you
    }
}

public enum FieldMapProjector {
    /// Default layout constants — deliberately mirroring
    /// `FF_MAP_CIRCLE_RADIUS_PX`/`FF_MAP_MARGIN_PX` (`scr_map.c`) in
    /// SPIRIT (a fitted circle with an inset margin), not in literal
    /// pixel value: the puck's numbers are sized for its 412px round
    /// glass, and this is a phone screen. `FieldMapView` passes its own
    /// on-screen radius; these are only the fallback for a caller (a
    /// test) that doesn't care.
    public static let defaultRadiusPx: Float = 160
    public static let defaultMarginPx: Float = 16

    /// Projects one build. `myPosition`/`headingDegrees` are both
    /// optional — a caller with no fix yet gets `you == nil` (S09 AC5:
    /// "hidden... when no fix"), never a fabricated position.
    public static func project(festpack: MapFestpack, crewPins: [CrewMapPin], myPosition: GeoCoordinate?,
                                headingDegrees: Double?, radiusPx: Float = defaultRadiusPx,
                                marginPx: Float = defaultMarginPx) -> FieldMapProjection {
        let origin = GeoCoordinate(latitude: festpack.meta.venue.latitude, longitude: festpack.meta.venue.longitude)

        // Anchor points feed the camera fit — S09's PR #73 amendment:
        // ONE representative point per feature (its single point, or its
        // vertex centroid), never every vertex of every polygon, so one
        // large boundary shape can't crush the fit for everything else.
        // Crew and "you" also contribute their own anchor, so the fitted
        // circle always includes every drawable thing, not just the
        // festpack's own features.
        var anchors: [MapEastNorth] = festpack.features.compactMap { feature in
            anchorEastNorth(for: feature.polygon, origin: origin)
        }
        anchors += crewPins.map { MapBridge.project(GeoCoordinate(latitude: $0.latitude, longitude: $0.longitude),
                                                      origin: origin) }
        if let myPosition { anchors.append(MapBridge.project(myPosition, origin: origin)) }

        let camera = MapBridge.fit(points: anchors, radiusPx: radiusPx, marginPx: marginPx)

        let features: [FieldMapFeature] = festpack.features.map { feature in
            let renderKind = MapBridge.renderKind(pointCount: feature.polygon.count, isStage: feature.kind == .stage)
            let screenPoints = feature.polygon.map { latlon -> (x: Float, y: Float) in
                let en = MapBridge.project(GeoCoordinate(latitude: latlon.latitude, longitude: latlon.longitude),
                                            origin: origin)
                let raw = MapBridge.projectToScreen(en, camera: camera)
                return MapBridge.clipToCircle(x: raw.x, y: raw.y, radiusPx: radiusPx)
            }
            let colorHex = stageColor(for: feature, in: festpack) ?? FieldMapProjector.kindColorHex(feature.kind)
            return FieldMapFeature(id: feature.id, kind: feature.kind, label: feature.label, colorHex: colorHex,
                                    renderKind: renderKind, points: screenPoints)
        }

        let crew: [FieldMapCrewDot] = crewPins.map { pin in
            let en = MapBridge.project(GeoCoordinate(latitude: pin.latitude, longitude: pin.longitude), origin: origin)
            let raw = MapBridge.projectToScreen(en, camera: camera)
            let clipped = MapBridge.clipToCircle(x: raw.x, y: raw.y, radiusPx: radiusPx)
            return FieldMapCrewDot(pin: pin, x: clipped.x, y: clipped.y)
        }

        let you: FieldMapYou? = myPosition.map { position in
            let en = MapBridge.project(position, origin: origin)
            let raw = MapBridge.projectToScreen(en, camera: camera)
            let clipped = MapBridge.clipToCircle(x: raw.x, y: raw.y, radiusPx: radiusPx)
            return FieldMapYou(x: clipped.x, y: clipped.y, headingDegrees: headingDegrees)
        }

        return FieldMapProjection(radiusPx: radiusPx, features: features, crew: crew, you: you)
    }

    /// The single east/north anchor a feature contributes to the camera
    /// fit — mirrors `map_feature_anchor_en` (`scr_map.c`): the one
    /// point for a 1-point feature, the vertex centroid for 2-or-more.
    /// `nil` for a 0-point feature (nothing to anchor).
    static func anchorEastNorth(for polygon: [FestpackLatLon], origin: GeoCoordinate) -> MapEastNorth? {
        guard !polygon.isEmpty else { return nil }
        if polygon.count == 1 {
            return MapBridge.project(GeoCoordinate(latitude: polygon[0].latitude, longitude: polygon[0].longitude),
                                      origin: origin)
        }
        let points = polygon.map { MapBridge.project(GeoCoordinate(latitude: $0.latitude, longitude: $0.longitude),
                                                       origin: origin) }
        let sumE = points.reduce(Float(0)) { $0 + $1.eastM }
        let sumN = points.reduce(Float(0)) { $0 + $1.northM }
        return MapEastNorth(eastM: sumE / Float(points.count), northM: sumN / Float(points.count))
    }

    private static func stageColor(for feature: MapFestpackFeature, in festpack: MapFestpack) -> UInt32? {
        guard feature.kind == .stage, let stageID = feature.stageID else { return nil }
        return festpack.stages.first { $0.id == stageID }?.colorHex
    }

    /// The fallback (non-stage) kind palette — see `MapColors.swift`
    /// (app/Firefly/Sources/Map) for the SwiftUI `Color` wrapper around
    /// these same hexes, transcribed from `ff_theme.h`'s `FF_THEME_MAP_*`
    /// block.
    public static func kindColorHex(_ kind: MapFestpackFeatureKind) -> UInt32 {
        switch kind {
        case .stage: return 0x8B8A97 // FF_THEME_COLOR_MUTED — only used when a stage has no pack color
        case .camping: return 0xC49A6C
        case .water: return 0x4FD8C4
        case .path: return 0x8B8A97
        case .entrance: return 0x9BE07B
        case .vendor: return 0xFFC66B
        case .medical: return 0xFF6B6B
        case .poi: return 0x6B8CAE
        case .unknown: return 0x8B8A97
        }
    }
}
