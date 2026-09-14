//
//  CrewSettingsViewModel.swift — the "Crew" section in More: every
//  paired member, with rename and a colour swatch (docs/specs/
//  A01-companion-app.md, M2: "Crew pairing and colours, driven by
//  `ff_crew`"). Backed by the SAME `CrewPairingController` Connect's
//  Nearby section writes through — a rename or a remove here is the
//  exact same seam, never a second copy of the paired list.
//
import FireflyModel
import Foundation
import Observation

@MainActor
@Observable
final class CrewSettingsViewModel {
    struct Row: Identifiable, Equatable {
        let id: UInt32
        /// `ff_crew_display_name`'s own answer — empty until the mesh
        /// has actually reported a name for this node, never guessed.
        let meshName: String
        /// The LOCAL DRAFT nickname (`CrewPairingRecord.nickname`'s own
        /// doc comment) — `nil` until the person renames this row.
        let nickname: String?
        let colorIndex: Int
        /// `ff_crew_member_t.initial`'s own value — `nil` until the
        /// mesh has actually reported one. Owner decision, 2026-09-13
        /// ("Nameless crew rows... a colour and initial '?'"):
        /// `CrewMemberRow`'s swatch shows this letter, or "?" when it is
        /// still unknown, rather than a bare colour circle.
        let initial: Character?

        /// Nickname first (it is what the person asked to see), then
        /// the mesh's own name (already "the radio's short name if
        /// known" — `ff_crew_display_name`'s own long-name-else-short-
        /// name rule), then "New crew member" (owner decision,
        /// 2026-09-13: "any row without a name... never a blank
        /// label") — never the raw `!nodeid` hex, which reads as an
        /// error code, not a person.
        var displayName: String {
            if let nickname, !nickname.isEmpty { return nickname }
            if !meshName.isEmpty { return meshName }
            return CrewDisplayFallback.namelessMember
        }
    }

    private(set) var rows: [Row] = []
    private let pairing: CrewPairingController

    init(pairing: CrewPairingController) {
        self.pairing = pairing
        refresh()
    }

    func refresh() {
        let now = FireflyClock.nowMillis()
        rows = pairing.pairedRecords().map { record in
            let member = pairing.crew.member(nodeID: record.nodeID, now: now)
            return Row(id: record.nodeID, meshName: member?.displayName ?? "",
                       nickname: record.nickname, colorIndex: Int(record.colorIndex), initial: member?.initial)
        }
    }

    func rename(_ nodeID: UInt32, to nickname: String) {
        pairing.rename(nodeID: nodeID, nickname: nickname)
        refresh()
    }

    func remove(_ nodeID: UInt32) {
        pairing.unpair(nodeID: nodeID)
        refresh()
    }
}
