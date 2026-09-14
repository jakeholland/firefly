//
//  NotificationTapRouter.swift — A03 §3.11.3: what happens when someone
//  taps a Firefly notification.
//
//  Two jobs, and the second one is a trap worth naming.
//
//  1. `didReceive` — read the deep link back out of `userInfo` (an
//     in-process routing token our own plan put there, not a registered
//     URL scheme) and hand it to `DeepLinkRouter`, which `RootView`
//     observes. A tap on a FLARE lands on Find ▸ Radar with that person
//     selected; a tap on a message opens their thread.
//
//  2. `willPresent` — MUST be implemented, and must call its completion
//     handler. A03 §1.10, quoting Apple: "If your delegate does not
//     implement this method, the system behaves as if you had passed the
//     `UNNotificationPresentationOptionNone` option", while "if you do
//     not provide a delegate at all … the system uses the notification's
//     original options." So the mere act of setting a delegate — which
//     job (1) requires — silently SWALLOWS every foreground
//     notification unless this method is present. Implementing it and
//     never calling the completion handler is a second, separate way to
//     lose one.
//
//  This lives in the app target rather than `FireflyModel` because it is
//  the app's own launch-time wiring, and because `AppGraph` must stay
//  buildable (and testable) without a notification centre at all.
//
import FireflyModel
import Foundation
#if canImport(UserNotifications)
import UserNotifications
#endif

#if canImport(UserNotifications)
/// Build-328 SIGABRT fix (TestFlight, main 6af04e1) — read this before
/// touching either delegate method below.
///
/// `didReceive`/`willPresent` used to be the Swift `async` spelling of
/// these requirements. UIKit still calls the underlying ObjC selector
/// through a completion handler (`async` is sugar the compiler bridges
/// for you), via a synthesized thunk shaped like
/// `Task { await self.didReceive(...); completionHandler() }`. That
/// `didReceive`/`willPresent` were themselves nonisolated — nothing in
/// this file pinned them to an actor — meant the unstructured `Task`
/// the thunk spawns had nothing to inherit, so it ran on the concurrent
/// executor. `completionHandler()`, and whatever UIKit does
/// synchronously off the back of it (state-restoration snapshot
/// bookkeeping, `_updateStateRestorationArchiveForBackgroundEvent…`),
/// fired from a background thread. UIKit asserts main-thread there and
/// aborts.
///
/// The fix: implement the completion-handler form directly and take
/// explicit control of when the handler is called —
/// `Task { @MainActor in … ; completionHandler() }`, every time — rather
/// than trust the compiler's ObjC bridging thunk to do it for us. Two
/// details make this compile clean under Swift 6 strict concurrency,
/// both load-bearing:
///
///  - The completion handler parameters below are typed
///    `@escaping @Sendable`. What that buys is narrower than an
///    earlier draft of this comment claimed, and the difference is
///    worth having straight (PR #318 review, all three measured on
///    Xcode 26.6 / Swift 6.3.3 against the macOS 26.5 SDK, not
///    reasoned):
///     * Dropping `@Sendable` while still handing the closure to
///       `runOnMainActor` is a hard COMPILE ERROR, not a warning:
///       "sending 'completionHandler' risks causing data races". That
///       is the only reason the annotation has to be here.
///     * It is NOT part of ObjC requirement matching. Dropping it
///       (and calling the handler inline, so nothing is sent across
///       isolation) compiles clean with no "nearly matches" warning,
///       and the emitted object file still carries the right selector.
///       So `@Sendable` is not what keeps UIKit calling us.
///     * What DOES keep UIKit calling us is the selector itself —
///       `userNotificationCenter:willPresentNotification:withCompletion
///       Handler:` and `…:didReceiveNotificationResponse:withCompletion
///       Handler:`. Both requirements are `optional`, so a signature
///       that only nearly matches would compile and simply never be
///       called. Verify by name after touching either declaration:
///       `strings <NotificationTapRouter.o> | grep userNotificationCenter`
///       must list both selectors.
///  - `runOnMainActor(_:)` below is the ONE place that hop happens, so
///    a plain `@MainActor () -> Void` (non-`@Sendable`) work closure is
///    fine there: the captures inside it (`completionHandler` itself,
///    already `@Sendable`; `onDeepLink`, typed `@MainActor` so it's
///    only ever called on the actor it's isolated to) are each
///    independently safe to send into `Task { @MainActor in … }`.
final class NotificationTapRouter: NSObject, UNUserNotificationCenterDelegate {
    /// Set by `FireflyApp.init` once the graph exists. A closure, not a
    /// graph reference, so this type knows nothing about the composition
    /// root it is wired into.
    var onDeepLink: (@MainActor (URL) -> Void)?

