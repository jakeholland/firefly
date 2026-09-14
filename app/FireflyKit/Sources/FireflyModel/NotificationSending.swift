//
//  NotificationSending.swift — A03 §3.11: local notifications for the
//  events that arrive while nobody is looking at the screen.
//
//  `UserNotifications` needs no UIKit/AppKit (unlike `HapticSignaling`'s
//  UIImpactFeedbackGenerator), so — unlike that seam — the real
//  implementation lives here in FireflyModel rather than split into an
//  app-target `+UIKit` file. `UNNotificationSending` below is real,
//  working code on both iOS and macOS.
//
//  WHAT CHANGED IN A03 S1a, and why (audit 2.3.11–2.3.16):
//
//  * Authorization is NO LONGER requested from the posting path. It was
//    requested lazily, on first need — and the only callers are the
//    background branches, so the first FLARE of the festival called
//    `requestAuthorization` while backgrounded, iOS could not present
//    the prompt, the status read `.notDetermined`, and the alert was
//    dropped. `requestAuthorization()` is now an explicit, foreground
//    call (`AppGraph`, on the first `.ready` seen while foregrounded)
//    and `post(_:)` only ever CHECKS (§3.11.5).
//  * Content comes from a `NotificationPlan` (that file's own header):
//    interruption level, thread identifier, category, derived
//    identifier and deep link, none of which existed before.
//  * Delivered notifications for a thread are withdrawn when it is read.
//
import Foundation
#if canImport(UserNotifications)
import UserNotifications
#endif

/// What the system says about our permission. A local mirror of
/// `UNAuthorizationStatus` so the seam — and the status line that reads
/// it — needs no `UserNotifications` import.
public enum NotificationAuthorization: String, Sendable, Equatable, CaseIterable {
    case notDetermined
    case denied
    case authorized
    /// Delivered quietly, straight to Notification Centre. Firefly never
    /// REQUESTS this (§3.11.5 — for a FLARE it is worse than asking),
    /// but the user's own Settings can produce it, so it is represented
    /// rather than collapsed into "authorized".
    case provisional
}

public protocol NotificationSending: Sendable {
    /// Posts one notification, exactly as the plan describes it. Never
    /// requests authorization (A03_AC13); never invents an identifier
    /// (A03_AC11).
    func post(_ plan: NotificationPlan) async
    /// Asks for permission. Call this from the FOREGROUND, at a moment
    /// the user can connect to a reason — never from a background
    /// callback, where iOS cannot present the prompt at all.
    @discardableResult
    func requestAuthorization() async -> Bool
    /// What the system currently says. Read by the "Background
    /// connection" status line, which must not claim Firefly can alert
    /// someone when it cannot.
    func authorization() async -> NotificationAuthorization
    /// Registers the §3.11.3 categories and their actions. Idempotent,
    /// and meant to run on EVERY launch (§1.10).
    func registerCategories() async
    /// Withdraws already-DELIVERED notifications for a thread when the
    /// user reads it — the one behaviour of Meshtastic-Apple's
    /// notification manager worth copying wholesale (§3.11.3).
    func withdrawDelivered(threadIdentifier: String) async
}

/// The honest default: posts nothing, asks for nothing, claims nothing.
/// Every unit test that does not care about notifications gets this
/// rather than a real `UNUserNotificationCenter` round-trip.
public final class NoNotificationSending: NotificationSending, Sendable {
    public init() {}
    public func post(_ plan: NotificationPlan) async {}
    @discardableResult
    public func requestAuthorization() async -> Bool { false }
    public func authorization() async -> NotificationAuthorization { .notDetermined }
    public func registerCategories() async {}
    public func withdrawDelivered(threadIdentifier: String) async {}
}

