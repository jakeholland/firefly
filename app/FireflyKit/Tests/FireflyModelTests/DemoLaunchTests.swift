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

    // MARK: - M3: -FireflyDemoRestored

    func testRestoredFlagRequestsRestoredMode() {
        XCTAssertTrue(DemoLaunch.isRestoredRequested(arguments: ["-FireflyDemoRestored"], environment: [:]))
    }

    func testRestoredEnvVarRequestsRestoredMode() {
        XCTAssertTrue(DemoLaunch.isRestoredRequested(arguments: [], environment: ["FIREFLY_DEMO_RESTORED": "1"]))
    }

    func testRestoredFlagAloneAlsoImpliesPlainDemoMode() {
        // A screenshot script only ever has to pass ONE flag — see
        // `DemoLaunch.isRequested(...)`'s own doc comment.
        XCTAssertTrue(DemoLaunch.isRequested(arguments: ["-FireflyDemoRestored"], environment: [:]))
    }

    func testPlainDemoFlagDoesNotImplyRestoredMode() {
        XCTAssertFalse(DemoLaunch.isRestoredRequested(arguments: ["-FireflyDemo"], environment: [:]))
    }

    func testNeitherPresentMeansNotRestoredRequested() {
        XCTAssertFalse(DemoLaunch.isRestoredRequested(arguments: [], environment: [:]))
    }
}
