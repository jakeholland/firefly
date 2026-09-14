//
//  NotificationTapRouterTests.swift — regression test for the build-328
//  TestFlight SIGABRT (main 6af04e1, Jake tapping a notification).
//
//  What crashed: UIKit's `_performBlockAfterCATransactionCommitSynchronizes:`
//  (called from its own state-restoration snapshot bookkeeping) asserted
//  main-thread and aborted, triggered from a closure inside
//  `NotificationTapRouter.userNotificationCenter(_:didReceive:)`, off
//  Thread 6.
//
//  Root cause: the delegate methods were the Swift `async` spelling of
//  `UNUserNotificationCenterDelegate`'s requirements, which are
//  NONISOLATED in the SDK. UIKit still calls the underlying ObjC
//  selector through a completion handler (`async` is sugar the compiler
//  bridges), via a synthesized thunk shaped like
//  `Task { await self.didReceive(...); completionHandler() }`. With no
//  actor for that unstructured `Task` to inherit, it ran on the
//  concurrent executor — so `completionHandler()`, and the UIKit
//  bookkeeping it triggers synchronously, fired off the main thread.
//
//  Fix (`NotificationTapRouter.swift`'s own top comment has the full
//  reasoning, including why `@MainActor` on the class alone does NOT
//  compile under Swift 6 strict concurrency here): implement the
//  completion-handler form directly and route the call through
//  `NotificationTapRouter.runOnMainActor(_:)`, which always hops to
//  `Task { @MainActor in … }` before running anything.
//
//  Why this test drives `runOnMainActor` directly rather than the
//  delegate methods themselves: neither `UNNotification` nor
//  `UNNotificationResponse` has a public initializer — the SDK marks
//  `init()` `NS_UNAVAILABLE` on both (confirmed against the SDK headers;
//  subclassing doesn't route around it either, since `super.init()`
//  hits the same unavailable-init error) — so no test can construct one
//  to call `willPresent`/`didReceive` directly. `runOnMainActor` is the
//  ONE place both methods hop to the main actor, so testing it directly
//  covers the exact mechanism the fix depends on.
//
import XCTest

final class NotificationTapRouterTests: XCTestCase {
    /// Calls `runOnMainActor` from a `Task.detached` — deliberately, so
    /// there is no actor to inherit, the same shape as the
    /// compiler-synthesized `Task { … }` a nonisolated ObjC completion-
    /// handler thunk spawns — and checks the work closure actually ran
    /// on the main thread.
    func testRunOnMainActorAlwaysRunsWorkOnTheMainThreadEvenWhenCalledFromABackgroundTask() async {
        let ranOnMainThread = await withCheckedContinuation { (continuation: CheckedContinuation<Bool, Never>) in
            Task.detached {
                NotificationTapRouter.runOnMainActor {
                    continuation.resume(returning: Thread.isMainThread)
                }
            }
        }

        XCTAssertTrue(ranOnMainThread,
                       "NotificationTapRouter.runOnMainActor must always hop to the main actor before " +
                       "running its work — losing that is what caused build 328's SIGABRT (UIKit " +
                       "asserted main-thread inside its own state-restoration bookkeeping after the " +
                       "delegate's completion handler was called off-main).")
    }

    /// Same check, called from a `Task { @MainActor in … }` this time —
    /// i.e. already on the main actor — to pin that `runOnMainActor`
    /// staying correct for the background case didn't quietly break the
    /// (far more common in practice) case where UIKit happens to call
    /// the delegate on the main thread already.
    func testRunOnMainActorStillRunsWorkOnTheMainThreadWhenAlreadyThere() async {
        let ranOnMainThread = await withCheckedContinuation { (continuation: CheckedContinuation<Bool, Never>) in
            Task { @MainActor in
                NotificationTapRouter.runOnMainActor {
                    continuation.resume(returning: Thread.isMainThread)
                }
            }
        }

        XCTAssertTrue(ranOnMainThread)
    }
}