#if canImport(UserNotifications)
/// The real implementation. An `actor`, not a class + `NSLock`: the
/// posted-identifier memory below is read-then-written across an
/// `await`, and an actor gets that atomicity without holding a lock
/// through a suspension point.
public actor UNNotificationSending: NotificationSending {
    /// A03_AC11 / §3.11.3's dedupe, belt AND braces. iOS already
    /// REPLACES a delivered notification whose identifier matches
    /// (§1.10), so a duplicate packet can never produce two banners; this
    /// ring additionally stops us re-posting one at all, which is what
    /// keeps a foreground catch-up (the same packet surfacing again
    /// after a gap) from re-alerting for something already seen. Bounded
    /// — same shape as `PongReplyDedup`/`CoreInboxProvider.PacketIDRing`.
    private var postedOrder: [String] = []
    private var postedIDs: Set<String> = []
    private static let dedupeCapacity = 128

    public init() {}

    /// `UNUserNotificationCenter.current()` is deliberately NOT fetched
    /// in `init()` (or stored as a property initialized there): outside
    /// a real, signed `.app` bundle — a bare `swift test` xctest binary,
    /// exactly this package's own unit-test target — it throws
    /// `bundleProxyForCurrentProcess is nil` and takes down the whole
    /// test process. `AppGraph` constructs this type unconditionally (it
    /// is the live graph's default), so every test that builds a graph
    /// but never posts must stay crash-free.
    private var center: UNUserNotificationCenter { UNUserNotificationCenter.current() }

    /// …and A03 S1a makes that trap reachable from one more place, so
    /// the guard is now explicit rather than implied by "only `post` ever
    /// touches the center". `registerCategories()` is called from
    /// `AppGraph.start()` on EVERY launch (§1.10 requires it), and
    /// `AppGraphTests` builds real graphs by the dozen inside a bare
    /// `xctest` process, where `UNUserNotificationCenter.current()`
    /// throws `bundleProxyForCurrentProcess is nil` and takes the whole
    /// test run down with it.
    ///
    /// Outside an `.app`, every method below is an honest no-op that
    /// reports `.notDetermined` — which is exactly true: a process with
    /// no bundle identity has no notification authorization, and saying
    /// so beats crashing or pretending.
    private static let hasAppBundle: Bool = Bundle.main.bundleURL.pathExtension == "app"

    public func post(_ plan: NotificationPlan) async {
        guard Self.hasAppBundle else { return }
        // §3.11.5 step 3: CHECK, never request. "Always check your app's
        // authorization status before scheduling local notifications."
        let settings = await center.notificationSettings()
        guard settings.authorizationStatus == .authorized || settings.authorizationStatus == .provisional else {
            return // denied or not yet decided — never a silent crash, never a fabricated delivery
        }
        guard !postedIDs.contains(plan.identifier) else { return }
        remember(plan.identifier)

        let content = UNMutableNotificationContent()
        content.title = plan.title
        content.body = plan.body
        content.sound = plan.playsSound ? .default : nil
        content.threadIdentifier = plan.threadIdentifier
        content.categoryIdentifier = plan.categoryIdentifier
        content.interruptionLevel = Self.level(for: plan.interruptionLevel)
        content.userInfo[NotificationUserInfoKey.deepLink] = plan.deepLink
        // `trigger: nil` — deliver immediately, no repeat, no schedule.
        let request = UNNotificationRequest(identifier: plan.identifier, content: content, trigger: nil)
        try? await center.add(request)
    }

    @discardableResult
    public func requestAuthorization() async -> Bool {
        guard Self.hasAppBundle else { return false }
        // `.badge` is new in A03 (§3.11.3); `.provisional` is
        // deliberately NOT requested (§3.11.5) — it delivers quietly to
        // Notification Centre, which for a FLARE is worse than asking.
        // `.timeSensitive` as a `UNAuthorizationOptions` case is
        // deprecated; the level comes from the entitlement plus
        // `content.interruptionLevel` instead (§1.10).
        let granted = (try? await center.requestAuthorization(options: [.alert, .sound, .badge])) ?? false
        return granted
    }

    public func authorization() async -> NotificationAuthorization {
        guard Self.hasAppBundle else { return .notDetermined }
        switch await center.notificationSettings().authorizationStatus {
        case .authorized: return .authorized
        case .provisional: return .provisional
        case .denied: return .denied
        default: return .notDetermined
        }
    }

    public func registerCategories() async {
        guard Self.hasAppBundle else { return }
        // §3.11.3: FLARE gets ONE action, "Find them" (foreground, same
        // destination as the tap). MESSAGE ships with NO actions rather
        // than a Reply that can silently not send — there is no
        // background send path that can honestly report failure yet, and
        // that is the cut, stated.
        let findThem = UNNotificationAction(identifier: NotificationCategory.findThemAction,
                                             title: NotificationCategory.findThemTitle,
                                             options: [.foreground])
        let flare = UNNotificationCategory(identifier: NotificationCategory.flare, actions: [findThem],
                                            intentIdentifiers: [], options: [])
        let rally = UNNotificationCategory(identifier: NotificationCategory.rally, actions: [],
                                            intentIdentifiers: [], options: [])
        let message = UNNotificationCategory(identifier: NotificationCategory.message, actions: [],
                                              intentIdentifiers: [], options: [])
        center.setNotificationCategories([flare, rally, message])
    }

    public func withdrawDelivered(threadIdentifier: String) async {
        guard Self.hasAppBundle else { return }
        let delivered = await center.deliveredNotifications()
        let ids = delivered
            .filter { $0.request.content.threadIdentifier == threadIdentifier }
            .map(\.request.identifier)
        guard !ids.isEmpty else { return }
        center.removeDeliveredNotifications(withIdentifiers: ids)
        // A withdrawn notification may legitimately be posted again —
        // the next message in that thread is a new packet with its own
        // identifier, but a REPEAT of the withdrawn one is now worth
        // showing again rather than being swallowed by the ring.
        for id in ids { forget(id) }
    }

    private static func level(for level: NotificationInterruption) -> UNNotificationInterruptionLevel {
        switch level {
        case .passive: return .passive
        case .active: return .active
        case .timeSensitive: return .timeSensitive
        }
    }

    private func remember(_ identifier: String) {
        postedOrder.append(identifier)
        postedIDs.insert(identifier)
        if postedOrder.count > Self.dedupeCapacity {
            postedIDs.remove(postedOrder.removeFirst())
        }
    }

    private func forget(_ identifier: String) {
        guard postedIDs.remove(identifier) != nil else { return }
        postedOrder.removeAll { $0 == identifier }
    }
}
#endif
