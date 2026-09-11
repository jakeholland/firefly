//
//  CrewStore.swift — the Swift-safe wrapper over `firmware/core/ff_crew`
//  (docs/specs/A01-companion-app.md, slice B).
//
//  Heap-owns exactly one `ff_crew_t` and the `CoreClock` it borrows
//  (see CoreClock.swift's top comment for why the two are allocated and
//  freed together), initialised once by `ff_crew_init` and torn down
//  once by `deinit`. Every public entry point takes and returns plain
//  Swift values — no `UnsafeMutablePointer`, no imported C tuple — per
//  the bridge's "C types never leave the bridge" rule; the raw pointer
//  exists only as an `internal` seam for other Bridge/* types in this
//  module (`RadarBridge` needs the whole roster, not a per-member
//  snapshot, to call `ff_radar_compute`).
//
//  Threading: not `@MainActor` itself (the bridge takes plain values,
//  so it is testable with no actor context at all — see this file's
//  own tests), but docs/specs/A01-companion-app.md's "Threading model"
//  requires every caller to confine ONE instance to a single isolation
//  domain (`CoreStore`'s `@MainActor`, in the app) — the C core has no
//  locks, by design, and this wrapper adds none either.
//
import FireflyCore
import Foundation

/// `ff_crew_presence_t` (core/ff_crew.h) — "is the radio still hearing
/// this person", from ANY packet, never gated on a position ever having
/// arrived. See ff_crew.h's own doc comment for the full
/// presence-vs-freshness distinction this axis is half of.
public enum HeardPresence: Sendable, Equatable, CaseIterable {
    case heard, stale, lost, never

    init(ffPresence: ff_crew_presence_t) {
        switch ffPresence {
        case FF_CREW_PRESENCE_HEARD: self = .heard
        case FF_CREW_PRESENCE_STALE: self = .stale
        case FF_CREW_PRESENCE_LOST: self = .lost
        default: self = .never
        }
    }
}

/// `ff_freshness_t` (core/ff_crew.h) — "how much to trust this
/// member's POSITION", a separate axis from `HeardPresence` above.
/// `.asserted` is issue #33's whole point: a typed-in (Meshtastic
/// LOC_MANUAL) position is not a measurement, so elapsed time is a
/// category error for it — it can never be simultaneously `.asserted`
/// and `.live`/`.stale`/`.lost`/`.never`.
public enum FreshnessCategory: Sendable, Equatable, CaseIterable {
    case live, stale, lost, never, asserted

    init(ffFreshness: ff_freshness_t) {
        switch ffFreshness {
        case FF_FRESH_LIVE: self = .live
        case FF_FRESH_STALE: self = .stale
        case FF_FRESH_LOST: self = .lost
        case FF_FRESH_ASSERTED: self = .asserted
        default: self = .never
        }
    }
}

/// `ff_crew_rssi_trend`'s -1/0/+1, named rather than left as a raw
/// integer a renderer would have to re-interpret.
public enum RSSITrend: Sendable, Equatable {
    case rising, falling, flat

    init(raw: Int8) {
        if raw > 0 { self = .rising } else if raw < 0 { self = .falling } else { self = .flat }
    }
}

/// A read-only snapshot of one `ff_crew_member_t`, decoded into plain
/// Swift values as of the `now` the caller supplied — ages are computed
/// at snapshot time, never cached, matching every `ff_crew_*` freshness
/// function's own "explicit `now_ms` in" convention (ff_crew.h's top
/// comment).
public struct CrewMember: Sendable, Equatable, Identifiable {
    public var id: UInt32 { nodeID }
    public let nodeID: UInt32
    public let shortName: String
    public let longName: String
    /// `ff_crew_display_name`'s own answer: `longName` when non-empty,
    /// else `shortName` — computed by calling the core function itself,
    /// not a reimplementation of its selection rule.
    public let displayName: String
    public let initial: Character?
    public let colorIndex: UInt8
    public let paired: Bool

