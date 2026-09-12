//
//  FieldMapProjectionTests.swift — Map tab slice: projecting the real
//  Firefly Fields demo geometry onto the schematic Field map.
//
@testable import FireflyModel
import XCTest

final class FieldMapProjectionTests: XCTestCase {
    private var festpack: MapFestpack { DemoMapFestpackSource.fireflyFields }

    func testEveryFeatureProjectsWithoutCrashingAndStaysInsideTheCircle() {
        let projection = FieldMapProjector.project(festpack: festpack, crewPins: [], myPosition: nil,
                                                     headingDegrees: nil, radiusPx: 160, marginPx: 16)
        XCTAssertEqual(projection.features.count, festpack.features.count)
        for feature in projection.features {
            for point in feature.points {
                let dist = (point.x * point.x + point.y * point.y).squareRoot()
                XCTAssertLessThanOrEqual(dist, 160.01, "\(feature.label) point escaped the fitted circle")
            }
        }
    }

    func testStagePointFeatureRendersAsStageStub() {
        let projection = FieldMapProjector.project(festpack: festpack, crewPins: [], myPosition: nil,
                                                     headingDegrees: nil)
        let beacon = projection.features.first { $0.id == "stage-beacon" }
        XCTAssertEqual(beacon?.renderKind, .stageStub)
        XCTAssertEqual(beacon?.points.count, 1)
        XCTAssertEqual(beacon?.colorHex, 0xFFC66B, "a stage feature must use the pack's OWN stage color")
    }

    func testNonStageSinglePointFeatureRendersAsLabelOnly() {
        let projection = FieldMapProjector.project(festpack: festpack, crewPins: [], myPosition: nil,
                                                     headingDegrees: nil)
        let tower = projection.features.first { $0.id == "firefly-tower" }
        XCTAssertEqual(tower?.renderKind, .labelOnly)
        XCTAssertEqual(tower?.points.count, 1)
    }

    func testFourPointCampingFeatureRendersAsPolygon() {
        let projection = FieldMapProjector.project(festpack: festpack, crewPins: [], myPosition: nil,
                                                     headingDegrees: nil)
        let campGlow = projection.features.first { $0.id == "camp-glow" }
        XCTAssertEqual(campGlow?.renderKind, .polygon)
        XCTAssertEqual(campGlow?.points.count, 4)
        XCTAssertEqual(campGlow?.colorHex, FieldMapProjector.kindColorHex(.camping))
    }

    func testTwoPointFeatureRendersAsLine() {
        // Synthetic: the real Firefly Fields pack has no 2-point feature,
        // so this proves the LINE branch of the same untraced-feature
        // policy independently of what the demo data happens to contain.
        let path = MapFestpackFeature(id: "test-path", kind: .path, label: "Test Path", polygon: [
            FestpackLatLon(latitude: 43.7000, longitude: -121.5000),
            FestpackLatLon(latitude: 43.7002, longitude: -121.4998),
        ])
        let pack = MapFestpack(meta: festpack.meta, stages: festpack.stages, features: [path], schedule: [])
        let projection = FieldMapProjector.project(festpack: pack, crewPins: [], myPosition: nil, headingDegrees: nil)
        XCTAssertEqual(projection.features.first?.renderKind, .line)
        XCTAssertEqual(projection.features.first?.points.count, 2)
    }

    func testCrewPinsProjectAndYouIsNilWithoutAFix() {
        let store = CrewStore(now: { 0 })
        store.onPosition(nodeID: 1, latitude: 43.700902, longitude: -121.498753, rxTimeMs: 0)
        let pins = CrewMapPinBuilder.build(from: store.members(now: 1_000),
                                            myPosition: GeoCoordinate(latitude: 43.7, longitude: -121.5))
        let projection = FieldMapProjector.project(festpack: festpack, crewPins: pins, myPosition: nil,
                                                     headingDegrees: nil)
        XCTAssertEqual(projection.crew.count, 1)
        XCTAssertNil(projection.you, "no fix -> YOU must be hidden, never a fabricated position (S09 AC5)")
    }

    // PR #283 review, BLOCKING 1: `MapTabView` used to hardcode
    // `radiusPx: 160` regardless of the Field map's ACTUAL on-screen
    // circle (`FieldMapView`'s own `GeometryReader`-measured `side/2`),
    // breaking S09 AC1's "every point stays inside the fitted circle"
    // guarantee whenever the real square wasn't ~320pt. The fix feeds
    // the view's own measured radius into `fieldMapProjection(radiusPx:
    // marginPx:)` instead — this test proves the PROJECTION ITSELF
    // (not just the default) correctly fits a boundary point onto the
    // circle for two genuinely different radii, so plumbing an
    // arbitrary measured value through can never silently regress back
    // to "some points land outside".
    func testBoundaryPointLandsExactlyOnTheFittedCircleForTwoDifferentRadii() {
        // Two festival-boundary features sharing a latitude — the
        // bounding box `ff_map_xform_fit` fits (`ff_map.c`) then has
        // ZERO north/south extent, so the fit is governed purely by the
        // east/west span between them, and EACH point sits at exactly
        // one extreme corner of that box. `ff_map_xform_fit`'s own
        // `FF_MAP_SQRT2` factor (its doc comment: fitting the bbox's
        // longer side to the side of the square INSCRIBED in the usable
        // circle) means an extreme-corner point on a zero-height box
        // lands at exactly `(radiusPx - marginPx) / sqrt(2)` from the
        // circle's center — a RATIO, so this doesn't need to hand-derive
        // `ff_geo_project`'s own meters-per-degree conversion to pin an
        // exact expected value.
        let venue = FestpackLatLon(latitude: 43.7000, longitude: -121.5000)
        let west = MapFestpackFeature(id: "west-edge", kind: .poi, label: "West Edge",
                                    polygon: [FestpackLatLon(latitude: 43.7000, longitude: -121.5050)])
        let east = MapFestpackFeature(id: "east-edge", kind: .poi, label: "East Edge",
                                    polygon: [FestpackLatLon(latitude: 43.7000, longitude: -121.4950)])
        let pack = MapFestpack(meta: MapFestpackMeta(name: "Boundary Test", venue: venue), stages: [],
                             features: [west, east], schedule: [])
        let marginPx: Float = 16

        for radiusPx: Float in [90, 220] {
            let projection = FieldMapProjector.project(festpack: pack, crewPins: [], myPosition: nil,
                                                         headingDegrees: nil, radiusPx: radiusPx, marginPx: marginPx)
            let expectedDistance = (radiusPx - marginPx) / Float(2).squareRoot()
            XCTAssertEqual(projection.features.count, 2)
            for feature in projection.features {
                guard let point = feature.points.first else {
                    return XCTFail("expected \(feature.label)'s own projected point at radiusPx \(radiusPx)")
                }
                let distanceFromCenter = (point.x * point.x + point.y * point.y).squareRoot()
                XCTAssertEqual(distanceFromCenter, expectedDistance, accuracy: 0.5,
                                "\(feature.label) must land exactly on the fitted circle for radiusPx \(radiusPx) — "
                                    + "proves the projection actually honors whatever radius it's given, not a "
                                    + "hardcoded one")
            }
        }
    }

    func testYouAppearsOnceAFixExists() {
        let projection = FieldMapProjector.project(festpack: festpack, crewPins: [],
                                                     myPosition: GeoCoordinate(latitude: 43.7, longitude: -121.5),
                                                     headingDegrees: 90)
        XCTAssertNotNil(projection.you)
        XCTAssertEqual(projection.you?.headingDegrees, 90)
    }
}
