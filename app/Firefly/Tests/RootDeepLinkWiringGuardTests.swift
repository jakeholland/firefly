//
//  RootDeepLinkWiringGuardTests.swift — SOURCE-level invariant guards
//  for the two notification-tap defects this PR fixes, the same genre
//  as `FindLifecycleWiringGuardTests` and `MapBackgroundLifecycleGuard
//  Tests` next door, and here for the same reason: both properties are
//  about WHEN SwiftUI runs something, which no unit test in this target
//  can drive.
//
//  (a) THE COLD-LAUNCH TAP LOSES ITS TAB. `RootLaunchPlanTests` pins
//      the decision; this pins that `RootView` actually asks it. The
//      pure rule being right while `applyInitialSelection()` kept its
//      own unconditional `selection = .find` is precisely the proxy
//      failure `docs/review/code-review.md` item 6 is about — the
//      decision would pass its tests and the app would still land on
//      the crew welcome.
//
//  (b) THE `deepLinkThread` RESET FIRES DURING A VIEW UPDATE.
//      `InboxContainerView`'s `.onChange(of:initial: true)` wrote
//      `deepLinkThread.wrappedValue = nil` — `RootView`'s own `@State`,
//      reached through a `Binding` — from inside the handler, i.e.
//      during the first view update, on exactly the launch where the
//      value is non-nil. The sibling of the `defer { pending = nil }`
//      PR #310's review removed from `DeepLinkRouter.consume()`. What a
//      test CAN prove is that neither write is in the handler any more.
//
//  Structural guards are weaker than behavioural tests and do not
//  pretend otherwise. The behavioural half of both fixes is
//  `NotificationTapUITests`.
//
import XCTest

final class RootDeepLinkWiringGuardTests: XCTestCase {

    // MARK: - Source reading

