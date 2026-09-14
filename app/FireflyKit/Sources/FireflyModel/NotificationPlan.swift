//
//  NotificationPlan.swift — A03 §3.11: everything a local notification
//  is, decided as a pure value before `UserNotifications` is touched.
//
//  This type is the seam that makes notification behaviour testable at
//  all (A03_AC10). Before it, `NotificationSending.post(title:body:)`
//  built a `UNMutableNotificationContent` inline with a random
//  identifier, no interruption level, no thread, no category and no
//  deep link — so every one of those decisions was unreachable from
//  `swift test`, and four of them were simply absent (audit 2.3.12–
//  2.3.16). Now `UNNotificationSending` does nothing but TRANSCRIBE a
//  plan onto the real content object.
//
//  Quiet hours (§3.11.4) are deliberately NOT here: A03 §7.0 cuts them
//  from the before-festival slice. The shape below has the seam for
//  them — `NotificationPlan.plan(for:)` is already a pure function of
//  the event — but no window, no clock and no branch, because a branch
//  no test can reach is worse than an absent feature.
//
import Foundation

/// The four things worth waking a phone for (§3.11.1). A fifth — "link
/// lost / reconnecting" — is a deliberate cut: a notification about our
/// own plumbing is noise at a festival, and the status line is where
/// that belongs.
public enum NotificationEvent: Sendable, Equatable {
    /// INTERPRETATION, stated rather than buried (§3.11.3 writes this
    /// identifier as `flare-<from>-<startedAtMs>`): a FLARE packet
    /// carries a DURATION, not a start time — `ff_proto`'s flare body is
    /// `duration_s` and nothing else — so there is no start-time field
    /// on the wire to key on, and inventing one from our own receive
    /// clock would make the identifier differ between two deliveries of
    /// the same packet, which is the exact bug the derived identifier
    /// exists to stop. `packetID` is the stable per-packet key the mesh
    /// already gives us (and the one §3.11.3 itself uses for `rally-`
    /// and `msg-`), so a retransmit REPLACES and a genuinely new flare
    /// from the same person does not.
    case flare(from: UInt32, senderName: String?, packetID: UInt32)
    /// `text` is the already-composed rally line ("MY SPOT — 210 m NE of
    /// you", or just the name when either fix is missing) — composed by
    /// `AppGraph.formatRallyText`, never re-derived here, so the feed
    /// row and the notification can never disagree about a distance.
    case rally(from: UInt32, senderName: String?, packetID: UInt32, text: String)
    /// A text addressed to us specifically.
    case directMessage(from: UInt32, senderName: String?, packetID: UInt32, text: String)
    /// A text broadcast on the crew channel.
    case crewMessage(from: UInt32, senderName: String?, packetID: UInt32, text: String)
}

/// §3.11.1's middle column. A local mirror of
/// `UNNotificationInterruptionLevel` so this file — and every test over
/// it — needs no `UserNotifications` import, and so macOS/Linux builds
/// of `FireflyModel` compile identically.
///
/// `.critical` is absent on purpose: it needs a special Apple
/// entitlement, overrides the hardware mute switch, and Meshtastic-Apple
/// ships that entitlement with the code path unreachable while promising
/// users it works (§8). A promise no code keeps is worse than the
/// missing feature.
public enum NotificationInterruption: String, Sendable, Equatable, CaseIterable {
    /// "adds the notification to the notification list without lighting
    /// up the screen or playing a sound."
    case passive
    /// The system default: "presents the notification immediately,
    /// lights up the screen, and can play a sound."
    case active
    /// "breaks through system notification controls" — Focus and
    /// Notification Summary. Requires
    /// `com.apple.developer.usernotifications.time-sensitive` in the
    /// entitlements AND the matching capability on the App ID; **if the
    /// entitlement is not granted the level silently degrades to
    /// `.active`**, which is why nothing in this app claims otherwise.
    case timeSensitive
}

/// The category identifiers registered at launch (§1.10 — categories
/// must be registered with `setNotificationCategories`, on EVERY launch
/// including background relaunches).
public enum NotificationCategory {
    public static let flare = "FLARE"
    public static let rally = "RALLY"
    public static let message = "MESSAGE"
    /// The FLARE category's one action. Action identifiers must be
    /// unique across ALL categories (§1.10), which is why this is
    /// namespaced rather than a bare "FIND".
    public static let findThemAction = "FLARE.FIND_THEM"
    public static let findThemTitle = "Find them"
}

/// The key `userInfo` carries the deep link under. Read straight back
/// out by our own delegate — this is an in-process routing token, not a
/// registered URL scheme (§3.11.3), so nothing here depends on
/// `CFBundleURLTypes` or on A02's own `firefly://` crew links.
public enum NotificationUserInfoKey {
    public static let deepLink = "firefly.deepLink"
}

/// Everything the poster needs, and nothing it has to decide for itself.
public struct NotificationPlan: Sendable, Equatable {
    /// DERIVED, never random (§3.11.3): a repeat of the same packet
    /// replaces the existing notification instead of adding a second
    /// one, because a matching identifier "alerts the user again,
    /// replaces the old notification with the new one" (§1.10).
    public let identifier: String
    public let title: String
    public let body: String
    /// Groups a conversation into one stack instead of forty banners.
    public let threadIdentifier: String
    public let categoryIdentifier: String
    public let interruptionLevel: NotificationInterruption
    public let playsSound: Bool
    /// `firefly://thread/crew`, `firefly://thread/dm/<nodeNum>`,
    /// `firefly://find/<nodeNum>`.
    public let deepLink: String