    public struct Position: Sendable, Equatable {
        public let latitude: Double
        public let longitude: Double
        /// `now - pos_age_ms` at the moment this snapshot was taken
        /// (wraparound-safe unsigned subtraction, `ff_clock_t`'s own
        /// convention).
        public let ageMs: UInt32
        /// LOC_MANUAL — typed in, never measured (issue #33). See
        /// `FreshnessCategory.asserted`.
        public let asserted: Bool
        /// nil = the sender didn't state precision — NOT full
        /// precision (issue #47).
        public let precisionBits: UInt8?
    }
    /// nil = no position has ever arrived for this member
    /// (`FF_FRESH_NEVER`).
    public let position: Position?

    /// nil = unknown (`battery_pct == -1`).
    public let batteryPercent: Int8?
    public let status: String

    public struct DirectSignal: Sendable, Equatable {
        public let rssiDbm: Int16
        public let ageMs: UInt32
    }
    /// nil = never had a DIRECT packet (`rssi_dbm == INT16_MIN`). A
    /// relayed packet's RSSI belongs to the relay, never the sender.
    public let directSignal: DirectSignal?

    public let freshness: FreshnessCategory
    public let heardPresence: HeardPresence
    /// True iff the MOST RECENT sighting (not necessarily the latest
    /// direct RSSI) arrived direct rather than relayed.
    public let heardDirect: Bool

    static func decode(_ member: ff_crew_member_t, now: UInt32) -> CrewMember {
        var m = member
        let shortName = FixedCString.decode(m.name)
        let longName = FixedCString.decode(m.long_name)
        let status = FixedCString.decode(m.status)
        let (display, freshness, presence) = withUnsafePointer(to: &m) {
            (ptr: UnsafePointer<ff_crew_member_t>) -> (String, FreshnessCategory, HeardPresence) in
            let displayName = String(cString: ff_crew_display_name(ptr))
            let fresh = FreshnessCategory(ffFreshness: ff_crew_freshness(ptr, now))
            let heard = HeardPresence(ffPresence: ff_crew_presence(ptr, now))
            return (displayName, fresh, heard)
        }

        let position: Position? = m.has_pos
            ? Position(latitude: m.pos.lat, longitude: m.pos.lon, ageMs: now &- m.pos_age_ms,
                       asserted: m.pos_asserted, precisionBits: m.has_precision_bits ? m.precision_bits : nil)
            : nil
        let directSignal: DirectSignal? = m.rssi_dbm == Int16.min
            ? nil
            : DirectSignal(rssiDbm: m.rssi_dbm, ageMs: now &- m.rssi_age_ms)

        return CrewMember(
            nodeID: m.node_id,
            shortName: shortName,
            longName: longName,
            displayName: display,
            initial: Character(ffInitial: m.initial),
            colorIndex: m.color_idx,
            paired: m.paired,
            position: position,
            batteryPercent: m.battery_pct == -1 ? nil : m.battery_pct,
            status: status,
            directSignal: directSignal,
            freshness: freshness,
            heardPresence: presence,
            heardDirect: m.heard_direct
        )
    }
}

/// Heap-owns one `ff_crew_t` (+ the `CoreClock` it borrows). See this
/// file's top comment for the ownership/threading rules.
public final class CrewStore {
    private let context: UnsafeMutablePointer<ff_crew_t>
    private let clock: CoreClock

    /// Node ids this store currently believes have a live slot in
    /// `ff_crew_t` — tracked here in Swift rather than by walking
    /// `ff_crew_t.members[]` (a fixed C array imported as an opaque
    /// tuple with no per-index accessor in the public header; `members`
    /// below reads each one back through `ff_crew_find`, the same
    /// public entry point any other caller would use).
    ///
    /// PR #268 review, SHOULD-FIX #2: this used to be an `[UInt32]`
    /// array, appended to (never pruned) on every `track()` call and
    /// scanned with O(N) `.contains` to dedupe. 2026-09-11 [api] S02
    /// amendment (issue #266) made that a real, not just theoretical,
    /// problem: before it, `nodeIDs` was implicitly bounded to
    /// `FF_CREW_MAX` (the roster rejected a 9th distinct id outright,
    /// so `track()` never saw one) — after it, `ff_crew_t` can churn
    /// through arbitrarily many distinct ids over a session (that's the
    /// eviction fix's whole point), so the old array grew unboundedly on
    /// a busy mesh and `members(now:)` — read by the UI, presumably per
    /// render — became an ever-growing O(N) walk.
    ///
    /// Fixed two ways at once: `Set<UInt32>` for O(1) average-case
    /// membership instead of O(N) `.contains`, AND `track()` now prunes
    /// every id `ff_crew_find` no longer resolves on every call (an
    /// eviction elsewhere in the roster silently drops OTHER previously
    /// tracked ids from `ff_crew_t`, same as before this fix — the old
    /// array just never noticed). Since `ff_crew_t` itself holds at most
    /// `FF_CREW_MAX` members at any time, a full prune-then-insert
    /// leaves this set no larger than the live roster — genuinely
    /// bounded, not merely "bounded in practice for one festival", even
    /// across thousands of distinct ids heard over a session.
    ///
    /// One observable consequence: `members(now:)` below no longer
    /// returns members in first-seen order (a `Set`'s iteration order is
    /// unspecified) — no current caller (`LiveAdapters.swift`,
    /// `CoreInboxProvider.swift`) depends on the order, only membership.
    private var nodeIDs: Set<UInt32> = []

