//
//  FieldMapProjectionTests.swift — Map tab slice: projecting the real
//  Firefly Fields demo geometry onto the schematic Field map.
//
@testable import FireflyModel
import XCTest

final class FieldMapProjectionTests: XCTestCase {
    private var festpack: Festpack { DemoMapFestpackSource.fireflyFields }

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
        let path = FestpackFeature(id: "test-path", kind: .path, label: "Test Path", polygon: [
            FestpackLatLon(latitude: 43.7000, longitude: -121.5000),
            FestpackLatLon(latitude: 43.7002, longitude: -121.4998),
        ])
        let pack = Festpack(meta: festpack.meta, stages: festpack.stages, features: [path], schedule: [])
        let projection = FieldMapProjector.project(festpack: pack, crewPins: [], myPosition: nil, headingDegrees: nil)
        XCTAssertEqual(projection.features.first?.renderKind, .line)
        XCTAssertEqual(projection.features.first?.points.count, 2)
    }

    func testCrewPinsProjectAndYouIsNilWithoutAFix() {
        let store = CrewStore(now: { 0 })
        store.onPosition(nodeID: 1, latitude: 43.700902, longitude: -121.498753, rxTimeMs: 0)
        let pins = CrewMapPinBuilder.build(from: store.members(now: 1_000),
                                            myPosition: GeoCoordinate(latitude: 43.7, longitude: -121.5),
                                            imperial: false)
        let projection = FieldMapProjector.project(festpack: festpack, crewPins: pins, myPosition: nil,
                                                     headingDegrees: nil)
        XCTAssertEqual(projection.crew.count, 1)
        XCTAssertNil(projection.you, "no fix -> YOU must be hidden, never a fabricated position (S09 AC5)")
    }

    func testYouAppearsOnceAFixExists() {
        let projection = FieldMapProjector.project(festpack: festpack, crewPins: [],
                                                     myPosition: GeoCoordinate(latitude: 43.7, longitude: -121.5),
                                                     headingDegrees: 90)
        XCTAssertNotNil(projection.you)
        XCTAssertEqual(projection.you?.headingDegrees, 90)
    }
}
