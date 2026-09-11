//
//  DemoMapFestpackSourceTests.swift — Map tab slice: the mock carries
//  Firefly Fields' real venue anchor and full feature/stage set.
//
@testable import FireflyModel
import XCTest

final class DemoMapFestpackSourceTests: XCTestCase {
    func testCurrentFestpackReturnsFireflyFields() async {
        let source = DemoMapFestpackSource()
        let pack = await source.currentFestpack()
        XCTAssertEqual(pack?.meta.name, "Firefly Fields")
    }

    func testVenueMatchesDemoWorldAnchorExactly() {
        // Byte-identical to DemoWorld.venueLatitude/venueLongitude —
        // this slice's own demo world must be the SAME festival as the
        // rest of the app's, not a second one that happens to share a
        // name (this file's own header comment).
        let venue = DemoMapFestpackSource.fireflyFields.meta.venue
        XCTAssertEqual(venue.latitude, 43.700000, accuracy: 0.000001)
        XCTAssertEqual(venue.longitude, -121.500000, accuracy: 0.000001)
    }

    func testFiveStagesMatchTheRealPack() {
        let stages = DemoMapFestpackSource.fireflyFields.stages
        XCTAssertEqual(Set(stages.map(\.id)), ["beacon", "hollow", "grove", "lantern", "glowworm"])
    }

    func testSixteenFeaturesMatchTheRealPack() {
        XCTAssertEqual(DemoMapFestpackSource.fireflyFields.features.count, 16)
    }

    func testEveryStageFeatureHasAStageID() {
        let stageFeatures = DemoMapFestpackSource.fireflyFields.features.filter { $0.kind == .stage }
        XCTAssertEqual(stageFeatures.count, 5)
        XCTAssertTrue(stageFeatures.allSatisfy { $0.stageID != nil })
    }
}
