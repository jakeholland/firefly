//
//  CrewPairingStore.swift — M2's persisted crew pairing seam (docs/specs/
//  A01-companion-app.md, M2: "Crew pairing and colours, driven by
//  `ff_crew`"; docs/specs/S02-core-crew.md; S11/S21's settings-persistence
//  semantics, mirrored here for the paired list).
//
//  `ff_crew_t` itself (`Bridge/CrewStore.swift`) is RAM-only — it is
//  re-initialized empty every launch (`ff_crew_init`), same as the
//  puck's own boot-time state. Before this file, the ONLY thing that
//  ever called `ff_crew_set_paired` from the app was `NearbyNodesViewModel
//  .toggleCrew(_:)`'s own local, session-only `Set<UInt32>` — that
//  view model's own doc comment: "Toggling this changes only what THIS
//  SCREEN shows, in THIS launch; it does not persist". This file is
//  the fix: a small, typed seam — same shape as `SettingsStoring`/
//  `FireflyExtraSettingsStoring` next door — that remembers node id ->
//  paired + colour index + optional nickname across relaunches, and
//  `CrewPairingRestorer` below is what replays it back onto a fresh
//  `ff_crew_t` at launch.
//
//  Landed as its OWN protocol/type rather than folded into
//  `SettingsStoring`/`FireflyExtraSettingsStoring`: those are landed,
//  frozen infra other slices depend on by their CURRENT shape (see
//  `SettingsStore.swift`'s own header comment on why its four extra
//  keys got a refining protocol instead of widening `SettingsKey`) —
//  three other M2 slices are building in parallel worktrees against
//  those same files right now, so growing them out from under those
//  slices is exactly the collision the coordinator's slicing rule
//  exists to avoid. A new file, a new protocol, wired in through
//  `AppDependencies`' own append-only convention (see that file), costs
//  those slices nothing.
//
//  Colour assignment lives here too (`CrewColorAssignment`), not in
//  `Bridge/CrewStore.swift`: the OLD scheme there derived `color_idx`
//  from `nodeID % 8` on every `setIdentity` call (a real bug — two
//  paired members can collide on `nodeID % 8`, and a later NodeInfo
//  re-announcement re-derived and silently reassigned the colour a
//  member had already been shown with). M2 replaces it with "first
//  free index in roster order": scan the OTHER currently-paired
//  members' colours and hand the new member the lowest index none of
//  them hold, assigned ONCE at pairing time and persisted — never
//  re-derived, so it survives every later identity update the mesh
//  sends. See `Bridge/CrewStore.swift`'s `setColorIndex`/`setIdentity`
//  doc comments for the other half of this change.
//
import FireflyCore
import Foundation

/// One paired member's persisted record: identity beyond what the mesh
/// itself reports is exactly `nodeID` + the two things this app decides
/// (colour, and an optional local nickname) — never a position, a
/// name, or anything else `ff_crew` already owns and re-derives from
/// live traffic. `nickname` is a LOCAL DRAFT ONLY, the same convention
/// `SettingsStore.nodeLongNamePreference` documents: it is never
/// written into `ff_crew_member_t.long_name` (which the MESH owns —
/// doing so would mean a later real NodeInfo silently overwrites it,
/// or the reverse: a stale local nickname could linger as if it were
/// the node's actual reported name). It stays purely an app-side
/// override, read by the Crew section in More.
public struct CrewPairingRecord: Sendable, Equatable, Codable {
    public let nodeID: UInt32
    public var colorIndex: UInt8
    public var nickname: String?

    public init(nodeID: UInt32, colorIndex: UInt8, nickname: String? = nil) {
        self.nodeID = nodeID
        self.colorIndex = colorIndex
        self.nickname = nickname
    }
}

/// The persistence seam for M2's paired crew list. Methods, not a
/// settable array property, matching `SettingsStoring`'s own rationale
/// (a conforming type stays a plain class, no exposed mutable storage
/// across actor boundaries).
public protocol CrewPairingStoring: AnyObject, Sendable {
    /// Every paired record, in ROSTER order — the order each member was
    /// first paired. This order is load-bearing: it is what
    /// `CrewPairingRestorer` replays at launch and what
    /// `CrewColorAssignment` scans to find the first free colour for a
    /// NEW member, so two members can never silently swap positions
    /// between launches.
    func records() -> [CrewPairingRecord]
    /// Adds `nodeID` at the END of roster order if new, or updates its
    /// colour/nickname in place (never moving it) if already present.
    func upsert(_ record: CrewPairingRecord)
    func remove(nodeID: UInt32)
}

extension CrewPairingStoring {
    public func record(for nodeID: UInt32) -> CrewPairingRecord? {
        records().first { $0.nodeID == nodeID }
    }
}

/// The M1-style stand-in: same shape as a real persisted store, with
/// nothing that survives a process relaunch — what a hermetic test
/// needs, and what `.stub()`/`.demoBundle()` use by default (matching
/// `InMemorySettingsStore`'s own role for the six shared keys).
public final class InMemoryCrewPairingStore: CrewPairingStoring, @unchecked Sendable {
    private let lock = NSLock()
    private var order: [CrewPairingRecord] = []

