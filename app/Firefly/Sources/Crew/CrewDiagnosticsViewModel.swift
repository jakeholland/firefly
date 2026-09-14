//
//  CrewDiagnosticsViewModel.swift — A02 slice E, task scope item 3:
//  Crew -> Advanced -> "Crew diagnostics" row (`docs/specs/
//  A02-crew-join.md` §6.5). Plain labels, numbers only where they help
//  (task brief) — never the raw `CrewChannelStatus`/`CrewAdmissionCounters`
//  types rendered directly, and UNKNOWN is never rendered as a
//  fabricated 0 (this module's own honest-data rule, `CrewHeardListProviding
//  .swift`'s header comment).
//
import FireflyModel
import Foundation
import Observation

@MainActor
@Observable
final class CrewDiagnosticsViewModel {
    /// `CrewMembershipEngine` in every real composition; a small fake
    /// in tests — same shape `CrewHeardListViewModel` follows.
    private let source: any CrewDiagnosticsProviding
    private let now: () -> Date

    init(source: any CrewDiagnosticsProviding, now: @escaping () -> Date = Date.init) {
        self.source = source
        self.now = now
    }

    /// `false` only before any Start/Join has ever happened on this
    /// phone (`crewChannel == nil`) — the state every count below
    /// treats as UNKNOWN rather than a real zero, because nothing has
    /// run yet to produce one.
    private var hasCrew: Bool { source.crewChannel != nil }

    /// §4.2/AC14's own resolution, in one plain sentence — never
    /// "index 0" as a fallback for anything.
    var channelLabel: String {
        switch source.channelStatus {
        case .noCrew: return "No crew set"
        case .resolving: return "Not resolved yet"
        case .resolved(let index): return "Channel \(index)"
        case .notOnCrewChannel: return "Your puck isn't on this crew's channel"
        }
    }

    /// "Admitted" — a plain count once a crew exists, "—" (never a
    /// fabricated 0) before one does.
    var admittedLabel: String { hasCrew ? "\(source.admissionCounters.admitted)" : "\u{2014}" }

    /// "Refused" — the same UNKNOWN treatment as `admittedLabel`.
    var refusedLabel: String { hasCrew ? "\(source.admissionCounters.refusedTotal)" : "\u{2014}" }

    /// One line per non-zero refusal reason, plain labels — "numbers
    /// only where they help" (task brief): a reason nothing has ever
    /// hit is left off entirely rather than padded in as "0 wrong
    /// channel", which would bury the reasons that actually fired.
    var refusalBreakdown: [(reason: String, count: Int)] {
        guard hasCrew else { return [] }
        let c = source.admissionCounters
        return [
            ("not decrypted", c.refusedNotDecrypted),
            ("puck's channel not resolved yet", c.refusedChannelNotResolved),
            ("wrong channel", c.refusedWrongChannel),
            ("your own radio", c.refusedSelfOrUnknownRadio),
            ("via MQTT", c.refusedViaMQTT),
            ("wrong app", c.refusedWrongPortnum),
            ("hidden", c.refusedHidden),
            ("crew full", c.refusedRosterFull),
        ].filter { $0.1 > 0 }
    }

    /// "Never" (not "0 s ago", not "—") before the first admission this
    /// session — the one place this row spells UNKNOWN as a word rather
    /// than a dash, because "last admission: —" reads as a fault while
    /// "last admission: Never" reads as the honest, ordinary case for a
    /// crew nobody has joined yet.
    var lastAdmissionLabel: String {
        guard let ms = source.lastAdmissionAtMs else { return "Never" }
        let at = Date(timeIntervalSince1970: TimeInterval(ms) / 1000)
        return PresenceAge.ago(max(0, now().timeIntervalSince(at)))
    }
}
