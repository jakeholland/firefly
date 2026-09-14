//
//  CrewCopy.swift — the presence vocabulary the Start screen's Joined
//  list and the Crew page's People list share.
//
//  This file deliberately defines NO strings of its own. The words
//  shipped in PR #304 (owner decision, 2026-09-13, "Presence/status
//  words everywhere they appear") and live in exactly two places:
//  `PresenceTag.plainLabel(age:)` and `CrewDisplayFallback` — Inbox
//  rows, Crew settings rows, Radar and map pins all render from those,
//  and so do these two screens. This file is only the adapter between
//  `HeardPresence` (the `ff_crew_presence_t` axis `CrewJoinedMember`
//  carries) and `PresenceTag` (the phone's own label enum).
//
//  A02 §6.3's own table ("QUIET", "waiting to hear from them", "NAME?")
//  is the pre-#304 draft and is superseded: those words were replaced
//  before this slice was written, and a docs PR is bringing §6.3/§6.4
//  in line. Shipping them here would have re-forked a vocabulary #304's
//  own review had just spent a PR consolidating (PR #308 review).
//
import FireflyCore
import FireflyModel
import Foundation

enum CrewCopy {
    /// `HeardPresence` (core's presence axis) -> `PresenceTag` (the
    /// phone's label enum). The two enums are the same four states
    /// under different names — `.never` is core's "no packet ever
    /// received", which the phone calls `.linked` ("paired, never
    /// heard").
    static func tag(for presence: HeardPresence) -> PresenceTag {
        switch presence {
        case .heard: return .heard
        case .stale: return .stale
        case .lost: return .lost
        case .never: return .linked
        }
    }

    /// The one presence label, from the shipped vocabulary:
    /// `HEARD 40S` / `6 min ago` / `No signal · 40 min` / `Paired · not
    /// seen yet`. `ageMs` is `nil` only when genuinely unknown — it is
    /// never substituted with 0, which would read as "just now".
    static func presenceLabel(_ presence: HeardPresence, ageMs: UInt32?) -> String {
        tag(for: presence).plainLabel(age: ageMs.map { TimeInterval($0) / 1000 })
    }

    /// "New crew member" — the SAME literal every other nameless-row
    /// surface uses (`CrewDisplayFallback.namelessMember`), never a
    /// second spelling of it.
    static func displayName(_ name: String?) -> String {
        guard let name, !name.isEmpty else { return CrewDisplayFallback.namelessMember }
        return name
    }
}
