//
//  FestpackProvidingMapAdapter.swift — Map tab slice: a thin
//  `MapFestpackSource` over the real `FestpackProviding` (S05 festpack
//  foundation, `Festpack/FestpackProviding.swift`) — the "one-line swap"
//  `MapFestpack.swift`'s own header comment anticipated, now that slice has
//  landed on `main` (PR #285).
//
//  Two gaps PR #283's own review flagged as a real possibility (see
//  `MapFestpack.swift`'s trailing note) turned out to be real, not
//  hypothetical, once the real slice landed — both closed honestly
//  here, never papered over:
//
//    - The real `Festpack` carries NO venue anchor of its own in Swift
//      — only `originKnown`/`originApproximate` booleans. The lat/lon
//      itself (`fp_pack_t.origin`) was computed by `fp_parse` for every
//      projected `eastMeters`/`northMeters` in the pack, then simply
//      never read back out into Swift. Rather than derive a synthetic
//      venue from a stage-centre bounding box — exactly the fabrication
//      the review warned against — `Festpack`/`FestpackParser` (PR
//      #285's own files, extended here) now surface that same real
//      `originLatitude`/`originLongitude`, guarded by the SAME
//      `originKnown` the C struct already carries.
//    - Stage geometry lives on `.stage`-kind `FestpackFeature`s
//      (`stageID`), not on `FestpackStage` itself, and in local ENU
//      meters, not WGS84. `MapBridge.unproject` — the exact inverse of
//      the projection `fp_parse` used to produce those meters — recovers
//      real coordinates; nothing here reimplements or approximates that
//      math.
//
//  `currentFestpack()` returns `nil` whenever the real pack's own origin
//  is unknown: with no honest anchor, nothing in it can be placed, so
//  handing back a `MapFestpack` full of unusable geometry would be worse
//  than handing back nothing — the same "never a fabricated position"
//  posture `CrewMapPinBuilder`/`MapViewModel` already hold elsewhere in
//  this slice. `MapFestpackStage.centre` is `nil` for a stage this pack
//  hasn't placed on the map at all — real festivals ship a stage lineup
//  before the map is finished: Lost Lands 2026's own vendored fixture
//  ships "raptor-alley"/"grove" with no map feature at all and "forest"
//  with an explicit null polygon ("unplaced"). Never a fabricated centre
//  to paper over any of the three.
//
//  Kept alongside `DemoMapFestpackSource`, never replacing it —
//  `AppGraph.makeMapViewModel()` still hands demo builds the demo source
//  (Firefly Fields must stay independent of network/real-pack
//  availability); every other build gets this adapter, wrapping the SAME
//  `AppGraph.festpack` instance `makeLineupViewModel()` already reads —
//  one provider, one composition root, never a second independent fetch.
//
import Foundation

public final class FestpackProvidingMapAdapter: MapFestpackSource {
    private let provider: any FestpackProviding

    public init(provider: any FestpackProviding) {
        self.provider = provider
    }

    public func currentFestpack() async -> MapFestpack? {
        guard let real = await provider.current() else { return nil }
        return Self.map(real)
    }

    /// A pure value transform — independently testable without a real
    /// `FestpackProviding` (`FestpackProvidingMapAdapterTests`, off a
    /// real pack parsed straight from `firmware/assets/field/
    /// lost-lands-2026.festpack.json` — the real bundled fallback pack
    /// `AlmanacFestpackProvider` itself falls back to — not a hand-built
    /// fixture). `nil` iff `real.originKnown` is false — see this file's
    /// header comment.
    static func map(_ real: Festpack) -> MapFestpack? {
        guard real.originKnown else { return nil }
        let origin = GeoCoordinate(latitude: real.originLatitude, longitude: real.originLongitude)

        let features: [MapFestpackFeature] = real.features.map { feature in
            MapFestpackFeature(id: String(feature.id), kind: mapKind(feature.kind), label: feature.label,
                                stageID: feature.stageID, polygon: unproject(feature.points, origin: origin))
        }

        let stages: [MapFestpackStage] = real.stages.map { stage in
            // Stage geometry lives on the matching `.stage`-kind
            // feature (this file's header comment), never on
            // `FestpackStage` itself — `nil`/empty when the pack has no
            // such feature for this stage at all.
            let stageFeature = real.features.first { $0.kind == .stage && $0.stageID == stage.id }
            let points = stageFeature.map { unproject($0.points, origin: origin) } ?? []
            return MapFestpackStage(id: stage.id, name: stage.name, colorHex: stage.colorRGB,
                                     // A stage `polygon` means "the pack traced a real outline"
                                     // (`MapFestpack.swift`'s own doc comment) — only >=3 points
                                     // qualify; 1-2 stays an untraced stub, matching every real
                                     // stage in Lost Lands 2026 today (single-point or absent).
                                     polygon: points.count >= 3 ? points : [],
                                     centre: centre(of: points))
        }

        return MapFestpack(
            meta: MapFestpackMeta(name: real.name, venue: FestpackLatLon(latitude: origin.latitude,
                                                                          longitude: origin.longitude)),
            stages: stages, features: features,
            // The real pack's own schedule (`real.sets`) is minute-
            // offset/day-of-year shaped, not the day/start/end STRING
            // shape `FestpackScheduleItem` carries, and nothing reads
            // `MapFestpack.schedule` today (that type's own doc comment:
            // carried through only because the shape names it). Guessing
            // at a string translation here would be new, unverified code
            // with no consumer to prove it against — left honestly
            // empty instead.
            schedule: [])
    }

    private static func unproject(_ points: [FestpackPoint], origin: GeoCoordinate) -> [FestpackLatLon] {
        points.map { point in
            let en = MapEastNorth(eastM: Float(point.eastMeters), northM: Float(point.northMeters))
            let geo = MapBridge.unproject(en, origin: origin)
            return FestpackLatLon(latitude: geo.latitude, longitude: geo.longitude)
        }
    }

    /// A stage's own label point: the polygon's centroid when traced
    /// (>=3 points), the midpoint of a 2-point line, the single known
    /// point for 1, and `nil` when this pack hasn't placed it at all —
    /// mirrors `MapFestpackStage.centre`'s own doc comment exactly, and
    /// never invents a point past what `points` actually holds.
    private static func centre(of points: [FestpackLatLon]) -> FestpackLatLon? {
        guard !points.isEmpty else { return nil }
        let lat = points.reduce(0) { $0 + $1.latitude } / Double(points.count)
        let lon = points.reduce(0) { $0 + $1.longitude } / Double(points.count)
        return FestpackLatLon(latitude: lat, longitude: lon)
    }

    /// Case-for-case: the real `FestpackFeatureKind` (`Festpack/
    /// Festpack.swift`) and this slice's own `MapFestpackFeatureKind`
    /// (`MapFestpack.swift`) carry the identical case set — this is a
    /// decode across the PR #283/#285 naming split, never a
    /// reinterpretation of what a kind means.
    private static func mapKind(_ kind: FestpackFeatureKind) -> MapFestpackFeatureKind {
        switch kind {
        case .stage: return .stage
        case .camping: return .camping
        case .water: return .water
        case .path: return .path
        case .entrance: return .entrance
        case .vendor: return .vendor
        case .medical: return .medical
        case .poi: return .poi
        case .unknown: return .unknown
        }
    }
}
