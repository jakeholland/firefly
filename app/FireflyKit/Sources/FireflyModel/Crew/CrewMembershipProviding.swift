//
//  CrewMembershipProviding.swift — the seam A02 slice B's Start screen
//  and Crew page read, and the seam slice C's engine implements.
//
//  Two protocols, deliberately separate because they have two different
//  callers and two different lifetimes:
//
//   * `CrewMembershipGating` is what `CoreStore` holds. It answers ONE
//     question, synchronously, for every node snapshot that arrives:
//     "may this node's data reach `ff_crew` at all?" — the membership
//     gate `docs/specs/A02-crew-join.md` AC13 puts in front of
//     `CoreStore.apply(nodeUpdate:)`.
//   * `CrewMembershipProviding` is what the UI reads: who is in the
//     crew, who joined since it was created, who the roster could not
//     fit, and the hide/unhide actions. No SwiftUI here — slice B owns
//     every screen; this file owns only the vocabulary the two slices
//     have to agree on.
//
//  Both are `@MainActor`: the crew roster is `ff_crew_t`, and every
//  `ff_*` context in this app lives in one isolation domain (A01's
//  threading model).
//
import FireflyMesh
import Foundation

/// The crew channel, as far as membership is concerned: the canonical
/// crew code (which IS the channel's `settings.name` — A02 §1.3, "why
/// the channel name *is* the code") and the 32-byte PSK derived from it
/// (§1.4). Both halves are required to resolve the index, and matching
/// on only one of them is the bug AC14 exists to prevent: a name match
/// alone would admit strangers from someone else's channel that happens
/// to share a name, and a PSK match alone would not tell two crews with
/// the same key apart.
///
/// Slice A owns the codec that MINTS these (`CrewCode.generate` /
/// `CrewCode.psk(for:)`); this type is deliberately just the pair of
/// values, so slice C neither depends on that codec landing first nor
/// duplicates any of it.
public struct CrewChannelIdentity: Sendable, Equatable {
    /// The canonical code, exactly as it is written into
    /// `ChannelSettings.name` — e.g. `FIRE-4K9M7X`.
    public let code: String
    /// The derived 32-byte channel key.
    public let psk: Data

    public init(code: String, psk: Data) {
        self.code = code
        self.psk = psk
    }
}

/// Where the crew channel sits on THIS radio — "inherently a local
/// concept" (mesh.proto), so this is resolved per connection and never
/// assumed (A02 §4.2, AC14).
public enum CrewChannelStatus: Sendable, Equatable {
    /// No crew code is configured at all. Every existing install before
    /// A02 is in this state, and it is not an error: the manually-paired
    /// roster keeps working exactly as it did (§4.6), and nobody is
    /// auto-admitted because there is no channel to be heard on.
    case noCrew
    /// A crew is configured but the radio's channel table has not been
    /// read yet (no link, or the read is in flight). Admits nobody —
    /// "not yet known" is not "index 0".
    case resolving
    /// The crew channel occupies this index on this radio.
    case resolved(index: UInt32)
    /// The radio's channel table holds no index whose name AND PSK match
    /// the crew. The Crew page says so honestly — *"Your puck isn't on
    /// this crew's channel"* with a **Fix it** button — and this engine
    /// admits nobody rather than falling back to index 0 (AC14).
    case notOnCrewChannel
}

/// How a member came to be in the roster — the distinction §4.6's
/// migration copy is built on ("From before" vs. the crew you just
/// started or joined).
public enum CrewMemberOrigin: Sendable, Equatable {
    /// Admitted automatically by A02 §4.1, on the current crew code.
    case joinedCrew(at: Date)
    /// A `CrewPairingRecord` that predates this crew code — a member
    /// paired by hand in M2, or carried over from a previous crew.
    /// **Never auto-removed** (§4.6): auto-removing these would silently
    /// delete the user's crew on upgrade.
    case fromBefore
}

