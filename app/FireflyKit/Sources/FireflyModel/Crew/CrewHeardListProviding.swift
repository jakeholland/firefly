//
//  CrewHeardListProviding.swift — A02 slice E's two Advanced-only seams
//  (`docs/specs/A02-crew-join.md` §4.7/§6.5, and the "Crew diagnostics"
//  row): read-only windows onto state `CrewMembershipEngine` already
//  keeps for its own admission decisions (#306), narrowed to exactly
//  what the two new screens need so a view model can be tested against
//  a small fake instead of the full engine + a live `MeshtasticClient`.
//
//  Interpretation call, stated here rather than silently: §4.7 in the
//  spec text describes "People my puck hears" as radios heard OFF the
//  crew channel entirely (today's `NearbyNodesViewModel`/nodeDB
//  strangers). This slice's own task brief overrides that: the roster-
//  trust policy (S16) never grows from raw nodeDB strangers, and a list
//  fed from them would either be empty by construction (they were never
//  crew) or would have to re-litigate roster trust to populate at all.
//  What §4.3 already describes — the crew-channel overflow list ("Not
//  tracked (N)") plus this phone's own hide list (§4.5) — is honest,
//  already-tracked, bounded state on `CrewMembershipEngine`, and is what
//  "People my puck hears" renders here. Noted in the PR body.
//
import Foundation

/// The Advanced screen's "People my puck hears" (§4.7 as scoped above):
/// crew-channel radios that are not full crew members. `CrewMembershipEngine`
/// conforms to this with ZERO extra code — its existing `untracked`/
/// `hidden`/`hide(nodeID:)`/`unhide(nodeID:)` already match this shape
/// exactly, which is deliberate: there is no second copy of this state
/// anywhere, only a narrower name for reading it.
@MainActor
public protocol CrewHeardListProviding: AnyObject {
    /// §4.3's overflow list — qualified for the crew, but `FF_CREW_MAX`
    /// was already full. "Make room" (hiding an existing member) is
    /// what frees a slot for one of these.
    var untracked: [UntrackedCrewMember] { get }
    /// §4.5's hide list, by node id — "Unhide" restores nothing by
    /// itself (§4.5's own rule: nothing is re-paired here), only makes
    /// the id eligible again on its next qualifying packet.
    var hidden: [UInt32] { get }
    /// §4.5 — unpairs (if paired) and remembers the hide. Used here as
    /// "Make room": hiding an EXISTING crew member to free a slot for
    /// someone on the overflow list.
    func hide(nodeID: UInt32)
    /// §4.5 — removes the hide. Never re-pairs by itself.
    func unhide(nodeID: UInt32)
}

/// The Advanced screen's "Crew diagnostics" row (§6.5): the SAME
/// admission decisions `CrewMembershipGating.admits(_:)` makes, read out
/// as plain counts and the resolved channel — never a second gate,
/// never anything that can influence admission.
@MainActor
public protocol CrewDiagnosticsProviding: AnyObject {
    /// `nil` before any Start/Join — the diagnostics row's own "no crew
    /// set" case, kept distinct from "resolving" and from a genuine
    /// zero count (this file's own header comment on why UNKNOWN is
    /// never rendered as 0).
    var crewChannel: CrewChannelIdentity? { get }
    /// Where the crew channel sits on THIS radio, resolved by name AND
    /// PSK (§4.2, AC14) — never a fallback to index 0.
    var channelStatus: CrewChannelStatus { get }
    /// Admitted / refused-by-reason since this process last configured
    /// a crew. Never persisted; a relaunch legitimately restarts the
    /// count.
    var admissionCounters: CrewAdmissionCounters { get }
    /// Epoch milliseconds of the most recent admission, or `nil` if
    /// none has happened yet THIS session — never 0, which would render
    /// as "just now" (this module's own honest-data rule).
    var lastAdmissionAtMs: UInt64? { get }
}
