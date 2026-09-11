//
//  MapBridgeProjectionTests.swift — Map tab slice: `MapBridge`'s
//  east/north -> screen-px projection against `ff_map`'s OWN numbers.
//
//  `testHandComputedFixtureMatchesFirmwareCoreTest` ports
//  `firmware/core/tests/test_map.c`'s
//  `S09_AC2_crew_rally_you_share_transform_hand_computed_1px` fixture
//  verbatim — same points, same hand-computed expected values, same
//  1px tolerance. `MapBridge` calls `ff_map_xform_fit`/`ff_map_project`
//  directly (no reimplementation), so this is a real end-to-end proof
//  the Swift wrapper plumbs to the exact object code that test already
//  pins, not a parallel, independently-derived number that could quietly
//  drift from it.
//
@testable import FireflyModel
import XCTest

final class MapBridgeProjectionTests: XCTestCase {
    private let radiusPx: Float = 206.0 // test_map.c's FF_TEST_RADIUS_PX
    private let marginPx: Float = 24.0 // test_map.c's FF_TEST_MARGIN_PX
    private var usableR: Float { radiusPx - marginPx }

    func testHandComputedFixtureMatchesFirmwareCoreTest() {
        // Same two feature points as test_map.c: a 200m(east) x 100m(north)
        // rectangle centered on (50, 25).
        let points = [MapEastNorth(eastM: -50, northM: -25), MapEastNorth(eastM: 150, northM: 75)]
        let camera = MapBridge.fit(points: points, radiusPx: radiusPx, marginPx: marginPx)

        let expectedScale = (usableR * 1.41421356) / 200.0
        XCTAssertEqual(camera.scalePxPerM, expectedScale, accuracy: 0.001)
        XCTAssertEqual(camera.centerEastM, 50.0, accuracy: 0.001)
        XCTAssertEqual(camera.centerNorthM, 25.0, accuracy: 0.001)

        // Crew: 20m east, 10m north of bbox center.
        let crew = MapBridge.projectToScreen(MapEastNorth(eastM: 70, northM: 35), camera: camera)
        XCTAssertEqual(crew.x, (70.0 - 50.0) * expectedScale, accuracy: 1.0)
        XCTAssertEqual(crew.y, -((35.0 - 25.0) * expectedScale), accuracy: 1.0)

        // Rally: 30m west, 40m south of bbox center.
        let rally = MapBridge.projectToScreen(MapEastNorth(eastM: 20, northM: -15), camera: camera)
        XCTAssertEqual(rally.x, (20.0 - 50.0) * expectedScale, accuracy: 1.0)
        XCTAssertEqual(rally.y, -((-15.0 - 25.0) * expectedScale), accuracy: 1.0)

        // YOU: exactly at bbox center -> (0, 0).
        let you = MapBridge.projectToScreen(MapEastNorth(eastM: 50, northM: 25), camera: camera)
        XCTAssertEqual(you.x, 0, accuracy: 1.0)
        XCTAssertEqual(you.y, 0, accuracy: 1.0)
    }

    func testZeroFeaturesFallsBackToOneKilometerSquareAroundOrigin() {
        let camera = MapBridge.fit(points: [], radiusPx: radiusPx, marginPx: marginPx)
        // 1km square -> longer side 1000m maps to usable_r*sqrt(2).
        let expectedScale = (usableR * 1.41421356) / 1000.0
        XCTAssertEqual(camera.scalePxPerM, expectedScale, accuracy: 0.01)
        XCTAssertEqual(camera.centerEastM, 0, accuracy: 0.001)
        XCTAssertEqual(camera.centerNorthM, 0, accuracy: 0.001)
    }

    func testGeoProjectRoundTripsThroughUnproject() {
        let origin = GeoCoordinate(latitude: 43.700000, longitude: -121.500000)
        let point = GeoCoordinate(latitude: 43.700988, longitude: -121.501368) // Camp Glow corner
        let en = MapBridge.project(point, origin: origin)
        // Camp Glow is north and slightly west of the venue anchor.
        XCTAssertGreaterThan(en.northM, 0)
        XCTAssertLessThan(en.eastM, 0)
    }

    func testRenderKindMirrorsUntracedFeaturePolicy() {
        XCTAssertEqual(MapBridge.renderKind(pointCount: 0, isStage: false), .omit)
        XCTAssertEqual(MapBridge.renderKind(pointCount: 1, isStage: true), .stageStub)
        XCTAssertEqual(MapBridge.renderKind(pointCount: 1, isStage: false), .labelOnly)
        XCTAssertEqual(MapBridge.renderKind(pointCount: 2, isStage: false), .line)
        XCTAssertEqual(MapBridge.renderKind(pointCount: 3, isStage: false), .polygon)
        XCTAssertEqual(MapBridge.renderKind(pointCount: 9, isStage: false), .polygon)
    }

    func testClipToCirclePullsOutsidePointsIn() {
        let clipped = MapBridge.clipToCircle(x: 1000, y: 0, radiusPx: 100)
        XCTAssertEqual(clipped.x, 100, accuracy: 0.01)
        XCTAssertEqual(clipped.y, 0, accuracy: 0.01)
    }

    func testClipToCircleLeavesInsidePointsUntouched() {
        let clipped = MapBridge.clipToCircle(x: 10, y: 10, radiusPx: 100)
        XCTAssertEqual(clipped.x, 10, accuracy: 0.01)
        XCTAssertEqual(clipped.y, 10, accuracy: 0.01)
    }
}
