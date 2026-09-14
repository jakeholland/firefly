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

    // MARK: - UNUserNotificationCenterDelegate

    func userNotificationCenter(_ center: UNUserNotificationCenter,
                                 willPresent notification: UNNotification) async
        -> UNNotificationPresentationOptions {
        // Explicit, never the default: see this file's own header for
        // why an unimplemented `willPresent` is how a delegate silently
        // eats every foreground notification.
        [.banner, .list, .sound]
    }

    func userNotificationCenter(_ center: UNUserNotificationCenter,
                                 didReceive response: UNNotificationResponse) async {
        let userInfo = response.notification.request.content.userInfo
        guard let link = userInfo[NotificationUserInfoKey.deepLink] as? String,
              let url = URL(string: link) else { return }
        // Both the plain tap (`UNNotificationDefaultActionIdentifier`)
        // and the FLARE category's "Find them" action go to the SAME
        // destination — §3.11.3 says so explicitly, and one destination
        // is one thing to get right.
        let handler = onDeepLink
        await MainActor.run { handler?(url) }
    }
}
#endif