/// One crew member, as the Crew page and the Start screen read them.
public struct CrewMembershipRecord: Sendable, Equatable, Identifiable {
    public var id: UInt32 { nodeID }
    public let nodeID: UInt32
    public let colorIndex: UInt8
    /// The LOCAL nickname, if the user set one — never a mesh-reported
    /// name (`CrewPairingRecord.nickname`'s own doc comment).
    public let nickname: String?
    public let origin: CrewMemberOrigin

    public init(nodeID: UInt32, colorIndex: UInt8, nickname: String?, origin: CrewMemberOrigin) {
        self.nodeID = nodeID
        self.colorIndex = colorIndex
        self.nickname = nickname
        self.origin = origin
    }
}

/// One auto-admission, for the Start screen's "Joined N" (§2.3).
/// Persisted per crew code, so the count survives a relaunch and a
/// leave-and-rejoin.
public struct CrewJoinEvent: Sendable, Equatable, Codable, Identifiable {
    public var id: UInt32 { nodeID }
    public let nodeID: UInt32
    public let joinedAt: Date

    public init(nodeID: UInt32, joinedAt: Date) {
        self.nodeID = nodeID
        self.joinedAt = joinedAt
    }
}

/// Somebody who qualified for the crew but did not fit: `FF_CREW_MAX`
/// is 8 and stays 8 (§4.3). The 9th person is NOT silently dropped —
/// they are listed, honestly, under "Not tracked (N)" with their
/// last-heard age, and hiding a member frees the slot.
public struct UntrackedCrewMember: Sendable, Equatable, Identifiable {
    public var id: UInt32 { nodeID }
    public let nodeID: UInt32
    /// When this id first qualified and could not be admitted.
    public let firstHeard: Date
    /// The most recent qualifying packet from this id.
    public let lastHeard: Date

    public init(nodeID: UInt32, firstHeard: Date, lastHeard: Date) {
        self.nodeID = nodeID
        self.firstHeard = firstHeard
        self.lastHeard = lastHeard
    }
}

/// The gate `CoreStore` consults before feeding ANY node snapshot into
/// `ff_crew` (A02 AC13). One synchronous question per snapshot, on the
/// main actor, in stream order — which is why admission has to be
/// decided HERE rather than on a second stream: the NodeInfo packet
/// that admits a joiner and the snapshot that carries their name are
/// the same event, and two independent `AsyncStream`s could deliver
/// them in either order.
@MainActor
public protocol CrewMembershipGating: AnyObject {
    /// `true` iff `snapshot`'s node may reach `ff_crew` — because it is
    /// already crew, or because THIS snapshot admits it under §4.1.
    /// Admission is a side effect of answering: the node is paired, given
    /// a colour and persisted before this returns.
    func admits(_ snapshot: MeshNodeSnapshot) -> Bool
}

/// What slice B's Crew page and Start screen read.
@MainActor
public protocol CrewMembershipProviding: AnyObject {
    /// The crew channel this engine admits on, or `nil` when no crew
    /// code is configured (§4.6's pre-A02 install).
    var crewChannel: CrewChannelIdentity? { get }
    /// Where that channel sits on this radio — see `CrewChannelStatus`.
    var channelStatus: CrewChannelStatus { get }
    /// Every current crew member, in roster order.
    var members: [CrewMembershipRecord] { get }
    /// Auto-admissions on the current crew code, oldest first.
    var joinedSinceCreated: [CrewJoinEvent] { get }
    /// Qualified, but the roster is full (§4.3). Empty in the normal
    /// case; non-empty is what drives the Crew page's overflow banner.
    var untracked: [UntrackedCrewMember] { get }
    /// Hidden ids, in the order they were hidden.
    var hidden: [UInt32] { get }
    func isHidden(_ nodeID: UInt32) -> Bool
    /// Hide = unpair + remember (§4.5). Local to this phone, never
    /// transmitted, and it frees a roster slot.
    func hide(nodeID: UInt32)
    /// Unhide. The member comes back on their next qualifying packet —
    /// nothing is fabricated to make them reappear immediately.
    func unhide(nodeID: UInt32)
}
