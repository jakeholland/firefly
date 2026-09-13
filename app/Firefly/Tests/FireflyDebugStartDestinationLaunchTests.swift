//
//  FireflyDebugStartDestinationLaunchTests.swift — the parsing half of
//  "app: Map subscribes to festpack updates" (2026-09-13)'s debug-only
//  `-FireflyStartTab`/`-FireflyFindSegment` seam. Pure argument parsing,
//  no process launch needed. Deliberately plain `XCTest`, no `@testable
//  import Firefly` — `FireflyDebugStartDestinationLaunch.swift`'s own
//  header comment on why it hands back a bare `String?` rather than
//  `Destination`/`FindSegment` explains why this file needs neither.
//
import XCTest

final class FireflyDebugStartDestinationLaunchTests: XCTestCase {
    func testRequestedTabNameParsesTheValueAfterTheFlag() {
        let name = FireflyDebugStartDestinationLaunch.requestedTabName(arguments: ["Firefly", "-FireflyStartTab", "find"])
        XCTAssertEqual(name, "find")
    }

    func testRequestedTabNameIsNilWithoutTheFlag() {
        XCTAssertNil(FireflyDebugStartDestinationLaunch.requestedTabName(arguments: ["Firefly"]))
    }

    func testRequestedTabNameIsNilWhenTheFlagIsTheLastArgument() {
        XCTAssertNil(FireflyDebugStartDestinationLaunch.requestedTabName(arguments: ["Firefly", "-FireflyStartTab"]))
    }

    func testRequestedFindSegmentNameParsesTheValueAfterTheFlag() {
        let name = FireflyDebugStartDestinationLaunch.requestedFindSegmentName(
            arguments: ["Firefly", "-FireflyFindSegment", "field"])
        XCTAssertEqual(name, "field")
    }

    func testRequestedFindSegmentNameIsNilWithoutTheFlag() {
        XCTAssertNil(FireflyDebugStartDestinationLaunch.requestedFindSegmentName(arguments: ["Firefly"]))
    }

    func testBothFlagsParseIndependently() {
        let arguments = ["Firefly", "-FireflyStartTab", "find", "-FireflyFindSegment", "field"]
        XCTAssertEqual(FireflyDebugStartDestinationLaunch.requestedTabName(arguments: arguments), "find")
        XCTAssertEqual(FireflyDebugStartDestinationLaunch.requestedFindSegmentName(arguments: arguments), "field")
    }
}