    public init(identifier: String, title: String, body: String, threadIdentifier: String,
                categoryIdentifier: String, interruptionLevel: NotificationInterruption,
                playsSound: Bool, deepLink: String) {
        self.identifier = identifier
        self.title = title
        self.body = body
        self.threadIdentifier = threadIdentifier
        self.categoryIdentifier = categoryIdentifier
        self.interruptionLevel = interruptionLevel
        self.playsSound = playsSound
        self.deepLink = deepLink
    }

    /// The honest fallback for a crew member whose name we do not have
    /// yet — the existing one (`AppGraph+M2Protocol.handleInboundFlare`)
    /// rather than a second spelling of the same idea.
    public static let unknownSender = "Someone"

    /// §3.11.1 (level/sound/thread/category), §3.11.2 (wording) and
    /// §3.11.3 (identifier, deep link), in one pure function. A03_AC10.
    public static func plan(for event: NotificationEvent) -> NotificationPlan {
        switch event {
        case .flare(let from, let senderName, let packetID):
            let name = displayName(senderName)
            let known = isKnown(senderName)
            return NotificationPlan(
                identifier: "flare-\(from)-\(packetID)",
                title: known ? "\(name) needs you" : "Someone needs you",
                body: known ? "They sent a flare. Tap to find them."
                            : "A flare came in from your crew. Tap to find them.",
                threadIdentifier: "flare",
                categoryIdentifier: NotificationCategory.flare,
                // The one message in this product whose entire purpose is
                // to interrupt. Anything less is silenced by Sleep Focus.
                interruptionLevel: .timeSensitive,
                playsSound: true,
                deepLink: "\(scheme)://find/\(from)")
        case .rally(let from, let senderName, let packetID, let text):
            let name = displayName(senderName)
            let known = isKnown(senderName)
            return NotificationPlan(
                identifier: "rally-\(from)-\(packetID)",
                title: known ? "\(name) set a meeting spot" : "Someone set a meeting spot",
                body: text,
                threadIdentifier: "rally",
                categoryIdentifier: NotificationCategory.rally,
                // §9 Q1 asks the owner whether "meet here" should break a
                // Focus too. The spec's own answer is `.active` —
                // directional, not an emergency — and that is what ships
                // until the owner says otherwise.
                interruptionLevel: .active,
                playsSound: true,
                deepLink: "\(scheme)://thread/dm/\(from)")
        case .directMessage(let from, let senderName, let packetID, let text):
            return NotificationPlan(
                identifier: "msg-\(from)-\(packetID)",
                title: displayName(senderName),
                body: text,
                threadIdentifier: "dm-\(from)",
                categoryIdentifier: NotificationCategory.message,
                interruptionLevel: .active,
                playsSound: true,
                deepLink: "\(scheme)://thread/dm/\(from)")
        case .crewMessage(let from, let senderName, let packetID, let text):
            return NotificationPlan(
                identifier: "msg-\(from)-\(packetID)",
                title: "\(displayName(senderName)) \u{00B7} crew",
                body: text,
                threadIdentifier: "crew",
                categoryIdentifier: NotificationCategory.message,
                // §3.11.1's own default OUTSIDE quiet hours. §9 Q3 asks
                // the owner whether a crew broadcast should be `.passive`
                // ALWAYS — "a busy channel during a set buzzes just as
                // much at 9 pm" — with DMs and FLARE/RALLY carrying the
                // alerting. Quiet hours are cut from this slice (§7.0),
                // so until that question is answered a crew message
                // alerts at the spec's stated default. Flipping it is
                // this one line plus `playsSound`.
                interruptionLevel: .active,
                playsSound: true,
                deepLink: "\(scheme)://thread/crew")
        }
    }

    /// The URL scheme the routing tokens use. Shared with A02's crew
    /// links by coincidence of scheme only — the hosts are disjoint, and
    /// `FireflyDeepLink.route(for:)` returns nil for anything it does not
    /// own, so the two can never eat each other's links.
    static let scheme = "firefly"

    private static func isKnown(_ name: String?) -> Bool {
        guard let name else { return false }
        return !name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    private static func displayName(_ name: String?) -> String {
        isKnown(name) ? name! : unknownSender
    }
}

/// Where a tapped notification (or a `firefly://` URL) should land
/// (§3.11.3). A pure translation — the routing itself is the app
/// target's job, because only it owns tab selection.
public enum NotificationRoute: Sendable, Equatable {
    /// A conversation thread: `.crew`, or a member's 1:1.
    case thread(ConversationKind)
    /// Find ▸ Radar, with this member selected when we know who.
    case find(nodeID: UInt32?)
}

public enum FireflyDeepLink {
    /// `nil` for anything this app does not own — including any OTHER
    /// `firefly://` host (A02 §1.8 registers the same scheme for
    /// shareable crew links), so adding a host here is the only way to
    /// claim one.
    public static func route(for url: URL) -> NotificationRoute? {
        guard url.scheme?.lowercased() == NotificationPlan.scheme else { return nil }
        // `firefly://find/123` parses as host "find", path "/123".
        let segments = url.path.split(separator: "/").map(String.init)
        switch url.host()?.lowercased() {
        case "find":
            return .find(nodeID: segments.first.flatMap(UInt32.init))
        case "thread":
            guard let first = segments.first?.lowercased() else { return nil }
            if first == "crew" { return .thread(.crew) }
            guard first == "dm", segments.count >= 2, let nodeID = UInt32(segments[1]) else { return nil }
            return .thread(.member(nodeID))
        default:
            return nil
        }
    }
}
