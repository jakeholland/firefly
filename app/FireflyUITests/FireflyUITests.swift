//
//  FireflyUITests.swift — the M3 XCUITest smoke suite
//  (docs/specs/A01-companion-app.md, "UI": "One XCUITest smoke test per
//  platform — launch, visit all four destinations, assert nothing
//  crashes and that the placeholder screens do not claim to have
//  data."; M3's own acceptance criterion adds Thread as a fifth stop
//  and CI wiring; the owner's 2026-09-13 Find-tab decision folds Radar
//  and Map into one tab's three segments, which is why the iOS test
//  below is `testDemoSmokeTapsThroughAllScreens`, not "...AllFiveScreens"
//  any more — "five" stopped being an honest count of anything once two
//  of those five became segments of a fourth tab). Deliberately thin,
//  same as the rest of this app's test strategy: the logic under test
//  lives in the view models FireflyKit's own `swift test` suite (and
//  `FireflyAppTests`) already exercise directly. This only proves the
//  SwiftUI layer wires them up and nothing crashes navigating between
//  them.
//
//  Two platforms, two different tests, not one test run twice:
//
//  - iOS (`testDemoSmokeTapsThroughAllScreens`) launches with
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
//    popping a permission prompt — never a full screen-by-screen walk,
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
    /// Connect -> Find/Radar -> Find/Map -> Find/Field -> Find/Radar ->
    /// Inbox -> Thread -> Settings -> Connect, asserting each screen's
    /// own `accessibilityIdentifier` (`RootView`/`ConnectScreen`/
    /// `RadarView`/`MapTabView`/`FieldMapView`/`InboxListView`/
    /// `ThreadView`/`SettingsScreen`, M3's "append-only" additions) is
    /// actually on screen before moving on — never just "the app did
    /// not crash".
    ///
    /// "app: Find tab — Radar · Map · Field segments, four-tab bar
    /// (owner decision)": the tab bar itself is now Find/Inbox/Lineup/
    /// More (`RootView.Destination`), with Find's own segmented control
    /// standing in for the two tabs (Radar, Map) it replaces — Connect
    /// and Settings still live one tap under More (`MoreScreen.swift`)
    /// instead of iOS's own auto-generated overflow list, so
    /// `tapDestination` below goes through OUR list, not a system one.
    /// Every `accessibilityIdentifier` this test asserts on
    /// (`Screen.Radar`, `Screen.Map`, `Screen.Map.GPS`,
    /// `Screen.Map.Field`) is exactly the one the old five-tab layout
    /// used — kept unchanged so this assertion stays just as strong,
    /// only reached through the segmented control now instead of a tab
    /// bar button.
    func testDemoSmokeTapsThroughAllScreens() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-FireflyDemo"]
        app.launch()

        // A02 §6.1: no known radio AND no crew code -> the crew welcome
        // replaces "launch lands on More with Connect pre-pushed" as
        // the zero-known-radio destination. "Connect your puck" is the
        // escape hatch back to the plain radio picker, which is what
        // the rest of this walk (predating A02) still exercises.
        assertScreen("Screen.CrewWelcome", in: app)
        tapWhenHittable(app.descendants(matching: .any)["CrewWelcome.ConnectPuck"])
        assertScreen("Screen.Connect", in: app)

        tapDestination("Find", in: app)
        // Radar is Find's default segment (owner decision) — no
        // segmented-control tap needed to see it first.
        assertScreen("Screen.Radar", in: app)

        tapFindSegment("Map", in: app)
        assertScreen("Screen.Map", in: app)
        assertScreen("Screen.Map.GPS", in: app)

        tapFindSegment("Field", in: app)
        assertScreen("Screen.Map.Field", in: app)

        // Back to Radar, proving the segmented control (not just the
        // tab bar) is a genuine two-way switch.
        tapFindSegment("Radar", in: app)
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

    /// A02 §6.1's connect-first flow, on the launch that actually has
    /// no radio: a PLAIN simulator launch (no `-FireflyDemo`) runs
    /// `AppDependencies.stub()`, whose client is never connected and
    /// whose scanner is nil — the honest "I just installed this and my
    /// puck is in my bag" state, and exactly the one the owner hit on
    /// build 328 ("tried to join but nothing happened").
    ///
    /// Welcome -> Connect your puck -> Join, asserting the thing that
    /// was missing: JOIN is disabled, the reason is ON SCREEN next to
    /// it, and the persistent banner offers the connect step. A tap that
    /// silently does nothing is what this pins shut.
    func testFirstLaunchWithNoPuckGoesThroughTheConnectStepAndGatesJoin() throws {
        let app = XCUIApplication()
        app.launch()

        assertScreen("Screen.CrewWelcome", in: app)
        tapWhenHittable(app.descendants(matching: .any)["CrewWelcome.Join"])

        // The connect step, because no puck is connected. (With one, the
        // container skips it entirely — `testDemoSmokeSkipsTheConnectStepWhenAPuckIsAlreadyConnected`.)
        assertScreen("Screen.CrewConnectPuck", in: app)
        XCTAssertTrue(app.descendants(matching: .any)["CrewConnect.Status"].waitForExistence(timeout: Self.uiTimeout),
                      "the connect step must say what the link is doing")
        XCTAssertTrue(app.buttons["CrewConnect.Rescan"].exists, "RESCAN must be reachable")
        XCTAssertTrue(app.descendants(matching: .any)["CrewConnect.NoPuck"].exists,
                      "\"Don't have a puck yet?\" must be reachable")

        // "Do this later" goes ON to Join rather than dead-ending, so
        // the banner can explain in place.
        tapWhenHittable(app.descendants(matching: .any)["CrewConnect.Later"])
        assertScreen("Screen.CrewJoin", in: app)

        XCTAssertTrue(app.descendants(matching: .any)["CrewBanner.NeedsRadio"].waitForExistence(timeout: Self.uiTimeout),
                      "Join must carry the persistent \"connect your puck\" banner with no radio")
        XCTAssertTrue(app.descendants(matching: .any)["CrewJoin.DisabledReason"].exists,
                      "the reason JOIN is disabled must be visible without tapping it")
        XCTAssertFalse(app.buttons["CrewJoin.Join"].isEnabled,
                       "JOIN must not be tappable while it could only be a no-op")

        // …and the banner's CONNECT goes back to the same one connect
        // step, not a second parallel one.
        tapWhenHittable(app.descendants(matching: .any)["CrewBanner.Connect"])
        assertScreen("Screen.CrewConnectPuck", in: app)
        dismissCameraPermissionAlertIfPresent(timeout: 2)
    }

    /// The other half of §6.1's rule — "the step is skipped
    /// automatically when already connected". Demo mode connects its
    /// client during `DemoRunner.start()`, so JOIN A CREW here must go
    /// straight to Join, with no banner and a live JOIN button.
    func testDemoSmokeSkipsTheConnectStepWhenAPuckIsAlreadyConnected() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-FireflyDemo"]
        app.launch()

        assertScreen("Screen.CrewWelcome", in: app)
        tapWhenHittable(app.descendants(matching: .any)["CrewWelcome.Join"])
        assertScreen("Screen.CrewJoin", in: app)
        XCTAssertFalse(app.descendants(matching: .any)["Screen.CrewConnectPuck"].exists,
                       "a connected puck must not be asked to connect again")
        XCTAssertFalse(app.descendants(matching: .any)["CrewBanner.NeedsRadio"].exists,
                       "no banner when a puck is connected")
        dismissCameraPermissionAlertIfPresent()
    }

    /// Answers the camera-permission alert `Screen.CrewJoin` raises, so
    /// it does not outlive this test.
    ///
    /// Review of PR #319: landing on Join starts `CrewScannerCard`'s
    /// `AVCaptureSession`, and on a simulator that has never been asked,
    /// iOS puts up a system-modal camera alert. It belongs to
    /// **Springboard**, not to this app, so terminating the app at the
    /// end of a test does not take it away — it stays on screen and
    /// every tap in the NEXT test lands on it instead. Measured, not
    /// theorised: on a freshly created `iPhone 17 Pro` this suite failed
    /// `testDemoSmokeTapsThroughAllScreens` ("Screen.Connect did not
    /// appear") twice in a row, passed that test when run on its own,
    /// and passed all three with `simctl privacy … grant camera`
    /// pre-applied. Before this PR no iOS UI test ever reached a screen
    /// with a camera on it, which is why it has not bitten before.
    ///
    /// Not `addUIInterruptionMonitor`: that fires only while a tap is
    /// being attempted on the app, and the alert here is raised by a
    /// screen appearing, then sits through the end of the test with
    /// nothing else to interrupt.
    private func dismissCameraPermissionAlertIfPresent(timeout: TimeInterval = 10) {
        let springboard = XCUIApplication(bundleIdentifier: "com.apple.springboard")
        let alert = springboard.alerts.firstMatch
        guard alert.waitForExistence(timeout: timeout) else { return }
        // Either answer clears it — a simulator has no camera to grant
        // access to — so take whichever this iOS version offers rather
        // than pinning one button's exact wording.
        for label in ["Allow", "OK", "Continue", "Don't Allow"] where alert.buttons[label].exists {
            alert.buttons[label].tap()
            return
        }
        alert.buttons.firstMatch.tap()
    }

    /// Taps one of `FindScreen`'s own segmented-control buttons
    /// (`FindScreen.swift`'s `"Find.Segment.<name>"` identifiers) —
    /// plain buttons, not a native segmented control, so this is a
    /// direct button lookup, same shape as `tapDestination`'s own
    /// `MoreRow.<name>` lookup just below.
    private func tapFindSegment(_ name: String, in app: XCUIApplication) {
        let button = app.buttons["Find.Segment.\(name)"]
        tapWhenHittable(button)
    }

    /// Taps a `Destination`'s tab directly when it still has one
    /// (Find/Inbox/Lineup), or goes through the More tab and its own
    /// row (`MoreScreen.MoreRow`'s own `accessibilityIdentifier`,
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

        // A02 §6.1 — same destination change as the iOS smoke test
        // above: a fresh launch with no known radio and no crew code
        // shows the crew welcome first; "Connect your puck" reaches the
        // plain radio picker this test originally asserted on directly.
        XCTAssertTrue(app.descendants(matching: .any)["Screen.CrewWelcome"].waitForExistence(timeout: Self.uiTimeout),
                      "Screen.CrewWelcome did not appear on launch")
        app.descendants(matching: .any)["CrewWelcome.ConnectPuck"].tap()
        XCTAssertTrue(app.descendants(matching: .any)["Screen.Connect"].waitForExistence(timeout: Self.uiTimeout),
                      "Screen.Connect did not appear after tapping Connect your puck")
    }
    #endif
}