    public init() {}

    public func records() -> [CrewPairingRecord] {
        lock.lock(); defer { lock.unlock() }
        return order
    }

    public func upsert(_ record: CrewPairingRecord) {
        lock.lock(); defer { lock.unlock() }
        if let idx = order.firstIndex(where: { $0.nodeID == record.nodeID }) {
            order[idx] = record
        } else {
            order.append(record)
        }
    }

    public func remove(nodeID: UInt32) {
        lock.lock(); defer { lock.unlock() }
        order.removeAll { $0.nodeID == nodeID }
    }
}

/// The real, `UserDefaults`-backed store. One JSON-encoded array under
/// one namespaced key — a typed array of small records has no natural
/// per-field `UserDefaults` key the way `SettingsStore`'s scalar
/// preferences do, so this follows `Codable` + a single key rather than
/// inventing eight parallel per-index keys.
public final class CrewPairingStore: CrewPairingStoring, @unchecked Sendable {
    private let defaults: UserDefaults
    private let lock = NSLock()
    private static let key = "firefly.settings.crewPairing.v1"

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    public func records() -> [CrewPairingRecord] {
        lock.lock(); defer { lock.unlock() }
        guard let data = defaults.data(forKey: Self.key) else { return [] }
        return (try? JSONDecoder().decode([CrewPairingRecord].self, from: data)) ?? []
    }

    public func upsert(_ record: CrewPairingRecord) {
        lock.lock()
        var current = (try? JSONDecoder().decode([CrewPairingRecord].self,
                                                   from: defaults.data(forKey: Self.key) ?? Data())) ?? []
        if let idx = current.firstIndex(where: { $0.nodeID == record.nodeID }) {
            current[idx] = record
        } else {
            current.append(record)
        }
        write(current)
        lock.unlock()
    }

    public func remove(nodeID: UInt32) {
        lock.lock()
        var current = (try? JSONDecoder().decode([CrewPairingRecord].self,
                                                   from: defaults.data(forKey: Self.key) ?? Data())) ?? []
        current.removeAll { $0.nodeID == nodeID }
        write(current)
        lock.unlock()
    }

    /// Caller already holds `lock`.
    private func write(_ records: [CrewPairingRecord]) {
        guard let data = try? JSONEncoder().encode(records) else { return }
        defaults.set(data, forKey: Self.key)
    }
}

/// "First free index in roster order" — see this file's header
/// comment for why this replaced the old `nodeID % 8` derivation.
public enum CrewColorAssignment {
    /// The lowest palette index (`0..<FF_CREW_MAX`) not already held by
    /// another currently-paired member. `nil` only if every slot is
    /// taken, which cannot happen while the roster itself is capped at
    /// `FF_CREW_MAX` (8) and every paired member holds exactly one
    /// index — a caller that already checked "roster not full" before
    /// assigning a colour will never see `nil` in practice, but this
    /// stays honest about the theoretical case rather than force-
    /// unwrapping.
    public static func nextFreeIndex(usedIndices: Set<UInt8>) -> UInt8? {
        for candidate in UInt8(0)..<UInt8(FF_CREW_MAX) where !usedIndices.contains(candidate) {
            return candidate
        }
        return nil
    }
}

/// Replays a persisted paired list back onto a fresh `ff_crew_t` — the
/// ONE place this has to run, and it has to run BEFORE anything else
/// subscribes that `ff_crew_t` to a client's `nodeUpdates()` stream
/// (`AppGraph.init`'s own comment: this happens before `start()`,
/// i.e. before any want_config replay can call `CoreStore.apply
/// (nodeUpdate:)` -> `crew.setIdentity`). `setIdentity` no longer
/// touches `color_idx` at all (`Bridge/CrewStore.swift`'s own doc
/// comment), so ordering here only has to win the PAIRED flag, not a
/// race over colour — but restoring before any replay is still what
/// keeps a member who reconnects mid-session from ever reading as
/// unpaired for even one frame.
public enum CrewPairingRestorer {
    public static func restore(from store: any CrewPairingStoring, into crew: CrewStore) {
        for record in store.records() {
            guard crew.upsert(nodeID: record.nodeID) else { continue }
            crew.setPaired(nodeID: record.nodeID, paired: true)
            crew.setColorIndex(nodeID: record.nodeID, index: record.colorIndex)
        }
    }
}

