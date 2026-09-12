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
    /// Slow CI runners (GitHub's macOS/iOS Simulator hosts under load)
    /// have made a 5s `waitForExistence` flaky in this suite before
    /// (`ConnectSettingsViewModelTests`' own `eventually` helper has the
    /// full story for the equivalent unit-test-side problem) — 30s here
    /// is a generous, failure-only ceiling, never a stand-in for
    /// "probably done by now": a passing run still moves on the moment
    /// the element actually appears/becomes hittable.
    private static let uiTimeout: TimeInterval = 30

    override func setUpWithError() throws {
        continueAfterFailure = false
    }

    #if os(iOS)
    /// Connect -> Radar -> Inbox -> Thread -> Settings -> Connect,
    /// asserting each screen's own `accessibilityIdentifier`
    /// (`RootView`/`ConnectScreen`/`RadarView`/`InboxListView`/
    /// `ThreadView`/`SettingsScreen`, M3's "append-only" additions) is
    /// actually on screen before moving on — never just "the app did
    /// not crash".
    ///
    /// "app: five-tab bar per design": the tab bar itself is now
    /// Radar/Map/Inbox/Lineup/More (`RootView.Destination`) — Connect
    /// and Settings live one tap under More (`MoreScreen.swift`)
    /// instead of iOS's own auto-generated overflow list, so
    /// `tapDestination` below goes through OUR list, not a system one.
    func testDemoSmokeTapsThroughAllFiveScreens() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-FireflyDemo"]
        app.launch()

        // No radio is bonded in demo/simulator mode
        // (`RootView.hasKnownRadio`), so launch lands on More with
        // Connect already pushed (`MoreScreen`'s own `autoOpen` doc
        // comment) — no tap needed to see it first, same as this
        // screen's old "Connect is RootView's initial `selection`"
        // behavior before the five-tab rewrite.
        assertScreen("Screen.Connect", in: app)

        tapDestination("Radar", in: app)
        assertScreen("Screen.Radar", in: app)

        tapDestination("Inbox", in: app)
        assertScreen("Screen.Inbox", in: app)

        // CREW is always present (`InboxViewModel`'s own "CREW is
        // always present" rule) — a stable tap target into Thread
        // regardless of whether demo mode's crew happens to be paired
        // by the time this runs.
        let crewRow = app.buttons["InboxRow.Crew"]
        XCTAssertTrue(crewRow.waitForExistence(timeout: Self.uiTimeout), "the CREW inbox row must always exist")
        tapWhenHittable(crewRow)
        assertScreen("Screen.Thread", in: app)

        // Back to Inbox through the real interactive-pop gesture — not
        // a second, parallel way of leaving Thread — before the last
        // stop. An edge swipe, not a tap on the back button itself:
        // confirmed empirically (screenshots taken mid-run) that a
        // synthesized tap on this app's back button — by identifier,
        // reported existing AND hittable by XCUITest's own checks right
        // before the tap — does not reliably trigger the pop on this
        // iOS/Xcode combination, while the standard left-edge swipe
        // (`popGesture`) does every time.
        popBack(in: app)

        tapDestination("Settings", in: app)
        assertScreen("Screen.Settings", in: app)

        // Round back to Connect, matching the spec's own "visit all
        // destinations" framing — the walk ends where it started.
        tapDestination("Connect", in: app)
        assertScreen("Screen.Connect", in: app)
    }

    /// Taps a `Destination`'s tab directly when it still has one
    /// (Radar/Map/Inbox/Lineup), or goes through the More tab and its
    /// own row (`MoreScreen.MoreRow`'s own `accessibilityIdentifier`,
    /// `"MoreRow.<name>"`) for Connect/Settings/System — OUR list, not
    /// an iOS-auto-generated one (`RootView`'s five-tab rewrite put an
    /// end to that overflow). `MoreScreen` keeps its own push state
    /// alive across tab switches, same as every other destination's
    /// `NavigationStack` here, so a second visit can land wherever the
    /// FIRST one left off (e.g. still pushed into Connect from launch)
    /// — `returnToMoreRoot` pops back to the plain list before this
    /// looks for a row inside it.
    private func tapDestination(_ name: String, in app: XCUIApplication) {
        let direct = app.tabBars.buttons[name]
        if direct.exists {
            tapWhenHittable(direct)
            return
        }
        let more = app.tabBars.buttons["More"]
        XCTAssertTrue(more.waitForExistence(timeout: Self.uiTimeout), "More tab not found")
        tapWhenHittable(more)
        returnToMoreRoot(in: app)
        let row = app.buttons["MoreRow.\(name)"]
        XCTAssertTrue(row.waitForExistence(timeout: Self.uiTimeout),
                      "\(name) not found directly on the tab bar or as a MoreRow under More")
        tapWhenHittable(row)
    }

    /// Pops `MoreScreen`'s own `NavigationStack` back to its root list
    /// (`Screen.More`) if a previous visit left it pushed into one of
    /// its rows — see `tapDestination`'s own doc comment and `popBack`'s
    /// own doc comment for why this is a swipe, not a back-button tap.
    private func returnToMoreRoot(in app: XCUIApplication) {
        let root = app.descendants(matching: .any)["Screen.More"]
        if root.waitForExistence(timeout: 1) { return }
        popBack(in: app)
        XCTAssertTrue(root.waitForExistence(timeout: Self.uiTimeout), "Screen.More did not reappear after the pop gesture")
    }

    /// Triggers `NavigationStack`'s interactive-pop gesture with a
    /// left-edge swipe rather than tapping the back button itself.
    /// Confirmed empirically on this iOS/Xcode combination (screenshots
    /// taken mid-run, on a simulator dedicated to just this test, so not
    /// a shared-device artifact either): a synthesized tap on the back
    /// button — found and reported `hittable == true` by XCUITest's own
    /// checks immediately beforehand — does not reliably perform the
    /// pop, while this swipe does every time it was tried. Exactly one
    /// swipe, never a retry loop: every screen this suite pops from is
    /// exactly one level deep, so there is nothing legitimate left to
    /// pop a second time.
    private func popBack(in app: XCUIApplication) {
        app.swipeRight()
    }

    private func assertScreen(_ identifier: String, in app: XCUIApplication) {
        XCTAssertTrue(app.descendants(matching: .any)[identifier].waitForExistence(timeout: Self.uiTimeout),
                      "\(identifier) did not appear")
    }

    /// Waits for `element` to exist AND report itself hittable before
    /// tapping — a slow CI runner can lay an element out (`.exists`)
    /// well before its enclosing `NavigationStack` transition finishes
    /// animating it into a tappable spot, and a tap during that window
    /// is exactly the kind of flake `waitForExistence` alone cannot
    /// catch.
    private func tapWhenHittable(_ element: XCUIElement, timeout: TimeInterval = FireflyUITests.uiTimeout,
                                  file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertTrue(element.waitForExistence(timeout: timeout), "element did not appear in time",
                      file: file, line: line)
        let hittable = XCTNSPredicateExpectation(predicate: NSPredicate(format: "hittable == true"), object: element)
        let result = XCTWaiter().wait(for: [hittable], timeout: timeout)
        XCTAssertEqual(result, .completed, "element never became hittable", file: file, line: line)
        element.tap()
    }
    #endif

    #if os(macOS)
    /// See this file's own header comment for why this is launch-only,
    /// no `-FireflyDemo`, and no navigation walk.
    func testLaunchesToConnectScreen() throws {
        let app = XCUIApplication()
        app.launch()

        XCTAssertTrue(app.descendants(matching: .any)["Screen.Connect"].waitForExistence(timeout: Self.uiTimeout),
                      "Screen.Connect did not appear on launch")
    }
    #endif
}
