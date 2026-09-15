//
//  StoreTourUITests.swift — the scripted walk `app/tools/store_media.sh`
//  records over with `xcrun simctl io <udid> recordVideo` to produce the
//  App Store app-preview video. Nothing here takes the screenshot or
//  starts the recording — `simctl` owns both, from the shell, around
//  this test's own run — this file only drives the app through a
//  deliberate, paced walk of the demo stack so the resulting clip has
//  something worth recording.
//
//  GATED, on purpose: `-only-testing:FireflyUITests` (`.github/workflows
//  /app.yml`'s own `FireflyUITests (iOS Simulator)` step) runs every
//  test in this target with no per-test filter, so without a guard this
//  suite would run on every PR — slow (this test deliberately sleeps for
//  its whole duration) and pointless (there is no `simctl recordVideo`
//  wrapping it in CI, so the "recording" would be a silent no-op that
//  still burns the wall-clock cost).
//
//  The gate is a MARKER FILE, not an environment variable, and that is
//  a deliberate departure from `FireflyHardwareTests`' own
//  `FIREFLY_HARDWARE` pattern (`BLEHardwareTests.swift`) — not an
//  oversight. `FireflyHardwareTests` hosts on `platform=macOS`, where
//  `xcodebuild test` forks the XCTest bundle as an ordinary child
//  process that inherits the invoking shell's environment directly, so
//  `FIREFLY_HARDWARE=1 xcodebuild test …` reaches
//  `ProcessInfo.processInfo.environment` for free. This suite hosts on
//  an iOS SIMULATOR, where the test runner is installed and launched
//  through CoreSimulator, not forked from the invoking shell — nothing
//  there inherits the shell's environment. The documented Xcode-native
//  answer is a test plan's `environmentVariableEntries`
//  (`FireflyUITests.xctestplan`) with a `$(FIREFLY_STORE_TOUR)` macro
//  value, mirroring `Firefly.xctestplan`'s own `FIREFLY_HARDWARE`
//  entry — that was this file's first cut, and it silently failed:
//  `xcodebuild -showBuildSettings` resolves an `xcodebuild test
//  FIREFLY_STORE_TOUR=1` command-line override to `FIREFLY_STORE_TOUR =
//  1` correctly, but the LITERAL, unexpanded string `"$(FIREFLY_STORE
//  _TOUR)"` was what actually reached this process's environment —
//  confirmed by dumping `ProcessInfo.processInfo.environment` from
//  inside a real run, not by reasoning about it. No amount of
//  reshaping the override (shell-prefix vs trailing build-setting
//  argument) changed that; the macro simply does not expand against a
//  custom/undeclared build setting name for an iOS Simulator test
//  plan's `environmentVariableEntries` in this Xcode toolchain, whatever
//  the ostensible Apple-documented behavior is elsewhere.
//
//  What DOES reach this process reliably: the plain filesystem. iOS
//  Simulator test-runner processes are ordinary macOS processes with
//  the host's real filesystem visible (verified the same way, by having
//  this process write a file to `/tmp` and finding it there on the
//  host) — `store_media.sh` drops `Self.tourMarkerPath` before invoking
//  `xcodebuild test` and removes it after, win or lose, so a stale
//  marker from an interrupted run can never leave this suite silently
//  armed for an unrelated later `xcodebuild test` invocation (CI's
//  included). `XCTSkipUnless` below checks for that file's existence,
//  not an environment variable — the "never runs in CI" property is
//  unchanged: CI never creates the marker, so this always skips there.
//
//  Deliberate pauses, not the shortest possible walk: a preview video
//  wants a viewer to actually register each screen (Apple's own
//  guidance is 15-30s per clip), so this pads with `sleep` between
//  stops rather than racing through them like `FireflyUITests`' own
//  smoke test does. Total budget across the stops below is ~20s of
//  on-screen pause, comfortably inside that window once the taps and
//  transitions between them are counted too.
//
//  Screen order deliberately mirrors `store_media.sh`'s own screenshot
//  numbering (Radar hero first) so the video and the still gallery tell
//  the same story, not two different ones.
//
import XCTest

#if os(iOS)
final class StoreTourUITests: XCTestCase {
    private static let uiTimeout: TimeInterval = 30