    private func source(_ relativePath: String) throws -> [String] {
        let url = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent() // this file -> Tests
            .deletingLastPathComponent() // Tests -> Firefly
            .appending(path: relativePath)
        let text = try String(contentsOf: url, encoding: .utf8)
        // Comments stripped BEFORE any search: both files explain these
        // fixes in prose that quotes the very statements below, and a
        // guard that matched its own explanation would read the wrong
        // thing — the rule `FindLifecycleWiringGuardTests` follows.
        return text.split(separator: "\n", omittingEmptySubsequences: false)
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.hasPrefix("//") }
    }

    /// Everything up to the `}` closing the block we are already inside
    /// (depth starts at 1: the caller has consumed the opening `{`).
    private static func balancedBlock(after text: String) -> String {
        var depth = 1
        var out = ""
        for character in text {
            if character == "{" { depth += 1 }
            if character == "}" {
                depth -= 1
                if depth == 0 { return out }
            }
            out.append(character)
        }
        return out
    }

    /// The code-only body of the first block whose opening line starts
    /// with `signature`, from its `{` to the matching `}` — the
    /// declaration (or the closure's own parameter list) excluded, so a
    /// parameter can never be mistaken for a statement.
    private func block(opening signature: String,
                       in lines: [String],
                       file: StaticString = #filePath,
                       line: UInt = #line) throws -> String {
        guard let index = lines.firstIndex(where: { $0.hasPrefix(signature) }) else {
            XCTFail("no block starting `\(signature)` — this guard's subject moved or was renamed",
                    file: file, line: line)
            return ""
        }
        let text = lines[index...].joined(separator: "\n")
        guard let brace = text.firstIndex(of: "{") else {
            XCTFail("`\(signature)` opens no block", file: file, line: line)
            return ""
        }
        return Self.balancedBlock(after: String(text[text.index(after: brace)...]))
    }

    // MARK: - (a) the cold-launch tap keeps its tab

    func testApplyInitialSelectionAsksRootLaunchPlanRatherThanDecidingItself() throws {
        let body = try block(opening: "private func applyInitialSelection()",
                             in: try source("Sources/RootView.swift"))
        XCTAssertTrue(body.contains("RootLaunchPlan.plan("),
                      "`applyInitialSelection()` must take its landing from `RootLaunchPlan.plan(…)`. "
                      + "Deciding inline is how it came to assign `selection = .find` over a route a "
                      + "notification tap had already applied — `RootLaunchPlanTests` would still pass.")
        XCTAssertTrue(body.contains("appliedDeepLink"),
                      "the plan must be asked with `appliedDeepLink`, not with `deepLinks.pending` "
                      + "alone: by the time this `.task` runs, `consume()` has already emptied "
                      + "`pending`, so a `pending`-only test reports NO deep link on exactly the "
                      + "launch that had one")
    }

    /// The deferring case must assign NOTHING — not the tab, not the
    /// cover, and not the debug `-FireflyStartTab`/`-FireflyFindSegment`
    /// overrides that follow the switch (hence `return`, not `break`).
    func testTheDeferToDeepLinkCaseAssignsNothingAtAll() throws {
        let body = try block(opening: "private func applyInitialSelection()",
                             in: try source("Sources/RootView.swift"))
        let caseRange = try XCTUnwrap(body.range(of: "case .deferToDeepLink:"),
                                      "`applyInitialSelection()` must handle `.deferToDeepLink` by name")
        let rest = String(body[caseRange.upperBound...])
        let nextCase = rest.range(of: "case .")?.lowerBound ?? rest.endIndex
        let statements = String(rest[..<nextCase])
        XCTAssertTrue(statements.contains("return"),
                      "`.deferToDeepLink` must `return`, not `break`: falling through leaves the debug "
                      + "`-FireflyStartTab`/`-FireflyFindSegment` overrides free to retarget a launch "
                      + "a real notification tap already chose")
        XCTAssertFalse(statements.contains("="),
                       "`.deferToDeepLink` must assign nothing. Found: \(statements.trimmingCharacters(in: .whitespacesAndNewlines))")
    }

    func testApplyPendingDeepLinkRecordsThatItRouted() throws {
        let body = try block(opening: "private func applyPendingDeepLink()",
                             in: try source("Sources/RootView.swift"))
        XCTAssertTrue(body.contains("appliedDeepLink = true"),
                      "`applyPendingDeepLink()` must record that it consumed a route — that flag is the "
                      + "only thing `applyInitialSelection()`'s later `.task` can read, since "
                      + "`DeepLinkRouter.consume()` has emptied `pending` by then")
    }

    /// The OTHER ordering, and the one a flag cannot fix. MEASURED on a
    /// fresh simulator by `NotificationTapUITests`' cold case before
    /// this line existed: the notification response arrived after
    /// `applyInitialSelection()`'s `.task` had already raised A02 §6.1's
    /// welcome, so `Screen.Thread` was correct, present, and invisible
    /// underneath the cover. `RootLaunchPlan` was right and the app was
    /// still wrong — which is why this guard is here and not only there.
    func testApplyPendingDeepLinkLowersTheCrewWelcomeItself() throws {
        let body = try block(opening: "private func applyPendingDeepLink()",
                             in: try source("Sources/RootView.swift"))
        XCTAssertTrue(body.contains("showCrewOnboarding = false"),
                      "a route that lands AFTER the cover has gone up must lower it: nothing read before "
                      + "the fact can, and a deep-linked thread behind A02 §6.1's full-screen cover is "
                      + "invisible even though the routing worked")
    }

    /// `applyPendingDeepLink()` writes `selection`, `deepLinkThread` and
    /// `showCrewOnboarding` — all observed, the last one driving a
    /// `.fullScreenCover` — so it must not run from inside the
    /// `initial: true` handler either. Defect (b) with a different
    /// binding; same fix, and worth pinning in both places.
    func testTheDeepLinksPendingHandlerDefersInsteadOfWritingDuringTheUpdate() throws {
        let body = try block(opening: ".onChange(of: deepLinks.pending, initial: true)",
                             in: try source("Sources/RootView.swift"))
        XCTAssertTrue(body.contains("Task { @MainActor in applyPendingDeepLink() }"),
                      "the pending-route handler must hop off the view update before applying anything: "
                      + "`initial: true` fires DURING the first update, on exactly the launch that has a "
                      + "route, and every write `applyPendingDeepLink()` makes is to observed state")
        XCTAssertFalse(body.contains("selection ="),
                       "no assignment may happen in the handler itself")
    }

    // MARK: - (b) the reset is not a view-update mutation

    func testTheDeepLinkThreadHandlerWritesNothingDuringTheViewUpdate() throws {
        let lines = try source("Sources/Inbox/InboxListView.swift")
        let body = try block(opening: ".onChange(of: deepLinkThread.wrappedValue, initial: true)", in: lines)
        for write in ["deepLinkThread.wrappedValue = nil", "activeThread = "] {
            XCTAssertFalse(body.contains(write),
                           "`\(write)` must not run inside the `initial: true` handler: that IS the first "
                           + "view update, on exactly the launch where the value is non-nil, and "
                           + "`deepLinkThread` is `RootView`'s own `@State`. Same \"modifying state during "
                           + "view update\" hazard PR #310's review removed from `DeepLinkRouter.consume()`.")
        }
        XCTAssertTrue(body.contains("Task { @MainActor in"),
                      "the handler must hand the work to a `Task { @MainActor in … }` so both writes land "
                      + "after the update that noticed them, rather than inside it")
        XCTAssertTrue(body.contains("consumeDeepLinkThread()"),
                      "the deferred work must be `consumeDeepLinkThread()` — one named place that both "
                      + "clears the request and opens the thread")
    }

    /// …and the writes still exist somewhere: a handler that quietly
    /// stopped clearing `deepLinkThread` would satisfy the guard above
    /// and re-push the thread on every redraw (this binding's own doc
    /// comment), which is the failure the clear exists to prevent.
    func testConsumeDeepLinkThreadStillClearsTheRequestAndOpensTheThread() throws {
        let body = try block(opening: "private func consumeDeepLinkThread()",
                             in: try source("Sources/Inbox/InboxListView.swift"))
        XCTAssertTrue(body.contains("deepLinkThread.wrappedValue = nil"),
                      "the deep-link request must still be cleared once opened — leaving it set re-pushes "
                      + "the thread on every redraw and traps the user there")
        XCTAssertTrue(body.contains("model.openThread("),
                      "the thread must still be opened through `model.openThread(_:)` and pushed by the "
                      + "SAME `navigationDestination(item:)` a real tap uses")
    }
}