    /// `UNUserNotificationCenter.current()` traps outside a real `.app`
    /// bundle (`UNNotificationSending`'s own doc comment has the full
    /// story) — the same guard, for the same reason, since
    /// `FireflyAppTests` runs inside the host app but other tooling may
    /// not.
    private static var hasAppBundle: Bool { Bundle.main.bundleURL.pathExtension == "app" }

    /// Installs this object as the notification centre's delegate.
    /// Called from `FireflyApp.init()`, which is the earliest hook a
    /// SwiftUI app without a `UIApplicationDelegateAdaptor` has — and a
    /// tap that LAUNCHED the app is delivered right after launch, so
    /// late is the same as never. (A03 §3.1 adds that delegate in S1b
    /// for state restoration; when it lands, this registration moves
    /// into `didFinishLaunchingWithOptions` alongside it.)
    func install() {
        guard Self.hasAppBundle else { return }
        UNUserNotificationCenter.current().delegate = self
    }

    /// The one place either delegate method below hops to the main
    /// actor and stays there. Factored out — rather than each method
    /// writing its own `Task { @MainActor in … }` — for a testability
    /// reason as much as a DRY one: `UNNotification`/
    /// `UNNotificationResponse` both mark their `init()` `NS_UNAVAILABLE`
    /// in the SDK (confirmed against the SDK headers — no public
    /// initializer exists, and subclassing doesn't route around it
    /// either, since `super.init()` hits the same unavailable-init
    /// error), so no unit test can construct one to call
    /// `userNotificationCenter(_:willPresent:…)`/`(_:didReceive:…)`
    /// directly. Routing BOTH through this single, SDK-object-free
    /// function means `NotificationTapRouterTests` can drive the actual
    /// mechanism the build-328 fix depends on — calling it from a
    /// background task and checking `work` really runs on the main
    /// thread — without needing either of those unconstructable types.
    static func runOnMainActor(_ work: @escaping @MainActor () -> Void) {
        Task { @MainActor in work() }
    }

    // MARK: - UNUserNotificationCenterDelegate

    // MUST be implemented, and must call its completion handler. A03
    // §1.10, quoting Apple: "If your delegate does not implement this
    // method, the system behaves as if you had passed the
    // `UNNotificationPresentationOptionNone` option", while "if you do
    // not provide a delegate at all … the system uses the notification's
    // original options." So the mere act of setting a delegate — which
    // `didReceive` below requires — silently SWALLOWS every foreground
    // notification unless this method is present. Implementing it and
    // never calling the completion handler is a second, separate way to
    // lose one — see this file's top comment for why that call goes
    // through `runOnMainActor` rather than being made directly.
    func userNotificationCenter(_ center: UNUserNotificationCenter,
                                 willPresent notification: UNNotification,
                                 withCompletionHandler completionHandler: @escaping @Sendable (UNNotificationPresentationOptions) -> Void) {
        Self.runOnMainActor {
            // Explicit, never the default: see the comment above for why
            // leaving this unimplemented is how a delegate silently eats
            // every foreground notification.
            completionHandler([.banner, .list, .sound])
        }
    }

    // Read the deep link back out of `userInfo` (an in-process routing
    // token our own plan put there, not a registered URL scheme) and
    // hand it to `DeepLinkRouter`, which `RootView` observes. A tap on a
    // FLARE lands on Find ▸ Radar with that person selected; a tap on a
    // message opens their thread.
    func userNotificationCenter(_ center: UNUserNotificationCenter,
                                 didReceive response: UNNotificationResponse,
                                 withCompletionHandler completionHandler: @escaping @Sendable () -> Void) {
        // Extracted to a plain `String?` BEFORE handing off to
        // `runOnMainActor`: pulling it out here, in this (nonisolated)
        // call, means the `@MainActor` work closure below only ever
        // captures Sendable-safe values (a `String?` and the already-
        // `@Sendable` `completionHandler`), never `UNNotificationResponse`
        // itself.
        let link = response.notification.request.content.userInfo[NotificationUserInfoKey.deepLink] as? String
        let handler = onDeepLink
        Self.runOnMainActor {
            defer { completionHandler() }
            guard let link, let url = URL(string: link) else { return }
            // Both the plain tap (`UNNotificationDefaultActionIdentifier`)
            // and the FLARE category's "Find them" action go to the SAME
            // destination — §3.11.3 says so explicitly, and one
            // destination is one thing to get right.
            handler?(url)
        }
    }
}
#endif
