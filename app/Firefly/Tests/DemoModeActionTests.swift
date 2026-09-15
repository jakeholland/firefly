//
//  DemoModeActionTests.swift — the mode-switch decision, in isolation:
//  which direction a tap means, which dependency graph it builds, and
//  which screen `RootView` should land on. No `AppGraph`/SwiftUI/async
//  machinery here on purpose, same "the decision half is its own file,
//  its own tests" shape `RootLaunchPlanTests.swift` already follows for
//  the cold-launch landing rule.
//
import FireflyMesh
import FireflyModel
import XCTest

final class DemoModeActionTests: XCTestCase {

    // MARK: - Direction

    func testNotAlreadyInDemoRequestsEnter() {
        XCTAssertEqual(DemoModeAction.requested(isDemoMode: false), .enterDemo)
    }

    func testAlreadyInDemoRequestsLeave() {
        XCTAssertEqual(DemoModeAction.requested(isDemoMode: true), .leaveDemo)
    }

    // MARK: - Dependencies — never Bluetooth, never a real radio, on enter

    /// The one hard guarantee this whole feature rests on, restated at
    /// the level the actual button/row call: entering the demo from
    /// inside the app can never construct a `DemoMeshtasticClient`'s
    /// real-radio sibling. Mirrors `DemoRunnerTests
    /// .testLiveDependenciesNeverConstructTheDemoClient` one layer up.
    func testEnterDemoDependenciesAreTheDemoClient() {
        let dependencies = DemoModeAction.enterDemo.dependencies
        XCTAssertTrue(dependencies.client is DemoMeshtasticClient,
                      "\"Try the demo\" must build the demo world, never a real radio")
        XCTAssertTrue(dependencies.location is DemoLocationProvider)
        XCTAssertTrue(dependencies.heading is DemoHeadingProvider)
    }

    /// Leaving must never re-enter the demo world, regardless of how
    /// this process itself was launched — `AppDependencies.nonDemo()`,
    /// not `.current()` (that function's own doc comment explains why a
    /// leave action must not re-consult `DemoLaunch`).
    func testLeaveDemoDependenciesAreNeverTheDemoClient() {
        let dependencies = DemoModeAction.leaveDemo.dependencies
        XCTAssertFalse(dependencies.client is DemoMeshtasticClient,
                       "\"Leave the demo\" must never rebuild the demo world")
        XCTAssertFalse(dependencies.location is DemoLocationProvider)
        XCTAssertFalse(dependencies.heading is DemoHeadingProvider)
    }

    // MARK: - Requested screen

    /// Entering asks for exactly what `-FireflyDemoScreen find` asks
    /// for — the DEMO badge up, the crew-welcome cover lowered, a seeded
    /// crew on screen — reusing `RootView.runInitialDemoScreen()`'s own
    /// existing "find" handling rather than inventing a parallel one.
    func testEnterDemoRequestsTheFindScreen() {
        XCTAssertEqual(DemoModeAction.enterDemo.requestedScreen, "find")
    }

    /// Leaving asks for nothing — the rebuilt real stack's own
    /// `hasKnownRadio`/`hasCrew` answer decides where it lands
    /// (`RootLaunchPlan.plan`), the same rule any other launch follows.
    func testLeaveDemoRequestsNoScreen() {
        XCTAssertNil(DemoModeAction.leaveDemo.requestedScreen)
    }
}
