//
//  DiagnosticsViewModel.swift — the Diagnostics sub-screen: live values
//  read from the node only, never inferred (docs/specs/
//  A01-companion-app.md, Design language > Diagnostics; M1 acceptance:
//  "Diagnostics shows link state, frame counters and firmware version,
//  and shows 'unknown' where it does not know").
//
//  A THIRD independent `linkState()` subscription — the spec calls this
//  out explicitly ("for linkState also Diagnostics") alongside
//  ConnectViewModel's and CoreStore's own, all three backed by the same
//  EventHub multicast (S1) without stealing each other's events.
//
import FireflyMesh
import FireflyModel
import Foundation
import Observation

@MainActor
@Observable
final class DiagnosticsViewModel {
    private(set) var link: LinkState = .disconnected

    private let client: any MeshtasticClientProtocol
    private var observation: Task<Void, Never>?
    /// M2 — "a Diagnostics line for uptime of the current link". The
    /// moment the CURRENT `.ready` streak began; nil whenever `link` is
    /// not `.ready`, and re-stamped fresh on every re-entry into
    /// `.ready` — the power-cycle manual test (app/README.md) is
    /// specifically "does uptime reset to ~0 after the node comes back",
    /// which only reads correctly if this is per-streak, not
    /// per-session.
    private var readySince: Date?
    /// A UI-only, foreground-visible 1s refresh (cancelled in
    /// `stopObserving()`, same as `observation`) so `uptimeLabel` visibly
    /// counts up while this screen is on screen — the same pattern
    /// `AppGraph`'s own 1s ack-timeout tick loop uses for a live value
    /// with no event of its own to arrive on. Not a background timer:
    /// nothing schedules this unless `observe()` has been called, which
    /// only happens while `DiagnosticsScreen` is actually visible.
    private var refreshTask: Task<Void, Never>?
    /// Bumped by `refreshTask` — `@Observable` tracks per-property
    /// reads, and `uptimeLabel` is a computed property with nothing
    /// else in it for SwiftUI to key a redraw off of as real time
    /// passes. Read (and discarded) at the top of `uptimeLabel` so it
    /// registers as that property's one tracked dependency.
    private(set) var tickTrigger = 0
    /// Injectable — same convention as `ConnectViewModel.now`.
    private let now: () -> Date
    /// A03 §3.6 — the transport's own reconnect counters, or `nil` on a
    /// stack with no BLE at all (the stub graph, the iOS Simulator),
    /// which renders as UNKNOWN rather than as zeros. A zero and a
    /// "there is no radio here" are different facts and this screen has
    /// never pretended otherwise.
    private let linkDiagnostics: (any BLELinkDiagnosticsProviding)?
    /// A03 §3.3/§3.10 — read once per refresh, not cached at
    /// construction: the toggle can move while this screen is open.
    private let backgroundConnectEnabled: () -> Bool
    /// A03 §3.11.5 — whether Firefly may actually alert anyone. The
    /// status line must not claim background coverage it does not have.
    private let notifications: (any NotificationSending)?

    private(set) var diagnostics = BLELinkDiagnostics()
    private(set) var notificationAuthorization: NotificationAuthorization = .notDetermined

    init(client: any MeshtasticClientProtocol, now: @escaping () -> Date = Date.init,
         linkDiagnostics: (any BLELinkDiagnosticsProviding)? = nil,
         notifications: (any NotificationSending)? = nil,
         backgroundConnectEnabled: @escaping () -> Bool = { false }) {
        self.client = client
        self.now = now
        self.linkDiagnostics = linkDiagnostics
        self.notifications = notifications
        self.backgroundConnectEnabled = backgroundConnectEnabled
    }

    func observe() {
        guard observation == nil else { return }
        let stream = client.linkState()
        observation = Task { [weak self] in
            for await state in stream {
                guard let self else { return }
                self.apply(state)
            }
        }
        refreshTask = Task { [weak self] in
            while !Task.isCancelled {
                // A03 §3.6/§3.11 — the counters and the notification
                // authorization are pulled on the SAME 1 s cadence the
                // uptime label already redraws on, rather than on a
                // second timer of their own.
                guard let live = self, !Task.isCancelled else { return }
                await live.refreshBackgroundConnectionState()
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                guard let live = self, !Task.isCancelled else { return }
                live.tickTrigger &+= 1
            }
        }
    }

    private func refreshBackgroundConnectionState() async {
        if let linkDiagnostics {
            diagnostics = await linkDiagnostics.linkDiagnostics()
        }
        if let notifications {
            notificationAuthorization = await notifications.authorization()
        }
    }

    func stopObserving() {
        observation?.cancel()
        observation = nil
        refreshTask?.cancel()
        refreshTask = nil
    }

    private func apply(_ state: LinkState) {
        let enteringReady = (state == .ready && link != .ready)
        link = state
        if enteringReady {
            readySince = now()
        } else if state != .ready {
            readySince = nil
        }
    }

    var linkStateLabel: String {
        switch link {
        case .disconnected: return "NOT CONNECTED"
        case .connecting: return "CONNECTING"
        case .handshaking: return "CONNECTING"
        case .ready: return "CONNECTED"
        case .reconnecting(let attempt): return "RECONNECTING (attempt \(attempt))"
        case .failed: return "FAILED"
        }
    }

