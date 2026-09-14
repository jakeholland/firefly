//
//  CrewCopy.swift — the presence vocabulary (`docs/specs/
//  A02-crew-join.md`, §6.3), shared by the Start screen's Joined list
//  and the Crew page's People list. One place these strings live, so
//  they can only ever say the same thing in both places.
//
import FireflyCore
import FireflyModel
import Foundation

enum CrewCopy {
    /// §6.3's chip column. `nil` for `.never` — "no chip", not an empty
    /// one.
    static func presenceChip(_ presence: HeardPresence) -> String? {
        switch presence {
        case .heard: return "HERE"
        case .stale: return "QUIET"
        case .lost: return "NO SIGNAL"
        case .never: return nil
        }
    }

    /// §6.3's line column, from an age in milliseconds ("heard just
    /// now" / "heard 40 s ago" / "quiet for 6 min"). `.lost`'s
    /// "not heard since 9:40 pm" needs a wall-clock timestamp instead —
    /// see `presenceLine(_:lastHeard:now:)`.
    static func presenceLine(_ presence: HeardPresence, ageMs: UInt32?) -> String {
        switch presence {
        case .never:
            return "waiting to hear from them"
        case .heard:
            guard let ageMs, ageMs >= 1000 else { return "heard just now" }
            return "heard \(shortAge(ms: ageMs)) ago"
        case .stale:
            guard let ageMs else { return "quiet" }
            return "quiet for \(roughMinutes(ms: ageMs))"
        case .lost:
            return "not heard recently"
        }
    }

    /// The `.lost` case, with an actual wall-clock "since HH:MM" when a
    /// last-heard timestamp is known (§6.3: "not heard since 9:40 pm").
    static func presenceLine(_ presence: HeardPresence, ageMs: UInt32?, lastHeard: Date?) -> String {
        guard presence == .lost, let lastHeard else { return presenceLine(presence, ageMs: ageMs) }
        let formatter = DateFormatter()
        formatter.dateFormat = "h:mm a"
        return "not heard since \(formatter.string(from: lastHeard).lowercased())"
    }

    private static func shortAge(ms: UInt32) -> String {
        let seconds = ms / 1000
        if seconds < 60 { return "\(seconds) s" }
        let minutes = seconds / 60
        if minutes < 60 { return "\(minutes) min" }
        return "\(minutes / 60) h"
    }

    private static func roughMinutes(ms: UInt32) -> String {
        let minutes = max(1, ms / 1000 / 60)
        return "\(minutes) min"
    }

    /// "New crew member" fallback — §4.4: never blank, never a hex id,
    /// on any main-path screen.
    static func displayName(_ name: String?) -> String {
        guard let name, !name.isEmpty else { return "New crew member" }
        return name
    }
}