//
//  ── Structural guards (PR #318 review) ──────────────────────────────
//
//  The two tests above prove `runOnMainActor` hops to the main actor.
//  They do NOT prove either delegate method uses it — and that gap is
//  the whole bug. Measured, not reasoned: with the fix's own
//  `runOnMainActor` left exactly as shipped, hoisting
//  `completionHandler()` out of the hop in BOTH delegate methods (the
//  precise shape of build 328) compiles clean under Swift 6 strict
//  concurrency and passes all 180 `FireflyAppTests`. That is this
//  repo's named house failure mode — `docs/review/code-review.md`
//  item 6, the proxy check: the test satisfies the proxy ("the helper
//  hops") while the property ("the completion handler is called on the
//  main actor") is free to break.
//
//  Closing it behaviourally is not available: `UNNotification`/
//  `UNNotificationResponse` have no public initializer (the file's
//  header has the detail), so no test can call the delegate methods.
//  So these are SOURCE-level invariant guards, the same genre as
//  `MapBackgroundLifecycleGuardTests`/`FindLifecycleWiringGuardTests`.
//  Weaker than a behavioural test, and not pretending otherwise — but
//  they do pin the exact edit that shipped the crash.
//
extension NotificationTapRouterTests {

    private func source(_ relativePath: String) throws -> String {
        let url = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent() // NotificationTapRouterTests.swift -> Tests
            .deletingLastPathComponent() // Tests -> Firefly
            .appending(path: relativePath)
        return try String(contentsOf: url, encoding: .utf8)
    }

