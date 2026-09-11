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

    init(client: any MeshtasticClientProtocol, now: @escaping () -> Date = Date.init) {
        self.client = client
        self.now = now
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
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                guard let self, !Task.isCancelled else { return }
                self.tickTrigger &+= 1
            }
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

    let heardInLast10MinCount = DiagnosticsViewModel.unknown
    let packetsIn = DiagnosticsViewModel.unknown
    let packetsOut = DiagnosticsViewModel.unknown
    let ackRate = DiagnosticsViewModel.unknown
    let nodeBatteryPercent = DiagnosticsViewModel.unknown
    let nodeVoltage = DiagnosticsViewModel.unknown
    let firmwareVersion = DiagnosticsViewModel.unknown
}