/// The single place that ADDS/REMOVES a crew member: writes both halves
/// — `ff_crew_t` (live, RAM-only, what every screen renders) and the
/// persisted `CrewPairingStoring` record (what survives a relaunch) —
/// so the two can never drift apart the way `NearbyNodesViewModel`'s
/// old local-only toggle risked. `@MainActor`-confined: `crew` is a
/// `CrewStore`, and every `ff_*` context in this app is confined to
/// one isolation domain (A01's threading model).
///
/// `CrewPairingController` is the ONLY writer of `ff_crew`'s paired
/// flag: the persisted store is authoritative, and `ff_crew` is always
/// rebuilt from it at launch (`CrewPairingRestorer.restore`, above) —
/// so on any conflict between the two, the persisted record wins.
@MainActor
public final class CrewPairingController {
    /// Mirrors `FF_CREW_MAX` (`ff_crew.h`) — the roster cap this
    /// controller enforces ITS OWN side of, in addition to whatever
    /// `ff_crew_upsert` itself does. Kept as a separate check (not just
    /// "trust the core's return value") because issue #266's concurrent
    /// eviction PR is changing what `ff_crew_upsert` does when the
    /// roster is full-but-some-slots-are-unpaired; this controller's
    /// `.full` result is defined against the PAIRED count this store
    /// tracks, which stays true regardless of how core's own eviction
    /// policy evolves underneath it.
    public static let maxCrewSize = Int(FF_CREW_MAX)

    public enum PairResult: Sendable, Equatable {
        case paired(colorIndex: UInt8)
        /// Crew is already at `CrewPairingController.maxCrewSize` — the
        /// honest limit message, never a silent no-op (M2's own
        /// acceptance criterion).
        case full(limit: Int)
    }

    public let crew: CrewStore
    private let store: any CrewPairingStoring

    public init(crew: CrewStore, store: any CrewPairingStoring) {
        self.crew = crew
        self.store = store
    }

    /// Every paired member's record, in roster order — what the Connect
    /// screen's Nearby section and the Crew section in More both list
    /// from.
    public func pairedRecords() -> [CrewPairingRecord] { store.records() }

    public var isFull: Bool { store.records().count >= Self.maxCrewSize }

    /// Pairs `nodeID`, assigning it a fresh colour (first free index in
    /// roster order) if it was not already paired, or re-affirming its
    /// EXISTING colour if it was (never reassigning one on a repeat
    /// call — a member's colour, once given, only ever changes by
    /// `unpair` + a later fresh `pair`). Returns `.full` without
    /// touching `ff_crew` at all when the roster is already at the
    /// limit and `nodeID` is not already one of its members.
    @discardableResult
    public func pair(nodeID: UInt32) -> PairResult {
        let existing = store.records()
        if let already = existing.first(where: { $0.nodeID == nodeID }) {
            crew.upsert(nodeID: nodeID)
            crew.setPaired(nodeID: nodeID, paired: true)
            crew.setColorIndex(nodeID: nodeID, index: already.colorIndex)
            return .paired(colorIndex: already.colorIndex)
        }
        guard existing.count < Self.maxCrewSize else { return .full(limit: Self.maxCrewSize) }
        // The core's own cap (`ff_crew_upsert` -> nil when full) should
        // agree with the check above, but is re-checked rather than
        // assumed: the concurrent eviction PR (issue #266) is changing
        // exactly this return, and neither side should have to trust
        // the other's math to stay honest about a failure.
        guard crew.upsert(nodeID: nodeID) else { return .full(limit: Self.maxCrewSize) }
        let usedColors = Set(existing.map(\.colorIndex))
        let colorIndex = CrewColorAssignment.nextFreeIndex(usedIndices: usedColors) ?? 0
        crew.setPaired(nodeID: nodeID, paired: true)
        crew.setColorIndex(nodeID: nodeID, index: colorIndex)
        store.upsert(CrewPairingRecord(nodeID: nodeID, colorIndex: colorIndex))
        return .paired(colorIndex: colorIndex)
    }

    /// Unpairs `nodeID`: removes its persisted record — the durable,
    /// authoritative half — BEFORE `ff_crew_set_paired(false)` (the
    /// member's slot stays, merely-heard, per `ff_crew.h`'s own "no
    /// eviction" policy). This order matters for crash safety: the
    /// persisted store is what `CrewPairingRestorer.restore` rebuilds
    /// `ff_crew`'s paired flags from at next launch, so if the process
    /// is killed between these two lines, removing the persisted
    /// record first means the worst case is a member who still reads
    /// as paired for the REST OF THIS LAUNCH ONLY — safe to lose, and
    /// corrected the moment the record is gone. Writing `ff_crew`
    /// first (the old order) had the opposite, unsafe failure mode: a
    /// kill between the two lines left the persisted record still
    /// saying "paired," so the next restore silently re-paired someone
    /// the user had just removed.
    public func unpair(nodeID: UInt32) {
        store.remove(nodeID: nodeID)
        crew.setPaired(nodeID: nodeID, paired: false)
    }

    /// Sets (or clears, for `nil`/empty) a paired member's LOCAL
    /// nickname — see `CrewPairingRecord.nickname`'s own doc comment
    /// for why this never touches `ff_crew_member_t.long_name`. A
    /// no-op for a node that is not currently paired.
    public func rename(nodeID: UInt32, nickname: String?) {
        guard var record = store.record(for: nodeID) else { return }
        record.nickname = (nickname?.isEmpty == false) ? nickname : nil
        store.upsert(record)
    }
}
