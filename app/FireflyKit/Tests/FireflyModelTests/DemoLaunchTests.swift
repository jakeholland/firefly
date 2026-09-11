//
//  DemoLaunchTests.swift — pure parsing, no process state touched:
//  every case is driven through the injectable `arguments`/
//  `environment` parameters so this suite never depends on (or
//  mutates) the real `CommandLine`/`ProcessInfo` the test runner
//  itself was launched with.
//
import FireflyModel
import XCTest

final class DemoLaunchTests: XCTestCase {

    func testFlagRequestsDemoMode() {
        XCTAssertTrue(DemoLaunch.isRequested(arguments: ["/path/to/app", "-FireflyDemo"], environment: [:]))
    }

    func testEnvVarRequestsDemoMode() {
        XCTAssertTrue(DemoLaunch.isRequested(arguments: ["/path/to/app"], environment: ["FIREFLY_DEMO": "1"]))
    }

    func testEnvVarMustBeExactlyOne() {
        XCTAssertFalse(DemoLaunch.isRequested(arguments: [], environment: ["FIREFLY_DEMO": "true"]))
        XCTAssertFalse(DemoLaunch.isRequested(arguments: [], environment: ["FIREFLY_DEMO": "0"]))
    }

    func testNeitherPresentMeansNotRequested() {
        XCTAssertFalse(DemoLaunch.isRequested(arguments: ["/path/to/app"], environment: [:]))
    }

    func testRequestedScreenReadsTheArgumentImmediatelyAfterTheFlag() {
        XCTAssertEqual(DemoLaunch.requestedScreen(arguments: ["-FireflyDemo", "-FireflyDemoScreen", "thread"]),
                       "thread")
    }

    func testRequestedScreenIsNilWithoutTheFlag() {
        XCTAssertNil(DemoLaunch.requestedScreen(arguments: ["-FireflyDemo"]))
    }

    func testRequestedScreenIsNilWhenTheFlagIsTheLastArgument() {
        XCTAssertNil(DemoLaunch.requestedScreen(arguments: ["-FireflyDemo", "-FireflyDemoScreen"]))
    }
}
