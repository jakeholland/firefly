//
//  FindLifecycleWiringGuardTests.swift — a SOURCE-level invariant
//  guard, the same genre as `MapBackgroundLifecycleGuardTests`' own
//  `MapTabView.swift` parse: it reads `RootView.swift` and
//  `FindScreen.swift` and asserts a structural property, because the
//  behaviour it protects is SwiftUI's own `NavigationSplitView`
//  mounting, which no unit test in this target can drive.
//
//  The property: the Find tab's start/stop rule (`FindLifecycle`,
//  owner decision 2026-09-13 — "only the visible segment's view model
//  observes/pumps") is driven from `RootView`'s own `selection`/
//  `findSegment` state, NEVER from `FindScreen`'s own `.onAppear`/
//  `.onDisappear`.
//
//  The bug this pins (PR #298 review). Wiring it to `FindScreen`'s own
//  mount events is the obvious place, and it is wrong for the same
//  reason `ConnectScreen.swift`/`RadarView.swift`/`InboxListView.swift`/
//  `SettingsScreen.swift` each document: `FindScreen` is one of
//  `RootView.detail(for:)`'s destinations, and that detail column
//  remounts once at launch. Measured on macOS (Xcode 26.6, an
//  instrumented build of this branch, logging each event to a file so
//  nothing depended on reading a console):
//
//      applyInitialSelection -> .find
//      onAppear segment=radar
//      onAppear segment=radar
//      onDisappear
//
//  Two mounts, and the FIRST instance's `onDisappear` arriving AFTER
//  the second's `onAppear` — so `FindLifecycle.stopAll` ran last and
//  left BOTH `RadarViewModel` and `MapViewModel` stopped while Find was
//  on screen. A frozen Radar on the app's own landing destination, for
//  the rest of the process or until the user happened to tap a segment.
//
//  `selection`/`findSegment` are plain `@State` on `RootView`, which
//  that remount does not touch, so the identical rule applied from
//  there cannot be orphaned by it.
//
//  A structural guard is weaker than a behavioural test and is not
//  pretending otherwise: it cannot drive SwiftUI. It CAN prove nobody
//  moves this back into the view that the remount hits, which is
//  exactly how it got in.
//
import XCTest

final class FindLifecycleWiringGuardTests: XCTestCase {

    private func source(_ relativePath: String) throws -> [String] {
        let url = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent() // this file -> Tests
            .deletingLastPathComponent() // Tests -> Firefly
            .appending(path: relativePath)
        let text = try String(contentsOf: url, encoding: .utf8)
        // Comments stripped BEFORE any search: both files explain this
        // fix in prose that quotes the very call sites below, and a
        // guard that matched its own explanation would read the wrong
        // thing — same rule `MapBackgroundLifecycleGuardTests` follows.
        return text.split(separator: "\n", omittingEmptySubsequences: false)
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.hasPrefix("//") && !$0.hasPrefix("///") }
    }

    func testFindScreenDoesNotDriveTheLifecycleFromItsOwnMountEvents() throws {
        let lines = try source("Sources/Find/FindScreen.swift")
        let offenders = lines.filter {
            $0.contains("FindLifecycle.") && ($0.contains(".onAppear") || $0.contains(".onDisappear"))
        }
        XCTAssertTrue(offenders.isEmpty,
                      "FindScreen must not start/stop the segments' view models from its own mount events: "
                      + "it is a RootView.detail(for:) destination, and that detail column remounts at launch "
                      + "with the first instance's .onDisappear arriving AFTER the second's .onAppear "
                      + "(measured, macOS, Xcode 26.6) — which leaves BOTH view models stopped on the screen "
                      + "the app just landed on. Drive it from RootView.applyFindLifecycle() instead. "
                      + "Found: \(offenders)")
    }

    /// And the other half, so the guard above cannot be satisfied by
    /// simply deleting the rule: `RootView` must actually drive it,
    /// from both pieces of state that define which segment is visible.
    func testRootViewDrivesTheLifecycleFromSelectionAndSegment() throws {
        let lines = try source("Sources/RootView.swift")
        XCTAssertTrue(lines.contains { $0.hasPrefix(".onChange(of: selection") && $0.contains("initial: true")
                                        && $0.contains("applyFindLifecycle") },
                      "RootView must apply the rule on every `selection` change AND on the first evaluation "
                      + "(`initial: true`) — otherwise a launch that lands straight on Find never starts the "
                      + "segment's pump at all")
        XCTAssertTrue(lines.contains { $0.hasPrefix(".onChange(of: findSegment") && $0.contains("applyFindLifecycle") },
                      "and on every segment change, which is the other half of \"only the visible segment's "
                      + "view model observes\"")
        XCTAssertTrue(lines.contains { $0.contains("FindLifecycle.apply(segment: findSegment") },
                      "the Find case must start the VISIBLE segment's view model")
        XCTAssertTrue(lines.contains { $0.contains("FindLifecycle.stopAll(") },
                      "and every other destination must stop both — leaving Find is what stops the pumps now")
    }
}
