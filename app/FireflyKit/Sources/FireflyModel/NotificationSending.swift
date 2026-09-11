//
//  NotificationSending.swift — M2 point (4): local notifications for an
//  inbound FLARE and for a text arriving while the app is backgrounded.
//
//  `UserNotifications` needs no UIKit/AppKit (unlike `HapticSignaling`'s
//  UIImpactFeedbackGenerator), so — unlike that seam — the real
//  implementation lives here in FireflyModel rather than split into an
//  app-target `+UIKit` file. `UNNotificationSending` below is real,
//  working code on both iOS and macOS.
//
//  Permission is requested LAZILY, on first need (the first time this
//  app actually has something to notify about), never at launch — the
//  task's own explicit requirement, and the same "ask when there is a
//  real reason, not up front" discipline `LocationProviding`'s
//  `requestWhenInUseAuthorization()` already follows for GPS.
//
import Foundation
#if canImport(UserNotifications)
import UserNotifications
#endif

public protocol NotificationSending: Sendable {
    /// Posts once for an inbound FLARE the user did not see live (the
    /// app was backgrounded — see `## Behavior`'s takeover rule: "never
    /// shown in the background beyond a local notification").
    func postFlare(senderName: String) async
    /// Posts once for an inbound text message that arrived while the
    /// app was backgrounded.
    func postMessage(senderName: String, preview: String) async
}

/// The honest default: posts nothing. Every unit test that does not
/// care about notifications gets this rather than a real
/// `UNUserNotificationCenter` round-trip.
public final class NoNotificationSending: NotificationSending, Sendable {
    public init() {}
    public func postFlare(senderName: String) async {}
    public func postMessage(senderName: String, preview: String) async {}
}

#if canImport(UserNotifications)
/// The real implementation. Authorization is requested at most once per
/// process, the first time `post(title:body:)` is actually reached —
/// never from `init`, never from app launch. An `actor`, not a class +
/// `NSLock`: `authorizationRequested`'s read-then-set has to stay
/// atomic across the `await` in `post(title:body:)`'s caller sequence,
/// and an actor gets that for free without holding a lock across a
/// suspension point (which Swift's concurrency checker flags even when
/// the lock itself isn't held THROUGH the `await`, only around it).
public actor UNNotificationSending: NotificationSending {
    private var authorizationRequested = false

    public init() {}

    /// `UNUserNotificationCenter.current()` is deliberately NOT fetched
    /// in `init()` (or stored as a property initialized there): outside
    /// a real, signed `.app` bundle — a bare `swift test` xctest binary,
    /// exactly this package's own unit-test target, the same shape
    /// `A01-companion-app.md`'s BLE-hardware-test split documents for
    /// CoreBluetooth — it throws `bundleProxyForCurrentProcess is nil`
    /// and takes down the whole test process. `AppGraph` constructs this
    /// type unconditionally (it is the live graph's default), so every
    /// `AppGraphTests` case that builds a graph but never actually posts
    /// a notification must stay crash-free; only `post(title:body:)`,
    /// reached exclusively from a real inbound FLARE/text, ever touches
    /// the real center.
    private var center: UNUserNotificationCenter { UNUserNotificationCenter.current() }

    public func postFlare(senderName: String) async {
        await post(title: "FLARE", body: "\(senderName) wants you to come find them")
    }

    public func postMessage(senderName: String, preview: String) async {
        await post(title: senderName, body: preview)
    }

    private func requestAuthorizationIfNeeded() async {
        guard !authorizationRequested else { return }
        authorizationRequested = true
        _ = try? await center.requestAuthorization(options: [.alert, .sound])
    }

    private func post(title: String, body: String) async {
        await requestAuthorizationIfNeeded()
        let settings = await center.notificationSettings()
        guard settings.authorizationStatus == .authorized || settings.authorizationStatus == .provisional else {
            return // denied or not yet decided — never a silent crash, never a fabricated delivery
        }
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        content.sound = .default
        // `trigger: nil` — deliver immediately, no repeat, no schedule.
        let request = UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: nil)
        try? await center.add(request)
    }
}
#endif
