//
//  FireflyDebugHideBadgeLaunchTests.swift — the parsing half of
//  `-FireflyDebugHideBadge` (`FireflyDebugHideBadgeLaunch.swift`'s own
//  header). Pure argument parsing, no process launch, no `@testable
//  import Firefly` needed — same shape as
//  `FireflyDebugStartDestinationLaunchTests`.
//
import XCTest

final class FireflyDebugHideBadgeLaunchTests: XCTestCase {
    func testRequestedWhenTheFlagIsPresent() {
        XCTAssertTrue(FireflyDebugHideBadgeLaunch.isRequested(arguments: ["Firefly", "-FireflyDebugHideBadge"]))
    }

    func testNotRequestedWithoutTheFlag() {
        XCTAssertFalse(FireflyDebugHideBadgeLaunch.isRequested(arguments: ["Firefly"]))
    }

    func testNotRequestedOnAnEmptyArgumentList() {
        XCTAssertFalse(FireflyDebugHideBadgeLaunch.isRequested(arguments: []))
    }
}
