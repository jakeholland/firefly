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