    public init(now: @escaping () -> UInt32 = FireflyClock.nowMillis) {
        clock = CoreClock(now: now)
        context = UnsafeMutablePointer<ff_crew_t>.allocate(capacity: 1)
        context.initialize(to: ff_crew_t())
        ff_crew_init(context, clock.raw)
    }

    deinit {
        context.deinitialize(count: 1)
        context.deallocate()
        // `clock` is released by ARC right after this deinit body
        // returns, which is fine: nothing dereferences `clock.raw`
        // once `context` — the only thing that held it — is gone.
    }

    /// Internal-only: the live `ff_crew_t` this store owns, for other
    /// Bridge/* types in this module. Never exposed outside
    /// `FireflyModel` — "C types never leave the bridge."
    var raw: UnsafeMutablePointer<ff_crew_t> { context }

    /// Internal-only, for `BridgeCrewStoreTests`'s boundedness
    /// regression test (PR #268 review, SHOULD-FIX #2): how many ids
    /// `nodeIDs` currently holds. Never exposed outside `FireflyModel`
    /// — this is an implementation detail (the id-tracking set), not
    /// part of the public bridge surface.
    var trackedIDCount: Int { nodeIDs.count }

    private func track(_ nodeID: UInt32) {
        guard ff_crew_find(context, nodeID) != nil else { return }
        // Prune first: drop any previously tracked id the roster no
        // longer holds (evicted by this or an earlier call — the
        // bounded-unpaired-LRU eviction, 2026-09-11 S02 amendment, can
        // silently reclaim ANY unpaired slot, not just the one for
        // `nodeID`). `ff_crew_find` is O(FF_CREW_MAX), and this set is
        // never larger than `FF_CREW_MAX` on entry (same invariant this
        // loop maintains every call), so the sweep is O(FF_CREW_MAX^2)
        // worst case — a constant (≤ 64 probes), not O(N) in ids ever
        // seen.
        nodeIDs = nodeIDs.filter { ff_crew_find(context, $0) != nil }
        nodeIDs.insert(nodeID)
    }

    /// Find-or-create a slot for `nodeID`. Returns `false` only when the
    /// roster is full (`FF_CREW_MAX` = 8) AND every occupied slot is
    /// already paired — 2026-09-11 [api] ff_crew.h's S02 amendment
    /// (issue #266): a full roster of merely-heard strangers no longer
    /// blocks this. When full, core evicts the least-recently-heard
    /// UNPAIRED occupant (never a paired one) to admit `nodeID`; that
    /// evicted occupant's own record — including RSSI trend history —
    /// is dropped, per the same amendment's own documented decision.
    @discardableResult
    public func upsert(nodeID: UInt32) -> Bool {
        let ok = ff_crew_upsert(context, nodeID) != nil
        if ok { track(nodeID) }
        return ok
    }

