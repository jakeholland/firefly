//
//  MapFestpack.swift — Map tab slice: the narrow protocol this screen
//  codes against, and the plain-value shapes it needs from a festpack.
//  Named `MapFestpack*` throughout, and this file itself renamed from
//  `Festpack.swift` (not just the types), because the parallel
//  festpack-foundation slice (S05, PR #285) landed its OWN `Festpack`/
//  `FestpackStage`/`FestpackFeature`/`FestpackFeatureKind`/`FestpackMeta`
//  in a file ALSO named `Festpack.swift`, in this SAME `FireflyModel`
//  target (`Festpack/Festpack.swift`) — two source files sharing a
//  basename in one SwiftPM target isn't just a naming clash, it fails
//  the build outright ("multiple producers" for the object file both
//  would compile to), so both the types below and this file's own name
//  had to move, not merely a style choice.
//
//  The real festpack foundation has now landed
//  (`docs/specs/S05-festpack.md`, `firmware/festpack/`) — a
//  `FestpackProviding` yielding a richer `Festpack` value (stages,
//  features, schedule, meta) parsed from a real `.festpack.json`.
//  `FestpackProvidingMapAdapter.swift` (same directory) is the thin
//  adapter over it this file's own `MapFestpackSource` seam was always
//  meant to receive — see that file's header comment for what closing
//  that gap actually took (it was more than a one-line remap; see this
//  file's trailing note for the history).
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
public enum MapFestpackFeatureKind: String, Sendable, Equatable, CaseIterable, Codable {
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

public struct MapFestpackStage: Sendable, Equatable, Identifiable, Codable {
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
    /// `nil` for a stage the pack hasn't placed at all yet — a real
    /// festival can ship its stage lineup before its map is finished
    /// (`FestpackProvidingMapAdapter`'s own doc comment: Lost Lands 2026
    /// itself ships three such stages), and this type never fabricates
    /// a centre to fill that gap.
    public let centre: FestpackLatLon?

    public init(id: String, name: String, colorHex: UInt32, polygon: [FestpackLatLon], centre: FestpackLatLon?) {
        self.id = id
        self.name = name
        self.colorHex = colorHex
        self.polygon = polygon
        self.centre = centre
    }
}

public struct MapFestpackFeature: Sendable, Equatable, Identifiable, Codable {
    public let id: String
    public let kind: MapFestpackFeatureKind
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

    public init(id: String, kind: MapFestpackFeatureKind, label: String, stageID: String? = nil,
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

public struct MapFestpackMeta: Sendable, Equatable, Codable {
    public let name: String
    public let venue: FestpackLatLon
    public init(name: String, venue: FestpackLatLon) {
        self.name = name
        self.venue = venue
    }
}

/// One resolved festpack — the whole shape the Map tab needs.
public struct MapFestpack: Sendable, Equatable, Codable {
    public let meta: MapFestpackMeta
    public let stages: [MapFestpackStage]
    public let features: [MapFestpackFeature]
    public let schedule: [FestpackScheduleItem]

    public init(meta: MapFestpackMeta, stages: [MapFestpackStage], features: [MapFestpackFeature],
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
    func currentFestpack() async -> MapFestpack?
}

// PR #283 review, SHOULD-FIX 7, resolved: the risk this note originally
// flagged (before PR #285 landed) turned out to be real, not
// hypothetical — the swap needed more than a one-line remap. Recorded
// here rather than deleted, since the reasoning is what a future reader
// would otherwise have to re-derive:
//
//   - The real `Festpack` (`Festpack/Festpack.swift`) carries NO
//     explicit venue anchor in Swift — only `originKnown`/
//     `originApproximate` booleans. The lat/lon itself
//     (`fp_pack_t.origin`) was computed by `fp_parse` and used to
//     project every feature/stage point into local meters, then
//     dropped on the floor rather than surfaced. This was the exact
//     fork in the road this note called out: derive a venue from a
//     stage-centre bounding box (an invented interpretation — which
//     stages count, how ties break) or surface the pack's own real
//     origin. `FestpackProvidingMapAdapter` took the second path —
//     `Festpack`/`FestpackParser` (PR #285's own files) now also expose
//     `originLatitude`/`originLongitude`, nothing more.
//   - Stage geometry lives on `.stage`-kind `FestpackFeature`s
//     (`stageID`), not on `FestpackStage` itself, and in local ENU
//     meters rather than WGS84 — recovered via `MapBridge.unproject`,
//     the exact inverse of the projection `fp_parse` used, never a
//     second hand-rolled unprojection.
//   - Some real stages have NO known geometry at all yet (Lost Lands
//     2026 itself: "raptor-alley"/"grove" have no map feature,
//     "forest" has an explicit null polygon) — which is why
//     `MapFestpackStage.centre` above is `nil`-able, a change this note
//     originally didn't anticipate needing.
//
// See `FestpackProvidingMapAdapter.swift` for the adapter itself and
// `FestpackProvidingMapAdapterTests.swift` for the real-pack proof.