    /// Fixed, hardcoded path — `store_media.sh`'s own header comment
    /// names this exact string too. Deliberately NOT derived from
    /// `NSTemporaryDirectory()`/`$TMPDIR`: those are per-user, per-
    /// session paths under `/var/folders/…` and are not guaranteed to
    /// resolve to the same directory in the shell that launches
    /// `xcodebuild test` and in the simulator-hosted process this
    /// runs as — plain `/tmp` is the one path both sides were actually
    /// observed sharing.
    static let tourMarkerPath = "/tmp/firefly-store-tour.enabled"

    override func setUpWithError() throws {
        continueAfterFailure = false
        try XCTSkipUnless(
            FileManager.default.fileExists(atPath: Self.tourMarkerPath),
            "create \(Self.tourMarkerPath) to run the scripted App Store tour (app/tools/store_media.sh) — never runs in CI")
    }

    /// Find/Radar (crew visible) -> Find/Map -> Find/Field -> back to
    /// Radar -> Inbox -> the CREW thread -> Lineup. One continuous
    /// launch, so the recording is one continuous clip with no relaunch
    /// flash in the middle of it.
    func testScriptedTourForAppPreviewVideo() throws {
        let app = XCUIApplication()
        // `-FireflyDemoScreen radar` skips A02's crew welcome cover
        // (`RootView.runInitialDemoScreen()`'s own "a demo screen name
        // is an explicit instruction... overrides the first-launch
        // gate") and lands directly on the hero shot.
        app.launchArguments = ["-FireflyDemo", "-FireflyDemoScreen", "radar"]
        app.launch()

        assertScreen("Screen.Radar", in: app)
        pause(4)

        tapFindSegment("Map", in: app)
        assertScreen("Screen.Map", in: app)
        pause(4)

        tapFindSegment("Field", in: app)
        assertScreen("Screen.Map.Field", in: app)
        pause(3)

        tapFindSegment("Radar", in: app)
        assertScreen("Screen.Radar", in: app)
        pause(1)

        tapTabBar("Inbox", in: app)
        assertScreen("Screen.Inbox", in: app)
        pause(2)

        let crewRow = app.buttons["InboxRow.Crew"]
        XCTAssertTrue(crewRow.waitForExistence(timeout: Self.uiTimeout), "the CREW inbox row must always exist")
        tapWhenHittable(crewRow)
        assertScreen("Screen.Thread", in: app)
        pause(4)

        // A left-edge swipe, not a tap on the back button — see
        // `FireflyUITests.popBack`'s own comment for why a synthesized
        // tap on this back button does not reliably pop on this
        // iOS/Xcode combination.
        app.swipeRight()
        assertScreen("Screen.Inbox", in: app)

        tapTabBar("Lineup", in: app)
        // `LineupScreen` has no root `accessibilityIdentifier`
        // (`assertScreen` cannot be used here) — its own header draws a
        // plain "Lineup" label, which is stable enough to wait on.
        XCTAssertTrue(app.staticTexts["Lineup"].waitForExistence(timeout: Self.uiTimeout), "Screen.Lineup did not appear")
        pause(4)
    }

    private func pause(_ seconds: TimeInterval) {
        Thread.sleep(forTimeInterval: seconds)
    }

    private func tapFindSegment(_ name: String, in app: XCUIApplication) {
        tapWhenHittable(app.buttons["Find.Segment.\(name)"])
    }

    private func tapTabBar(_ name: String, in app: XCUIApplication) {
        let button = app.tabBars.buttons[name]
        XCTAssertTrue(button.waitForExistence(timeout: Self.uiTimeout), "\(name) tab not found")
        tapWhenHittable(button)
    }

    private func assertScreen(_ identifier: String, in app: XCUIApplication) {
        XCTAssertTrue(app.descendants(matching: .any)[identifier].waitForExistence(timeout: Self.uiTimeout),
                      "\(identifier) did not appear")
    }

    private func tapWhenHittable(_ element: XCUIElement, timeout: TimeInterval = StoreTourUITests.uiTimeout) {
        XCTAssertTrue(element.waitForExistence(timeout: timeout), "element did not appear in time")
        let hittable = XCTNSPredicateExpectation(predicate: NSPredicate(format: "hittable == true"), object: element)
        XCTAssertEqual(XCTWaiter().wait(for: [hittable], timeout: timeout), .completed,
                       "element never became hittable")
        element.tap()
    }
}
#endif