    /// M2 — the Diagnostics uptime line. `UNKNOWN` (never `0s`) before
    /// the link has ever reached `.ready` or while it is not `.ready`
    /// right now — a stale number surviving a drop would be exactly the
    /// fabricated-freshness failure this screen's own footer line
    /// ("Nothing here is inferred") exists to refuse.
    var uptimeLabel: String {
        _ = tickTrigger
        guard let readySince else { return DiagnosticsViewModel.unknown }
        let seconds = max(0, Int(now().timeIntervalSince(readySince)))
        let hours = seconds / 3600
        let minutes = (seconds % 3600) / 60
        let secs = seconds % 60
        if hours > 0 { return String(format: "%dh %02dm", hours, minutes) }
        if minutes > 0 { return String(format: "%dm %02ds", minutes, secs) }
        return "\(secs)s"
    }

    /// Every field below needs a value `MeshtasticClientProtocol` does
    /// not expose in M1 — no packet counters, no battery/voltage
    /// telemetry, no firmware-version field on the seam at all. UNKNOWN
    /// is the honest rendering the acceptance criterion asks for, not
    /// a placeholder number standing in for a real one.
    static let unknown = "UNKNOWN"

    // MARK: - A03: the "Background connection" rows

    /// A03 §3.10, cut to what this build can observe (§7.0). The line
    /// itself is decided by `BackgroundConnectionStatus` in FireflyModel
    /// — a pure function with its own honesty test — so this screen only
    /// renders it.
    var backgroundConnectionStatus: BackgroundConnectionStatus {
        _ = tickTrigger
        return BackgroundConnectionStatus.status(.init(
            backgroundConnectEnabled: backgroundConnectEnabled(), link: link,
            lastReconnectAt: diagnostics.lastReconnectAt, notifications: notificationAuthorization, now: now()))
    }

    var backgroundConnectionLabel: String {
        let status = backgroundConnectionStatus
        return "\(status.headline.rawValue) \u{00B7} \(status.detail)"
    }

    /// A03 §3.6 — how many rediscovery scan windows this process has
    /// opened. UNKNOWN, not "0", where there is no BLE transport to ask:
    /// "we did not scan" and "there is nothing here that could scan" are
    /// different facts.
    var scanStartsLabel: String {
        _ = tickTrigger
        guard linkDiagnostics != nil else { return Self.unknown }
        return "\(diagnostics.scanStarts)"
    }

    /// A03 §3.4/§3.6 — how many times the link came back with nobody
    /// tapping CONNECT.
    var reconnectsLabel: String {
        _ = tickTrigger
        guard linkDiagnostics != nil else { return Self.unknown }
        return "\(diagnostics.reconnects)"
    }

    /// When the most recent of those happened. UNKNOWN when it has not
    /// happened in this process — never a fabricated date, and never
    /// "never", which would claim more than we know.
    var lastReconnectLabel: String {
        _ = tickTrigger
        guard linkDiagnostics != nil, let at = diagnostics.lastReconnectAt else { return Self.unknown }
        return PresenceAge.ago(now().timeIntervalSince(at))
    }

    /// A03 §3.1 (S1b) — how many CoreBluetooth state RESTORATIONS this
    /// process has adopted: how many times iOS relaunched Firefly into a
    /// session it had kept alive. This is the number P3 (§6) is actually
    /// measuring, and the only way to tell "restoration worked" apart
    /// from "the app was never killed" after the fact.
    ///
    /// UNKNOWN, not "0", where there is no BLE transport to ask — "this
    /// process was not restored" and "there is nothing here that could
    /// be restored" are different facts, same rule the scan/reconnect
    /// counters already follow.
    var restoredSessionsLabel: String {
        _ = tickTrigger
        guard linkDiagnostics != nil else { return Self.unknown }
        return "\(diagnostics.restores)"
    }

    /// When the most recent restore was adopted, and what it restored
    /// INTO (§3.1 branches on the restored peripheral's own
    /// `CBPeripheralState`, so which branch it took is the interesting
    /// half). UNKNOWN until one has actually happened — never a
    /// fabricated date, and never "never".
    var lastRestoreLabel: String {
        _ = tickTrigger
        guard linkDiagnostics != nil, let at = diagnostics.lastRestoreAt else { return Self.unknown }
        let ago = PresenceAge.ago(now().timeIntervalSince(at))
        guard let action = diagnostics.lastRestoreAction else { return ago }
        return "\(ago) \u{00B7} \(Self.restoreWords(action))"
    }

    /// The `BLERestoreAction` cases in the register this screen uses —
    /// plain words for what the session was doing when we got it back,
    /// not an enum case name.
    static func restoreWords(_ action: BLERestoreAction) -> String {
        switch action {
        case .adoptConnected: return "still connected"
        case .keepPendingConnect: return "still connecting"
        case .reconnect: return "had dropped"
        }
    }

    /// A03 §3.11.5 — said plainly, because a user whose notifications
    /// are off has an app that will never tell them about a FLARE.
    var notificationsLabel: String {
        _ = tickTrigger
        switch notificationAuthorization {
        case .authorized: return "Allowed"
        case .provisional: return "Quiet only"
        case .denied: return "Not allowed"
        case .notDetermined: return "Not asked yet"
        }
    }

    /// Whether a given row's value came from something this process
    /// actually observed — the Diagnostics screen colours live values
    /// differently from ones it does not have, and a counter with no
    /// transport behind it is NOT live.
    var hasLinkDiagnostics: Bool { linkDiagnostics != nil }

    let heardInLast10MinCount = DiagnosticsViewModel.unknown
    let packetsIn = DiagnosticsViewModel.unknown
    let packetsOut = DiagnosticsViewModel.unknown
    let ackRate = DiagnosticsViewModel.unknown
    let nodeBatteryPercent = DiagnosticsViewModel.unknown
    let nodeVoltage = DiagnosticsViewModel.unknown
    let firmwareVersion = DiagnosticsViewModel.unknown
}