    /// Mark `nodeID` paired/unpaired. 2026-09-11 [api] ff_crew.h's S02
    /// amendment (issue #266): `ff_crew_set_paired` now returns whether
    /// `nodeID` ends this call in the roster with the requested `paired`
    /// state — `false` only when the roster is full of `FF_CREW_MAX`
    /// (8) members who are ALL already paired (the one honest "no room"
    /// case left; pairing a node not yet in the roster otherwise always
    /// succeeds, evicting an unpaired stranger if needed). Marked
    /// `@discardableResult` since most existing callers don't need to
    /// react to the (usually impossible outside a fully-paired roster)
    /// failure; a future pairing UI can check it to show an honest
    /// "crew full" state instead of a silent no-op.
    @discardableResult
    public func setPaired(nodeID: UInt32, paired: Bool) -> Bool {
        let ok = ff_crew_set_paired(context, nodeID, paired)
        if ok { track(nodeID) }
        return ok
    }

    /// Write the identity fields the MESH reports (`NodeInfo.user`'s
    /// short/long name) into a member slot — `initial` too, since it is
    /// derived from whichever name arrived (`ff_crew.h`'s "app-assigned"
    /// note covers both, but `initial` is a pure function of the name
    /// fields, not an independent choice).
    ///
    /// There is no `ff_crew_set_names` to call: `ff_crew_member_t` is a
    /// fully-defined struct whose name fields the OWNING APP fills in
    /// (the header's own "display letter; '\0' until known" /
    /// "app-assigned" notes), exactly as the puck's shell does. So this
    /// writes through the mutable member pointer `ff_crew_upsert`
    /// already returns — the same pointer the core hands every caller —
    /// and the pointer never escapes this call.
    ///
    /// NOTHING here is synthesized: a nil `shortName` clears the field
    /// rather than deriving one from the long name (`long_name`'s own
    /// header note: "never synthesized from `name`"), and `initial` is
    /// '\0' — honestly unknown — until some name is actually known.
    ///
    /// Deliberately does NOT touch `color_idx` (M2 change — see
    /// `setColorIndex`'s own doc comment): a NodeInfo re-announcement is
    /// exactly the kind of routine, repeated event `CoreStore.apply
    /// (nodeUpdate:)` calls this on every time a name is present, and a
    /// colour re-derived on every one of those would have silently
    /// reassigned a member's colour mid-session under the OLD `nodeID %
    /// 8` scheme this method used to run here.
    @discardableResult
    public func setIdentity(nodeID: UInt32, shortName: String?, longName: String?) -> Bool {
        guard let member = ff_crew_upsert(context, nodeID) else { return false }
        track(nodeID)
        if let shortName { FixedCString.encode(shortName, into: &member.pointee.name) }
        if let longName { FixedCString.encode(longName, into: &member.pointee.long_name) }
        member.pointee.initial = CrewStore.initialByte(shortName: shortName, longName: longName)
        return true
    }

    /// The display letter, as a single C byte: the first ASCII
    /// letter/digit of the short name, else of the long name, else
    /// '\0' ("until known" — never a '?' placeholder, which would render
    /// as a real, wrong initial).
    static func initialByte(shortName: String?, longName: String?) -> CChar {
        for candidate in [shortName, longName] {
            guard let scalar = candidate?.unicodeScalars.first(where: { $0.properties.isAlphabetic || ("0"..."9").contains($0) }),
                  scalar.isASCII else { continue }
            return CChar(bitPattern: UInt8(String(scalar).uppercased().utf8.first ?? 0))
        }
        return 0
    }

    /// Sets a member's palette slot directly — `ff_crew_member_t
    /// .color_idx` is "app-assigned" (`ff_crew.h`), and as of M2 the
    /// app's assignment policy is `CrewPairingStore.swift`'s
    /// `CrewColorAssignment` ("first free index in roster order",
    /// assigned once at pairing time and persisted) rather than the
    /// OLD `nodeID % 8` derivation `setIdentity` used to apply on every
    /// identity update — that scheme could collide (two paired members
    /// sharing one `nodeID % 8` slot) and reassigned itself on every
    /// NodeInfo re-announcement, neither of which is honest once a
    /// colour is something the app promises to keep stable.
    /// Find-or-create (matches `setIdentity`'s own contract): a slot
    /// this store has never seen gets created rather than silently
    /// dropping the colour.
    public func setColorIndex(nodeID: UInt32, index: UInt8) {
        guard let member = ff_crew_upsert(context, nodeID) else { return }
        track(nodeID)
        member.pointee.color_idx = index
    }

