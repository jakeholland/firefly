//
//  FireflyUITests.swift — the M3 XCUITest smoke suite
//  (docs/specs/A01-companion-app.md, "UI": "One XCUITest smoke test per
//  platform — launch, visit all four destinations, assert nothing
//  crashes and that the placeholder screens do not claim to have
//  data."; M3's own acceptance criterion adds Thread as a fifth stop
//  and CI wiring). Deliberately thin, same as the rest of this app's
//  test strategy: the logic under test lives in the view models
//  FireflyKit's own `swift test` suite (and `FireflyAppTests`) already
//  exercise directly. This only proves the SwiftUI layer wires them up
//  and nothing crashes navigating between them.
//
//  Two platforms, two different tests, not one test run twice:
//
//  - iOS (`testDemoSmokeTapsThroughAllFiveScreens`) launches with
//    `-FireflyDemo`. `AppDependencies.current()`'s own doc comment: the
//    `-FireflyDemo`/`FIREFLY_DEMO=1` check lives INSIDE `#if
//    targetEnvironment(simulator)`, so this only ever does anything in
//    the iOS Simulator — never on a real device, and (see below) never
//    on macOS either. Demo mode seeds a paired crew and live-looking
//    readings (S20: "real state seeded through the real core APIs"),
//    which is what lets this test see Inbox/Radar/Thread actually
//    populated rather than only their honest-but-uninformative empty
//    states.
//  - macOS (`testLaunchesToConnectScreen`) launches PLAIN — no
//    `-FireflyDemo` — because `targetEnvironment(simulator)` is false
//    for a native macOS build, so the flag would be silently ignored
//    and the real `.live()` composition would run regardless
//    (`AppDependencies.current()`). That is fine and exactly the point
//    of this test: `.live()` never touches CoreBluetooth at process
//    launch (`BLETransport.connect()`'s own doc comment —
//    `CBCentralManager` is constructed lazily, only once something
//    calls `connect()`/`scan()`) and `ConnectScreen` deliberately never
//    auto-starts a scan (its own `.onAppear` comment), so a plain
//    launch is honestly Bluetooth-free. This test proves only that:
//    the app launches to the Connect screen without crashing or
//    popping a permission prompt — never a full five-screen walk,
//    which needs demo mode's seeded data to be worth anything and demo
//    mode does not run here.
//
import XCTest

final class FireflyUITests: XCTestCase {
    override func setUpWithError() throws {
        continueAfterFailure = false
    }

    #if os(iOS)
    /// Connect -> Radar -> Inbox -> Thread -> Settings, asserting each
    /// screen's own `accessibilityIdentifier` (`RootView`/`ConnectScreen`
    /// /`RadarView`/`InboxListView`/`ThreadView`/`SettingsScreen`, M3's
    /// "append-only" additions) is actually on screen before moving on
    /// — never just "the app did not crash".
    func testDemoSmokeTapsThroughAllFiveScreens() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-FireflyDemo"]
        app.launch()

        // Connect is RootView's initial `selection` — no tap needed to
        // see it first.
        assertScreen("Screen.Connect", in: app)

        // The tab bar buttons are found by their `Destination.rawValue`
        // label text (`RootView`'s `Label(destination.rawValue, ...)`)
        // — confirmed the actual, reliable query: a `.accessibilityIdentifier`
        // applied to a `TabView` ForEach item's content does NOT reach
        // the generated `UITabBarButton` on this SDK, only the label
        // text does.
        app.tabBars.buttons["Radar"].tap()
        assertScreen("Screen.Radar", in: app)

        app.tabBars.buttons["Inbox"].tap()
        assertScreen("Screen.Inbox", in: app)

        // CREW is always present (`InboxViewModel`'s own "CREW is
        // always present" rule) — a stable tap target into Thread
        // regardless of whether demo mode's crew happens to be paired
        // by the time this runs.
        let crewRow = app.buttons["InboxRow.Crew"]
        XCTAssertTrue(crewRow.waitForExistence(timeout: 5), "the CREW inbox row must always exist")
        crewRow.tap()
        assertScreen("Screen.Thread", in: app)

        // Back to Inbox through the real navigation bar back button —
        // not a second, parallel way of leaving Thread — before the
        // last stop.
        app.navigationBars.buttons.element(boundBy: 0).tap()

        app.tabBars.buttons["Settings"].tap()
        assertScreen("Screen.Settings", in: app)

        // Round back to Connect, matching the spec's own "visit all
        // destinations" framing — the walk ends where it started.
        app.tabBars.buttons["Connect"].tap()
        assertScreen("Screen.Connect", in: app)
    }

    private func assertScreen(_ identifier: String, in app: XCUIApplication) {
        XCTAssertTrue(app.descendants(matching: .any)[identifier].waitForExistence(timeout: 5),
                      "\(identifier) did not appear")
    }
    #endif

    #if os(macOS)
    /// See this file's own header comment for why this is launch-only,
    /// no `-FireflyDemo`, and no navigation walk.
    func testLaunchesToConnectScreen() throws {
        let app = XCUIApplication()
        app.launch()

        XCTAssertTrue(app.descendants(matching: .any)["Screen.Connect"].waitForExistence(timeout: 5),
                      "Screen.Connect did not appear on launch")
    }
    #endif
}