    /// Comments are stripped BEFORE any search: this file's subject is
    /// documented in prose that quotes the very calls being matched, and
    /// a guard that matched its own explanation would read the wrong
    /// block. (Brace counting below is therefore over code only; there
    /// are no string literals in either method body to confuse it.)
    private func codeLines(_ source: String) -> [String] {
        source.split(separator: "\n", omittingEmptySubsequences: false)
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.hasPrefix("//") }
    }

    /// The code-only body of the `userNotificationCenter(…)` overload
    /// whose declaration carries `label` (`willPresent`/`didReceive`),
    /// excluding the declaration itself — so the `completionHandler:`
    /// PARAMETER can never be mistaken for a `completionHandler(` CALL.
    private func delegateBody(label: String, in source: String) throws -> String {
        let lines = codeLines(source)
        var index = 0
        while index < lines.count {
            defer { index += 1 }
            guard lines[index].hasPrefix("func userNotificationCenter(") else { continue }
            var declaration = ""
            var cursor = index
            while cursor < lines.count, !lines[cursor].hasSuffix("{") {
                declaration += lines[cursor]
                cursor += 1
            }
            guard cursor < lines.count else { break }
            declaration += lines[cursor]
            guard declaration.contains(label) else { continue }
            return Self.balancedBlock(after: lines[(cursor + 1)...].joined(separator: "\n"))
        }
        XCTFail("NotificationTapRouter must implement the completion-handler form of `\(label)` — "
                + "the async spelling is what let UIKit's bridging thunk call the completion handler "
                + "off the main thread and abort build 328")
        return ""
    }

    /// Everything up to the `}` that closes the block we are already
    /// inside of (depth starts at 1: the caller has consumed the `{`).
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

    /// Every `completionHandler(` call in `body` must sit inside the
    /// `Self.runOnMainActor {` block — and there must be at least one,
    /// so "never call it" (the OTHER documented way to lose a
    /// notification) cannot satisfy this guard either.
    private func assertCompletionHandlerIsCalledOnlyOnTheMainActor(_ body: String,
                                                                   _ method: String,
                                                                   file: StaticString = #filePath,
                                                                   line: UInt = #line) throws {
        let hop = try XCTUnwrap(body.range(of: "Self.runOnMainActor {"),
                                "\(method) must hop to the main actor through `runOnMainActor` — that "
                                + "single hop is the entire build-328 fix",
                                file: file, line: line)
        let inside = Self.balancedBlock(after: String(body[hop.upperBound...]))

        var calls = 0
        var cursor = body.startIndex
        while let call = body.range(of: "completionHandler(", range: cursor..<body.endIndex) {
            calls += 1
            cursor = call.upperBound
        }
        XCTAssertGreaterThan(calls, 0,
                             "\(method) must actually CALL its completion handler: a delegate that "
                             + "implements the method and never completes it swallows the notification "
                             + "just as thoroughly as not implementing it at all",
                             file: file, line: line)

        var insideCalls = 0
        var insideCursor = inside.startIndex
        while let call = inside.range(of: "completionHandler(", range: insideCursor..<inside.endIndex) {
            insideCalls += 1
            insideCursor = call.upperBound
        }
        XCTAssertEqual(insideCalls, calls,
                       "every `completionHandler(…)` call in \(method) must be INSIDE "
                       + "`Self.runOnMainActor { … }`. Calling it outside is exactly what crashed "
                       + "TestFlight build 328: UIKit does state-restoration snapshot bookkeeping "
                       + "synchronously off that call and asserts main-thread. Found \(calls) call(s), "
                       + "\(insideCalls) of them on the main actor.",
                       file: file, line: line)
    }

    func testWillPresentCallsItsCompletionHandlerOnlyOnTheMainActor() throws {
        let source = try source("Sources/NotificationTapRouter.swift")
        try assertCompletionHandlerIsCalledOnlyOnTheMainActor(
            try delegateBody(label: "willPresent", in: source), "willPresent")
    }

    func testDidReceiveCallsItsCompletionHandlerOnlyOnTheMainActor() throws {
        let source = try source("Sources/NotificationTapRouter.swift")
        try assertCompletionHandlerIsCalledOnlyOnTheMainActor(
            try delegateBody(label: "didReceive", in: source), "didReceive")
    }

    /// `willPresent`'s answer is the thing it exists to give. A hop that
    /// completes with `[]` would pass the guard above and still swallow
    /// every foreground notification — A03 §1.10's first trap, exactly.
    func testWillPresentStillAsksForBannerListAndSound() throws {
        let body = try delegateBody(label: "willPresent",
                                    in: try source("Sources/NotificationTapRouter.swift"))
        for option in [".banner", ".list", ".sound"] {
            XCTAssertTrue(body.contains(option),
                          "willPresent must present \(option): the mere act of setting a delegate makes "
                          + "the system treat a missing/empty answer as `UNNotificationPresentationOptionNone`")
        }
    }

    //
    //  ── Cold start ──────────────────────────────────────────────────
    //
    //  `UNUserNotificationCenter.h`, on `didReceiveNotificationResponse`:
    //  "The delegate must be set before the application returns from
    //  application:didFinishLaunchingWithOptions:." A tap that LAUNCHED
    //  the app is delivered right after launch, so a delegate installed
    //  from a view's `.onAppear`/`.task` is installed too late and that
    //  first tap — the one that is always someone's FLARE — routes
    //  nowhere. `FireflyApp.init()` is the earliest hook a SwiftUI
    //  lifecycle app has, and SwiftUI runs it before
    //  `didFinishLaunchingWithOptions`.
    //

    private func fireflyAppInit() throws -> String {
        let lines = codeLines(try source("Sources/FireflyApp.swift"))
        let start = try XCTUnwrap(lines.firstIndex { $0 == "init() {" },
                                  "FireflyApp must have an `init()` — it is the launch path this guards")
        return Self.balancedBlock(after: lines[(start + 1)...].joined(separator: "\n"))
    }

    func testTheNotificationDelegateIsInstalledOnTheColdStartPath() throws {
        let initBody = try fireflyAppInit()
        XCTAssertTrue(initBody.contains("taps.install()"),
                      "`NotificationTapRouter.install()` must be called from `FireflyApp.init()`. It is "
                      + "the earliest hook a SwiftUI lifecycle app has, and the SDK requires the delegate "
                      + "to be set before `didFinishLaunchingWithOptions` returns — a tap that launched "
                      + "the app is delivered right after launch, so late is the same as never.")

        let source = try source("Sources/FireflyApp.swift")
        for tooLate in [".onAppear", ".task"] {
            XCTAssertFalse(codeLines(source).contains { $0.hasPrefix(tooLate) && $0.contains("install()") },
                           "the notification delegate must not be installed from \(tooLate): a view "
                           + "lifecycle hook runs after launch has already delivered the tap")
        }
    }

    func testTheDeepLinkHandlerIsWiredBeforeTheDelegateIsInstalled() throws {
        let initBody = try fireflyAppInit()
        let wire = try XCTUnwrap(initBody.range(of: "taps.onDeepLink ="),
                                 "`FireflyApp.init()` must give the router somewhere to route to")
        let install = try XCTUnwrap(initBody.range(of: "taps.install()"))
        XCTAssertLessThan(wire.lowerBound, install.lowerBound,
                          "`onDeepLink` must be wired BEFORE `install()`: the launch tap can be delivered "
                          + "as soon as the centre has a delegate, and a router with no handler drops it "
                          + "silently — the same nothing-happens symptom as no delegate at all.")
    }

    /// `UNUserNotificationCenter` holds its delegate WEAKLY. A router
    /// built in `init()` and not stored is deallocated on the spot and
    /// every tap routes nowhere — a silent failure with no crash to
    /// find it by.
    func testTheRouterIsRetainedPastInit() throws {
        let initBody = try fireflyAppInit()
        XCTAssertTrue(initBody.contains("_notificationTaps = State(initialValue: taps)"),
                      "the router must be stored in `@State`: the notification centre's delegate "
                      + "reference is weak, so an unstored router dies before the first tap arrives.")
    }
}