    /// Provenance/precision accompanying one position report — Swift's
    /// side of `ff_crew_pos_meta_t`. `.none` (not asserted, precision
    /// unknown) is `FF_CREW_POS_META_NONE`'s own "least-claiming"
    /// default.
    public struct PositionMeta: Sendable, Equatable {
        public var asserted: Bool
        public var precisionBits: UInt8?
        public init(asserted: Bool = false, precisionBits: UInt8? = nil) {
            self.asserted = asserted
            self.precisionBits = precisionBits
        }
        public static let none = PositionMeta()

        var ffValue: ff_crew_pos_meta_t {
            ff_crew_pos_meta_t(asserted: asserted,
                                has_precision_bits: precisionBits != nil,
                                precision_bits: precisionBits ?? 0)
        }
    }

    /// Records a position fix. `rxTimeMs` is the caller's own clock
    /// reading at receipt — use `FireflyClock.millis(since:)` on the
    /// packet's own timestamp when one exists, never a re-read of "now"
    /// for a fix that already happened.
    public func onPosition(nodeID: UInt32, latitude: Double, longitude: Double, rxTimeMs: UInt32,
                            meta: PositionMeta = .none) {
        ff_crew_on_position(context, nodeID, ff_latlon_t(lat: latitude, lon: longitude), rxTimeMs, meta.ffValue)
        track(nodeID)
    }

    /// Direct-packet RSSI only — never call this for a relayed packet's
    /// reading (the caller's job to gate, same as core's).
    public func onRSSI(nodeID: UInt32, rssiDbm: Int16) {
        ff_crew_on_rssi(context, nodeID, rssiDbm)
        track(nodeID)
    }

    /// Records that ANY packet arrived from `nodeID`, direct or relayed.
    public func onHeard(nodeID: UInt32, rxTimeMs: UInt32, direct: Bool) {
        ff_crew_on_heard(context, nodeID, rxTimeMs, direct)
        track(nodeID)
    }

    public func member(nodeID: UInt32, now: UInt32) -> CrewMember? {
        guard let ptr = ff_crew_find(context, nodeID) else { return nil }
        return CrewMember.decode(ptr.pointee, now: now)
    }

    /// Every member currently occupying a live slot in the roster.
    /// Unspecified order (`nodeIDs` is a `Set` — see its doc comment);
    /// no current caller relies on ordering, only membership.
    public func members(now: UInt32) -> [CrewMember] {
        nodeIDs.compactMap { member(nodeID: $0, now: now) }
    }

    public var count: Int { Int(context.pointee.count) }

    public func selected(now: UInt32) -> CrewMember? {
        guard let ptr = ff_crew_selected(context) else { return nil }
        return CrewMember.decode(ptr.pointee, now: now)
    }

    public func selectNext() { ff_crew_select_next(context) }

    public func selectNode(_ nodeID: UInt32) { ff_crew_select_node(context, nodeID) }

    /// `distanceM: nil` means "distance unknown" (the RSSI leg of the
    /// OR can still fire honestly).
    public func closeRange(nodeID: UInt32, distanceM: Float?, now: UInt32) -> Bool {
        guard let ptr = ff_crew_find(context, nodeID) else { return false }
        return ff_crew_close_range(ptr, distanceM ?? -1, now)
    }

    public func rssiTrend(nodeID: UInt32, now: UInt32) -> RSSITrend {
        RSSITrend(raw: ff_crew_rssi_trend(context, nodeID, now))
    }

    /// `ff_fmt_distance` — metric/imperial formatting is the puck's own
    /// unit-boundary rules, not reimplemented here.
    public static func formatDistance(meters: Float, imperial: Bool) -> String {
        var buf = [CChar](repeating: 0, count: 32)
        ff_fmt_distance(&buf, buf.count, meters, imperial)
        return String(cString: buf)
    }

    /// `ff_fmt_age`.
    public static func formatAge(ms: UInt32) -> String {
        var buf = [CChar](repeating: 0, count: 32)
        ff_fmt_age(&buf, buf.count, ms)
        return String(cString: buf)
    }

    /// `ff_crew_pos_precision_grid_m` — the approximate cell edge a
    /// degraded-precision fix could be anywhere inside (issue #47).
    public static func positionPrecisionGridMeters(bits: UInt8) -> Float {
        ff_crew_pos_precision_grid_m(bits)
    }
}
