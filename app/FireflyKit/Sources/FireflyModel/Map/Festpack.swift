//
//  Festpack.swift — Map tab slice: the narrow protocol this screen codes
//  against, and the plain-value shapes it needs from a festpack.
//
//  A PARALLEL slice is building the real festpack foundation
//  (`docs/specs/S05-festpack.md`, `firmware/festpack/`) — a
//  `FestpackProviding` yielding a richer `Festpack` value (stages,
//  features, schedule, meta) parsed from a real `.festpack.json`. This
//  file does NOT depend on that work landing first: `MapFestpackSource`
//  below is this slice's OWN, deliberately narrow seam (only the fields
//  the Map tab actually renders), so integration once the real slice
//  ships is a one-line swap — a new type conforming to
//  `MapFestpackSource` (or a thin adapter over the real
//  `FestpackProviding`), not a rewrite of anything below.
//
//  Never fabricates geometry: a feature's `polygon` is exactly what the
//  pack states — 0 points (nothing to draw), 1 (a point), 2 (a line), or
//  >=3 (a real polygon) — mirroring `ff_map_feature_render_kind`'s own
//  untraced-feature policy (`Bridge/MapBridge.swift`), never invented
//  past what the data says.
//
import Foundation

/// One festival-map feature kind — mirrors `fp_feature_kind_t` /
/// `ff_app_map_kind_t` (firmware) and `FF_THEME_MAP_*` (ff_theme.h)
/// 1:1, so a kind here maps straight onto the SAME palette entry the
/// puck's glass uses (`MapColors.swift`, app/Firefly/Sources/Map —
/// transcribed from ff_theme.h, same convention `FireflyTheme.swift`
/// already uses for the puck's other colors).
public enum FestpackFeatureKind: String, Sendable, Equatable, CaseIterable, Codable {
    case stage, camping, water, path, entrance, vendor, medical, poi, unknown
}

/// A bare WGS84 point, decoupled from `GeoCoordinate` (Bridge/) so this
/// file — the seam a future real-festpack adapter targets — carries no
/// bridge/C dependency of its own.
public struct FestpackLatLon: Sendable, Equatable, Codable {
    public let latitude: Double
    public let longitude: Double
    public init(latitude: Double, longitude: Double) {
        self.latitude = latitude
        self.longitude = longitude
    }
}

public struct FestpackStage: Sendable, Equatable, Identifiable, Codable {
    public let id: String
    public let name: String
    /// 0xRRGGBB, the pack's OWN stage color (S09 spec: "stage features
    /// use their own `fp_stage_t` color when valid") — never a
    /// theme-assigned fallback for a real stage.
    public let colorHex: UInt32
    /// The stage's own polygon, if the pack traced one — empty for an
    /// untraced stage (S09's stub-circle case).
    public let polygon: [FestpackLatLon]
    /// The point a stage renders/labels at when untraced (or the
    /// polygon's own centroid when traced) — the map view computes this
    /// once here rather than re-deriving a centroid at render time.
    public let centre: FestpackLatLon

    public init(id: String, name: String, colorHex: UInt32, polygon: [FestpackLatLon], centre: FestpackLatLon) {
        self.id = id
        self.name = name
        self.colorHex = colorHex
        self.polygon = polygon
        self.centre = centre
    }
}

public struct FestpackFeature: Sendable, Equatable, Identifiable, Codable {
    public let id: String
    public let kind: FestpackFeatureKind
    public let label: String
    /// Non-nil only for `kind == .stage` — the stage this feature IS,
    /// so a renderer can look up its own pack color rather than falling
    /// back to the kind palette (S09: stage features use their own
    /// color when valid).
    public let stageID: String?
    /// Exactly what the pack states — see this file's header comment
    /// on the 0/1/2/>=3-point render policy. Never padded or truncated
    /// here.
    public let polygon: [FestpackLatLon]

    public init(id: String, kind: FestpackFeatureKind, label: String, stageID: String? = nil,
                polygon: [FestpackLatLon]) {
        self.id = id
        self.kind = kind
        self.label = label
        self.stageID = stageID
        self.polygon = polygon
    }
}

public struct FestpackScheduleItem: Sendable, Equatable, Identifiable, Codable {
    public var id: String { "\(stageID)-\(day)-\(start)-\(artist)" }
    public let artist: String
    public let stageID: String
    public let day: String
    public let start: String
    public let end: String
    public let note: String?

    public init(artist: String, stageID: String, day: String, start: String, end: String, note: String? = nil) {
        self.artist = artist
        self.stageID = stageID
        self.day = day
        self.start = start
        self.end = end
        self.note = note
    }
}

public struct FestpackMeta: Sendable, Equatable, Codable {
    public let name: String
    public let venue: FestpackLatLon
    public init(name: String, venue: FestpackLatLon) {
        self.name = name
        self.venue = venue
    }
}

/// One resolved festpack — the whole shape the Map tab needs.
public struct Festpack: Sendable, Equatable, Codable {
    public let meta: FestpackMeta
    public let stages: [FestpackStage]
    public let features: [FestpackFeature]
    public let schedule: [FestpackScheduleItem]

    public init(meta: FestpackMeta, stages: [FestpackStage], features: [FestpackFeature],
                schedule: [FestpackScheduleItem]) {
        self.meta = meta
        self.stages = stages
        self.features = features
        self.schedule = schedule
    }
}

/// The Map tab's OWN narrow seam onto "whatever festpack is currently
/// loaded" — see this file's header comment for why it exists
/// independently of the parallel `FestpackProviding` slice. A single
/// async accessor rather than a stream: the Map tab re-reads this on
/// appear / on pull-to-refresh, it does not need push updates for v1
/// (matching S09's own "fixed-fit v1" framing for the geometry itself).
public protocol MapFestpackSource: Sendable {
    func currentFestpack() async -> Festpack?
}

// PR #283 review, SHOULD-FIX 7: as of this fix, the parallel
// `FestpackProviding` slice (S05) has NOT landed on `main` yet — this
// file's header comment's "one-line swap" framing is still unverified,
// not confirmed. The one concrete risk this review flagged, documented
// here rather than assumed away: `Festpack.meta.venue`/`meta.name` are
// REQUIRED by every caller of this seam (`FieldMapProjector.project`'s
// own `ff_geo_project` origin, `GPSMapView`'s offline-fallback center).
// If the real `FestpackProviding` slice's own value only carries
// "stages id/name/colour/polygon/centre + features" (per that slice's
// own brief) and NOT an explicit venue anchor, a `MapFestpackSource`
// adapter over it needs MORE than a one-line field remap — e.g.
// deriving a venue from the stage-centre bounding box's own centroid,
// which is itself an interpretation call (which stages count, how ties
// break) that should be confirmed with whoever owns that slice, not
// invented silently the day this adapter is written. If that slice DOES
// carry its own venue anchor by the time it lands, the swap really is
// the one-line `MapFestpackSource` conformance this file's header
// already describes — this note is only for the other case.

