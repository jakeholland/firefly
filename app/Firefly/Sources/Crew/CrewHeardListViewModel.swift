//
//  CrewHeardListViewModel.swift — A02 slice E, task scope item 1:
//  Crew -> Advanced -> "People my puck hears" (`docs/specs/
//  A02-crew-join.md` §4.7/§6.5, as scoped for this slice —
//  `CrewHeardListProviding.swift`'s own header comment on why this
//  reads the crew-channel overflow/hide lists rather than raw nodeDB
//  strangers on other channels).
//
//  Plain view model, no SwiftUI import (same convention `CrewCopy`
//  follows) — `CrewHeardListView` renders `rows`, nothing else touches
//  `CrewMembershipEngine` for this screen.
//
import FireflyModel
import Foundation
import Observation

@MainActor
@Observable
final class CrewHeardListViewModel {
    /// `CrewMembershipEngine` in every real composition (via
    /// `CrewScreen`'s existing `membership` parameter, downcast — see
    /// `CrewAdvancedScreen`'s own comment on why); a small fake in
    /// tests.
    private let heard: any CrewHeardListProviding
    /// Current crew members — the "Make room" candidate list. The SAME
    /// `CrewMembershipProviding` seam `CrewScreen`'s People list already
    /// reads, so "who could I hide to make room" can never disagree
    /// with who the People list itself shows.
    private let membership: any CrewMembershipProviding
    private let now: () -> Date

    init(heard: any CrewHeardListProviding, membership: any CrewMembershipProviding,
         now: @escaping () -> Date = Date.init) {
        self.heard = heard
        self.membership = membership
        self.now = now
    }

    struct Row: Identifiable, Equatable {
        enum Kind: Equatable {
            /// §4.5 — hidden by this phone. "Unhide" is the only action.
            case hidden
            /// §4.3 — qualified for the crew but the roster was full.
            /// "Make room" is the only action.
            case overflow
        }
        let id: UInt32
        let kind: Kind
        /// `!08x`-style short id — the SAME format the existing Hidden
        /// (N) section already renders (`CrewScreen.hiddenMembers`),
        /// never a name: neither list carries one (§4.7's own "Name"
        /// action is explicitly NOT part of this slice's scope — see
        /// `CrewHeardListProviding.swift`'s header comment).
        let shortID: String
        /// Honest, age-carrying label — "heard 6 min ago" for overflow
        /// (reusing `PresenceAge`, the SAME vocabulary the Crew page's
        /// presence pills use, §6.3's "one vocabulary" rule), or a
        /// fixed sentence for hidden (there is no "heard" age that
        /// matters once you've chosen not to see it).
        let detail: String
    }

    var rows: [Row] {
        let hiddenRows = heard.hidden.sorted().map { nodeID in
            Row(id: nodeID, kind: .hidden, shortID: Self.shortID(nodeID), detail: "hidden from your radar")
        }
        let overflowRows = heard.untracked.map { member in
            Row(id: member.nodeID, kind: .overflow, shortID: Self.shortID(member.nodeID),
                detail: "heard \(PresenceAge.ago(max(0, now().timeIntervalSince(member.lastHeard))))")
        }
        // A TOTAL order for the same reason every other rendered list in
        // this module picks one (`CrewMembershipEngine.currentMembers`'s
        // own comment): hidden first (nothing about hidden members
        // changes moment to moment), then overflow oldest-heard first —
        // the SAME order `CrewMembershipEngine.noteUntracked` evicts by,
        // so "who gets bumped next" really does read top to bottom.
        // PR #313 review: this sorted by node id, which is neither that
        // order nor anything a reader could act on, and the test that
        // named the property used a fixture where id order and age order
        // happened to agree. Ties fall back to the node id, so the order
        // is still total.
        let ages = Dictionary(heard.untracked.map { ($0.nodeID, $0.lastHeard) },
                               uniquingKeysWith: { first, _ in first })
        return hiddenRows + overflowRows.sorted {
            let (l, r) = (ages[$0.id], ages[$1.id])
            guard let l, let r else { return $0.id < $1.id }
            return l == r ? $0.id < $1.id : l < r
        }
    }

    var isEmpty: Bool { rows.isEmpty }

    /// §4.3's banner text — shown only while the overflow list is
    /// non-empty, never a standing warning once everyone fits.
    var overflowBanner: String? {
        let count = heard.untracked.count
        guard count > 0 else { return nil }
        let people = count == 1 ? "1 more person is" : "\(count) more people are"
        return "\(people) on this crew than your puck can track (8 is the limit). Hide someone to make room."
    }

    /// "Make room" candidates — the crew you could hide one of. Sorted
    /// the same "newest joined first" order the Start/Crew screens
    /// already use, via the shared provider — never a second sort rule
    /// invented here.
    var makeRoomCandidates: [CrewJoinedMember] { membership.currentMembers() }

    func unhide(nodeID: UInt32) { heard.unhide(nodeID: nodeID) }

    /// "Make room": hides an EXISTING crew member so the overflow
    /// person can be admitted on their next qualifying packet (§4.3).
    /// Never re-pairs the overflow person directly — that is not how
    /// admission works (§4.1: only a real qualifying packet admits
    /// anyone), and pretending otherwise here would be exactly the kind
    /// of fabricated state this codebase's honesty rule forbids.
    func makeRoom(hiding existingMemberID: UInt32) { heard.hide(nodeID: existingMemberID) }

    static func shortID(_ nodeID: UInt32) -> String { String(format: "!%08x", nodeID) }
}
