//
//  NotificationTapUITests.swift — A03 §3.11.3, the tap itself.
//
//  `xcrun simctl` cannot tap a notification; SpringBoard can, and
//  XCUITest can drive SpringBoard (`XCUIApplication(bundleIdentifier:
//  "com.apple.springboard")`). The app schedules a REAL local
//  notification for itself through the shipping transcription path
//  (`-FireflyDebugNotify`, `FireflyDebugNotifyLaunch` + `UNNotification
//  Sending.scheduleDebugNotification`), the test backgrounds or
//  terminates the app, waits for the banner, and taps it.
//
//  Two cases, because they are two different code paths (§3.1):
//
//  - WARM: the app is backgrounded. The process is alive, `RootView`
//    already exists, and the tap is an ordinary `didReceive` into a
//    live `DeepLinkRouter`.
//  - COLD: the app has been terminated. `didFinishLaunchingWithOptions`
//    runs first, then the scene attaches, and the notification response
//    arrives around the same time as the first view update — which is
//    the ordering the crash in build 328 lives in.
//
//  Both assert the same thing: the app comes to the foreground, on the
//  screen the deep link named, and is STILL RUNNING a beat later. That
//  last check is the one that catches a crash-on-tap: an app that
//  crashed on launch-from-notification can flash its first frame before
//  going away, so "an element appeared" alone is not proof it survived.
//
import XCTest

#if os(iOS)
final class NotificationTapUITests: XCTestCase {
    private static let uiTimeout: TimeInterval = 30
    /// Computed, not a stored `let`: `XCUIApplication`'s initializer is
    /// main-actor isolated and a stored property's default value is a
    /// nonisolated context, which Swift 6 rejects. `XCUIApplication` is
    /// a proxy, not a connection — constructing one per use is free.
    private var springboard: XCUIApplication { XCUIApplication(bundleIdentifier: "com.apple.springboard") }

    override func setUpWithError() throws {
        continueAfterFailure = false
    }

    /// A tapped message notification opens its thread, with the app
    /// already running in the background. `Screen.Thread` is a real
    /// assertion about the routing and not an artefact of the launch
    /// arguments: `-FireflyDemoScreen inbox` lands on the Inbox LIST,
    /// and nothing but the deep link pushes a thread on top of it.
    func testTappingMessageNotificationWhileBackgroundedOpensThread() throws {
        let app = launchAndScheduleNotification(kind: "thread")
        XCUIDevice.shared.press(.home)
        tapBanner()
        assertForegroundedAndAlive(app, screen: "Screen.Thread")
    }

    /// The same tap, with the app TERMINATED first — the cold-launch
    /// ordering (`didFinishLaunchingWithOptions` -> scene -> response).
    ///
    /// And deliberately WITHOUT `-FireflyDemoScreen`, unlike the two
    /// warm cases: that argument lands the launch on Inbox and lowers
    /// A02 §6.1's crew welcome (`runInitialDemoScreen()`'s own "a demo
    /// screen name is an explicit instruction … so it OVERRIDES the
    /// first-launch gate"), which would hide the defect this case
    /// exists to pin. With no override, `applyInitialSelection()` is
    /// the only other thing with an opinion about where a cold launch
    /// lands, and on this simulator — no radio bonded, no crew code
    /// stored — its old unconditional `selection = .find` +
    /// `showCrewOnboarding = true` put the crew welcome on screen with
    /// the routed thread behind it. Hence the second assertion below:
    /// Thread present is not enough, the cover must be absent.
    func testTappingMessageNotificationAfterTerminationOpensThread() throws {
        let app = launchAndScheduleNotification(kind: "thread", demoScreen: nil)
        XCUIDevice.shared.press(.home)
        // Give the backgrounded app the moment it needs to actually
        // register the request before killing it. MEASURED: the app
        // schedules from its `scenePhase` -> `.background` handler, in a
        // `Task`, so a `terminate()` fired immediately after the Home
        // press wins that race and nothing is ever delivered — this test
        // (and only this test, the two warm ones being unaffected)
        // failed exactly that way with "the scheduled notification
        // banner never appeared". Once the request is in
        // `usernotificationsd` it outlives the process, which is the
        // whole point of the cold case.
        Thread.sleep(forTimeInterval: 2)
        app.terminate()
        tapBanner()
        assertForegroundedAndAlive(app, screen: "Screen.Thread")
        // The cold-launch deep-link defect, stated as its own assertion
        // rather than folded into "Screen.Thread appeared": the thread
        // push lives inside the Inbox tab's own `NavigationStack` and
        // SURVIVES being covered, so a query that only asked whether the
        // element exists would have passed against the broken code —
        // `docs/review/code-review.md` item 6, exactly. What the fix
        // changes is whether anything is on top of it.
        XCTAssertFalse(app.descendants(matching: .any)["Screen.CrewWelcome"].exists,
                       "A02 §6.1's crew welcome must not cover a cold-launch deep link: a tap that "
                       + "launched the app has already said where it wants to go, and this install has "
                       + "no crew code, which is every device on its first evening")
    }

