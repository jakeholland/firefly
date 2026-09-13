//
//  AppGraphFestpackAutoRefreshTests.swift — the ONE rule gating
//  `AppGraph`'s automatic festpack refresh (review finding on "app:
//  automatic almanac refresh + festival picker").
//
//  `.stub()` — the stack every unit test composes — carries
//  `StubMeshtasticClient`, not `DemoMeshtasticClient`, so `AppGraph.init`
//  builds a real `AlmanacFestpackProvider` over a real
//  `URLSessionFestpackFetcher` for it. Before this gate, `start()`'s
//  `refreshIfNeeded()` therefore issued a live HTTPS GET to
//  raw.githubusercontent.com from inside `swift test`/`xcodebuild test`
//  on any machine whose Application Support cache was cold — i.e. every
//  CI runner. (It did NOT reproduce on a dev box, because that cache is
//  warm and inside the 6-hour staleness window: precisely the kind of
//  "green locally, different on CI" the repo's own compiler note warns
//  about.)
//
//  The refresh POLICY itself is covered where it lives —
//  `AlmanacFestpackProviderTests` drives `refreshIfNeeded()` directly
//  against a stub fetcher — so nothing is lost by not exercising it
//  through the graph.
//
@testable import FireflyModel
import XCTest

@MainActor
final class AppGraphFestpackAutoRefreshTests: XCTestCase {
    func testAutomaticRefreshIsSuppressedUnderXCTestAndAllowedOtherwise() {
        XCTAssertFalse(AppGraph.shouldAutoRefreshFestpack(isRunningUnderXCTest: true),
                       "a unit test must never reach the network on its own")
        XCTAssertTrue(AppGraph.shouldAutoRefreshFestpack(isRunningUnderXCTest: false),
                      "a real app launch still auto-refreshes — that is the whole feature")
    }

    /// The half that keeps the rule above from being a tautology: this
    /// process really is detected as a test host, so the gate is
    /// genuinely engaged for every test in this suite rather than being
    /// a pure function nothing ever feeds `true`.
    ///
    /// Measured, and the reason the gate does NOT reuse
    /// `AppGraph.isRunningUnderXCTest`: under a bare `swift test` the
    /// `XCTestConfigurationFilePath` variable that property reads is
    /// not set at all (this process's environment carries only
    /// `SWIFT_TESTING_ENABLED`), so it reports false right here, in the
    /// FireflyKit unit suite. Pinned below so a future toolchain change
    /// in either direction shows up as a test result rather than as a
    /// silent live fetch on CI.
    func testThisProcessIsDetectedAsATestHost() {
        XCTAssertTrue(AppGraph.isXCTestRuntimeLoaded,
                      "the XCTest runtime is loaded into every swift test / xcodebuild test process")
        XCTAssertFalse(AppGraph.isRunningUnderXCTest,
                       "documenting the measured gap: swift test does not set XCTestConfigurationFilePath")
    }
}
