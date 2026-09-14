//
//  BackgroundConnection.swift — A03 §3.10's honest status line, and the
//  little router a tapped notification lands in.
//
//  §3.10's full nine-row table needs `lastInboundAt` and the §3.5 power
//  states, neither of which exists yet — both are S1b/S2. A03 §7.0 is
//  explicit about the cut ("one honest line is worth shipping in S1a;
//  the nine-row table is not"), so what is here is the subset this build
//  can actually OBSERVE: the setting, the link state, whether the link
//  has ever come back on its own and when, and whether Firefly is
//  allowed to alert anyone. Every row below is derived from a real
//  observation or says UNKNOWN; no row is a guess dressed up as a
//  reading, and nothing invents a Bluetooth power state we are not yet
//  tracking.
//
import FireflyMesh
import Foundation
import Observation

/// The status line, as a value. Split from any view so the honesty rule
/// itself is a unit test (`BackgroundConnectionStatusTests`): the word
/// "connected" may appear only when the link is genuinely `.ready` —
/// the same mechanical shape `SignalTierTests` uses to forbid numbers in
/// the signal view.
public struct BackgroundConnectionStatus: Sendable, Equatable {
    /// The one-word state, in the register A02 §6 sets.
    public enum Headline: String, Sendable, Equatable, CaseIterable {
        case on = "On"
        case off = "Off"
        case stopped = "Stopped"
    }

    public let headline: Headline
    public let detail: String

    public init(headline: Headline, detail: String) {
        self.headline = headline
        self.detail = detail
    }

    /// Everything the line is allowed to depend on. All four are
    /// observations; none is inferred.
    public struct Inputs: Sendable, Equatable {
        public var backgroundConnectEnabled: Bool
        public var link: LinkState
        /// When the link last came back WITHOUT anyone tapping CONNECT
        /// (`BLELinkDiagnostics.lastReconnectAt`). `nil` means it has not
        /// happened in this process — which is not the same as "never",
        /// and the wording below is careful about that.
        public var lastReconnectAt: Date?
        public var notifications: NotificationAuthorization
        public var now: Date

        public init(backgroundConnectEnabled: Bool, link: LinkState, lastReconnectAt: Date? = nil,
                    notifications: NotificationAuthorization = .notDetermined, now: Date = Date()) {
            self.backgroundConnectEnabled = backgroundConnectEnabled
            self.link = link
            self.lastReconnectAt = lastReconnectAt
            self.notifications = notifications
            self.now = now
        }
    }

    /// A03 §3.10, cut to what S1a can honestly say. Plain words per A02
    /// §6.3/§6.4 — no dBm, no node ids, no jargon — and ages rendered
    /// with the SHIPPED helper (`PresenceAge`, PR #304), never a second
    /// opinion about what an age sounds like.
    public static func status(_ inputs: Inputs) -> BackgroundConnectionStatus {
        guard inputs.backgroundConnectEnabled else {
            return BackgroundConnectionStatus(
                headline: .off, detail: "Firefly disconnects when you leave the app.")
        }
        // Nothing below may say "connected" unless the link is `.ready`.
        switch inputs.link {
        case .ready:
            if inputs.notifications == .denied || inputs.notifications == .notDetermined {
                return BackgroundConnectionStatus(
                    headline: .on,
                    detail: "Staying connected, but Firefly can't alert you. Turn on notifications.")
            }
            return BackgroundConnectionStatus(
                headline: .on, detail: "Staying connected in your pocket." + reconnectSentence(inputs))
        case .reconnecting:
            return BackgroundConnectionStatus(
                headline: .on, detail: "Lost your puck. Still looking." + reconnectSentence(inputs))
        case .failed:
            return BackgroundConnectionStatus(
                headline: .stopped,
                detail: "Firefly has stopped trying to reach your puck. Open Connect to try again.")
        case .disconnected, .connecting, .handshaking:
            return BackgroundConnectionStatus(
                headline: .on, detail: "Looking for your puck." + reconnectSentence(inputs))
        }
    }

    /// " Last came back on its own 6 min ago." — or nothing at all. An
    /// absent observation gets no sentence rather than a fabricated one;
    /// the Diagnostics row is where UNKNOWN is spelled out.
    ///
    /// "came back", not "reconnected", and that word choice is load
    /// bearing rather than style: A03_AC14 forbids the substring
    /// "connected" on any line whose link is not `.ready`, and
    /// "recon**nected**" contains it. A reader skimming a lock screen
    /// parses substrings, not tense — which is exactly why the criterion
    /// is written mechanically.
    private static func reconnectSentence(_ inputs: Inputs) -> String {
        guard let lastReconnectAt = inputs.lastReconnectAt else { return "" }
        let age = inputs.now.timeIntervalSince(lastReconnectAt)
        return " Last came back on its own \(PresenceAge.ago(age))."
    }
}

/// Where a tapped notification wants to land, held until the view layer
/// can act on it. `@Observable` and `@MainActor` for the same reason
/// every other view-facing type here is: `RootView` observes it.
///
/// It deliberately does NOT navigate anything itself — only `RootView`
/// owns tab selection, and a second opinion about which tab is showing
/// is exactly the class of bug `AppGraph`'s own header comment is about.
@MainActor
@Observable
public final class DeepLinkRouter {
    /// The route waiting to be applied, or `nil`. Set by `handle(_:)`,
    /// cleared by `consume()`.
    public private(set) var pending: NotificationRoute?

    public init() {}

    /// `true` when this URL was ours. `false` — and nothing changes —
    /// for any other `firefly://` host, so A02's crew links pass
    /// straight through to whoever owns them.
    @discardableResult
    public func handle(_ url: URL) -> Bool {
        guard let route = FireflyDeepLink.route(for: url) else { return false }
        pending = route
        return true
    }

    /// Takes the pending route, leaving nothing behind — a route must be
    /// applied exactly once, or a redraw would re-navigate under the
    /// user.
    ///
    /// REVIEW FIX (PR #310): the `defer { pending = nil }` this replaces
    /// wrote to `@Observable` state on EVERY call, including the common
    /// one where there was no route at all. `RootView` calls this from
    /// `.onChange(of:initial: true)`, i.e. during the first view update,
    /// so every launch mutated observed state mid-update for no reason —
    /// the "Modifying state during view update" hazard, next door to a
    /// `.crewOnboardingCover` whose presentation is driven by exactly
    /// that kind of state. Nothing is written now unless something is
    /// actually taken.
    public func consume() -> NotificationRoute? {
        guard let route = pending else { return nil }
        pending = nil
        return route
    }
}