    /// A tapped FLARE lands on Find ▸ Radar (§3.11.3) — again a real
    /// assertion, because the launch arguments land this run on Inbox.
    /// Warm only: the cold variant would race `RootView
    /// .runInitialDemoScreen()`'s own `selection = .inbox` (it awaits
    /// `DemoRunner.waitUntilStarted()` and then assigns), and a test
    /// that sometimes loses a race it is not about is worse than one
    /// test fewer. The thread deep link above is immune to that race —
    /// its push lives INSIDE the Inbox tab's own NavigationStack — which
    /// is why the cold case is covered there.
    func testTappingFlareNotificationWhileBackgroundedOpensRadar() throws {
        let app = launchAndScheduleNotification(kind: "flare")
        XCUIDevice.shared.press(.home)
        tapBanner()
        assertForegroundedAndAlive(app, screen: "Screen.Radar")
    }

    // MARK: - Helpers

    /// Launches in demo mode (so the deep link names a member the
    /// seeded roster actually holds) with the repro seam armed, grants
    /// the notification permission through SpringBoard's own alert, and
    /// returns with the notification scheduled but not yet delivered.
    private func launchAndScheduleNotification(kind: String,
                                               demoScreen: String? = "inbox") -> XCUIApplication {
        let app = XCUIApplication()
        // `-FireflyDemo` is always on: it seeds the roster, so the repro
        // notification's deep link names somebody who actually exists
        // (`FireflyDebugNotifyLaunch.reproNodeID`).
        //
        // `-FireflyDemoScreen inbox` is the WARM cases' way of taking
        // the first-launch gate out of the picture — it lands the launch
        // on a tab and lowers A02 §6.1's crew welcome — so those tests
        // are about the tap and nothing else. The cold case passes `nil`
        // on purpose; see its own comment.
        app.launchArguments = ["-FireflyDemo"]
            + (demoScreen.map { ["-FireflyDemoScreen", $0] } ?? [])
            + ["-FireflyDebugNotify", kind]
        app.launch()
        allowNotificationsIfAsked()
        return app
    }

    /// The system permission alert is SpringBoard's, not the app's. It
    /// appears only on the first launch against a given simulator; a
    /// non-failing wait, so a second run is not a failure.
    ///
    /// How long this takes no longer matters to the outcome: the app
    /// schedules its repro notification off the BACKGROUND transition
    /// (`FireflyApp`'s own `scenePhase` handler), not off a delay from
    /// launch, so nothing here can race the delivery. That was not true
    /// of the first two versions of this suite, and both flaked for it.
    private func allowNotificationsIfAsked() {
        let allow = springboard.buttons["Allow"]
        if allow.waitForExistence(timeout: 10) {
            allow.tap()
        }
    }

    /// Waits for the banner and taps it. The banner is a SpringBoard
    /// element; matched on the notification's own title ("Taylor",
    /// "Taylor needs you") rather than by index, so this cannot silently
    /// tap some other app's notification.
    ///
    /// A POLL, not `waitForExistence` + `tap()`, and that is a measured
    /// distinction: a banner auto-dismisses a few seconds after it
    /// appears, and the split-second between XCUITest resolving the
    /// element and synthesizing the tap is long enough to lose it
    /// ("Failed to tap Other (First Match): No matches found", seen once
    /// on this suite's flare case while the other two passed). Re-query
    /// and re-check `isHittable` immediately before every tap so the
    /// window is milliseconds rather than a whole query round trip.
    private func tapBanner() {
        let deadline = Date().addingTimeInterval(Self.uiTimeout)
        while Date() < deadline {
            let banner = springboard.descendants(matching: .any)
                .matching(NSPredicate(format: "label CONTAINS[c] %@", "Taylor")).firstMatch
            if banner.exists, banner.isHittable {
                banner.tap()
                return
            }
            Thread.sleep(forTimeInterval: 0.25)
        }
        XCTFail("the scheduled notification banner never appeared, or never became tappable")
    }

    /// The whole point of this suite: the app must be FOREGROUNDED, on
    /// the right screen, and still running afterwards.
    private func assertForegroundedAndAlive(_ app: XCUIApplication, screen: String) {
        XCTAssertTrue(app.descendants(matching: .any)[screen].waitForExistence(timeout: Self.uiTimeout),
                      "\(screen) did not appear after tapping the notification")
        // A crash on the notification-tap path can still render a first
        // frame. Re-check a beat later: `runningForeground` here means
        // the process survived the routing, not just that it started.
        let stillUp = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "state == %d", XCUIApplication.State.runningForeground.rawValue),
            object: app)
        XCTAssertEqual(XCTWaiter().wait(for: [stillUp], timeout: 5), .completed,
                       "the app did not stay in the foreground after the notification tap")
        Thread.sleep(forTimeInterval: 2)
        XCTAssertEqual(app.state, .runningForeground,
                       "the app left the foreground (crashed?) after the notification tap")
    }
}
#endif
