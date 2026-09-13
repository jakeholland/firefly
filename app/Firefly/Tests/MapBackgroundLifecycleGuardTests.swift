//
//  MapBackgroundLifecycleGuardTests.swift — a SOURCE-level invariant
//  guard, the same genre as `BLEScanTeardownGuardTests`' BLETransport
//  parse and `ThemeTests`' ff_theme.h parse: it reads
//  `MapTabView.swift` and asserts a structural property, because the
//  behaviour it protects is SwiftUI's own `TabView`/`scenePhase`
//  plumbing, which no unit test in this target can drive.
//
//  The property: `MapTabView`'s `scenePhase` observer restarts
//  `MapViewModel.observe()` only while the Map tab is actually on
//  screen, and stops it unconditionally on `.background`.
//
//  The bug this pins (PR #294 review). The hardening pass added a
//  `scenePhase` observer to stop the Map's 1 Hz `pinRefreshLoop`, its
//  three subscriptions and its `NWPathMonitor` when the app is
//  backgrounded — `.onDisappear` does not fire then, and this app
//  declares `UIBackgroundModes` so it genuinely keeps running. The stop
//  half was right. The restart half was `case .active: model.observe()`,
//  unconditional, justified by "if the tab is not showing, this view is
//  not in the hierarchy and nothing here runs at all".
//
//  Measured on an iPhone 17 Pro simulator (iOS 26.5) with an
//  instrumented build, backgrounding via another app and foregrounding
//  again, that justification is only half true:
//
//    * Map tab NEVER visited -> `.onChange` never fires. True.
//    * Map tab visited and then switched away from -> the view stays in
//      the `TabView`'s hierarchy and `.onChange` fires for BOTH
//      `.background` and `.active`. False.
//
//  So on every foreground after the user had ever opened Map and moved
//  on, the whole loop came back up for a screen nobody could see, with
//  nothing to stop it until the next backgrounding — the same leak the
//  observer was added to close, just relocated off-screen.
//
//  A structural guard is weaker than a behavioural test and is not
//  pretending otherwise: it cannot drive SwiftUI. It CAN prove nobody
//  drops the on-screen guard again, which is exactly how this got in.
//
import XCTest

final class MapBackgroundLifecycleGuardTests: XCTestCase {

    private func mapTabViewSource() throws -> String {
        let url = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent() // MapBackgroundLifecycleGuardTests.swift -> Tests
            .deletingLastPathComponent() // Tests -> Firefly
            .appending(path: "Sources/Map/MapTabView.swift")
        return try String(contentsOf: url, encoding: .utf8)
    }

    /// Comment lines are excluded: this fix is documented in prose that
    /// quotes the old unconditional restart, and a guard that matched
    /// its own explanation would be unmaintainable.
    private func codeLines(_ source: String) -> [String] {
        source.split(separator: "\n", omittingEmptySubsequences: false)
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.hasPrefix("//") }
    }

    /// The `scenePhase` observer's body, code only — comments are
    /// stripped BEFORE the search, because the prose above the modifier
    /// quotes `.onChange(of: scenePhase)` itself and a parser that
    /// matched its own explanation would read the wrong block.
    ///
    /// Bounded by the next view modifier (a line starting `.`), which
    /// is what actually ends this one.
    private func scenePhaseObserver() throws -> [String] {
        let lines = codeLines(try mapTabViewSource())
        let start = try XCTUnwrap(
            lines.firstIndex { $0.hasPrefix(".onChange(of: scenePhase)") },
            "MapTabView must observe scenePhase — `.onDisappear` does not fire on backgrounding, so "
            + "without this the Map's 1 Hz loop runs in the user's pocket")
        let rest = lines[(start + 1)...]
        let end = rest.firstIndex { $0.hasPrefix(".") } ?? rest.endIndex
        return Array(rest[..<end])
    }

    func testTheForegroundRestartIsGatedOnBeingOnScreen() throws {
        let active = try XCTUnwrap(try scenePhaseObserver().first { $0.hasPrefix("case .active") },
                                   "the observer must handle .active — otherwise foregrounding leaves a "
                                   + "visible Map frozen")
        XCTAssertTrue(active.contains("isOnScreen"),
                      "`case .active` must restart the Map only while it is the tab on screen. A deselected "
                      + "tab stays in the TabView's hierarchy and still receives this event (measured, iOS "
                      + "26.5), so an unconditional restart wakes the 1 Hz pinRefreshLoop, three "
                      + "subscriptions and an NWPathMonitor for a screen nobody can see. Found: \(active)")
        XCTAssertTrue(active.contains("model.observe()"),
                      "and it must still actually restart it. Found: \(active)")
    }

    /// The other half, so the guard above cannot be satisfied by simply
    /// never restarting: backgrounding must still stop everything,
    /// unconditionally — a backgrounded app has no on-screen tab at all.
    func testBackgroundingStopsUnconditionally() throws {
        let background = try XCTUnwrap(try scenePhaseObserver().first { $0.hasPrefix("case .background") },
                                       "the observer must handle .background — that is its whole reason to exist")
        XCTAssertTrue(background.contains("model.stopObserving()"),
                      "backgrounding must stop the Map's loop. Found: \(background)")
        XCTAssertFalse(background.contains("isOnScreen"),
                       "the STOP must not be conditional: a backgrounded app has no tab on screen, and the "
                       + "one case that matters most is the Map being the tab that was showing. "
                       + "Found: \(background)")
    }

    /// `isOnScreen` is only honest if the two events that define it are
    /// the two that maintain it.
    func testOnScreenIsTrackedByAppearAndDisappear() throws {
        let lines = codeLines(try mapTabViewSource())
        XCTAssertTrue(lines.contains { $0.hasPrefix(".onAppear") && $0.contains("isOnScreen = true") },
                      "`.onAppear` must record that this tab is on screen")
        XCTAssertTrue(lines.contains { $0.hasPrefix(".onDisappear") && $0.contains("isOnScreen = false") },
                      "`.onDisappear` must record that it no longer is — that is the transition the "
                      + "foreground restart has to be able to see")
    }
}
