//
//  InboxViewModel.swift — the Inbox screen's state (docs/specs/
//  A01-companion-app.md, slice E; docs/specs/S24-signals-inbox.md;
//  docs/specs/S22-signals-rework.md).
//
//  One row per conversation: the CREW conversation (broadcast traffic)
//  plus one row per paired crew member (direct traffic) — the same
//  membership rulebook `firmware/core/include/ff_inbox.h` documents
//  ("Conversation membership") and the same ordering
//  ("Ordering (S24 AC2)": unread-first newest-first, then
//  read-with-traffic newest-first, then quiet members by presence
//  freshness, CREW always first among equals).
//
//  Slice B's `InboxBridge` (`FireflyModel/Bridge/InboxBridge.swift`) is
//  what this view model is SPECIFIED to depend on (A01's Slices table),
//  projecting `ff_inbox_build`/`ff_inbox_thread_build` into plain Swift
//  values the same way `RadarBridge`/`CrewStore` do for their modules —
//  "C types never leave the bridge." Six slices build in parallel worktrees
//  (A01, "Slices"), and slice B has not landed in THIS one, so
//  `InboxViewModel` depends on `InboxProviding` — a protocol existential,
//  per this app's own MVVM convention #2 ("takes its dependencies as
//  protocol existentials in init, stores no concrete service type") —
//  rather than on a concrete `InboxBridge` that does not exist here yet.
//  `InMemoryInboxStore` below is the M1 stand-in, written to the exact
//  membership/ordering rules `ff_inbox.h` documents so swapping it for
//  the real bridge later is a wiring change, not a behavior change; every
//  ordering/membership rule it encodes is pinned by `InboxViewModelTests`
//  against those exact documented rules, not invented independently.
//
import FireflyCore
import FireflyMesh
import Foundation
import Observation

// `ConversationKind` (CREW vs. one member's 1:1 thread) now comes from
// slice B's `Bridge/InboxBridge.swift` — B has landed in this tree, so
// the stand-in declared here (same two cases, `Sendable, Hashable`) is
// gone; it collided with the bridge's own `ConversationKind` (which
// additionally carries `ffKind`/`nodeID` for the real C bridging) as a
// duplicate top-level type in this module. No call site below changes:
// `.crew`/`.member(_:)` construct and pattern-match identically either
// way.

/// The Firefly-protocol message kinds a feed item can carry — mirrors
/// `ff_feed_kind_t` (`firmware/core/include/ff_feed.h`) minus the
/// permanently-retired PULSE slot (`S04-firefly-protocol.md`'s
/// Amendments: "the device drops the notion of a pulse entirely").
public enum MessageKind: String, Sendable, Equatable, CaseIterable {
    case text
    case rally
    case status
    case flare
}

/// The item's direction fact — mirrors `ff_feed_dir_t` (`ff_feed.h`)
/// exactly, UNKNOWN included: "a push site records only what it
/// actually knows... never guessed."
public enum MessageDirection: Sendable, Equatable {
    case unknown
    case broadcast
    case direct
    case out
}

/// The Inbox row's honest presence tag, from the HEARD axis —
/// `ff_crew_presence_t` (`firmware/core/include/ff_crew.h`): ANY packet
/// heard, gated on nothing but a receive (the 2026-09-07
/// presence-heard-vs-position amendment `S24-signals-inbox.md` and
/// `S02-core-crew.md` both cross-reference — NOT position freshness).
/// `LINKED` is this app's spelling of `FF_CREW_PRESENCE_NEVER` — paired,
/// no packet ever heard, so there is no honest age to show
/// (`ff_sigview.h`'s own `FF_PRESENCE_LINKED`, which this collapses
/// `.heard`/`.stale` into as `SEEN` — see `PresenceTag.sigviewEquivalent`
/// and `InboxViewModelTests` for the cross-check against the real
/// `ff_sigview_presence` C function).
public enum PresenceTag: String, Sendable, Equatable, CaseIterable {
    case heard = "HEARD"
    case stale = "STALE"
    case lost = "LOST"
    case linked = "LINKED"

    /// `FF_CREW_HEARD_LIVE_MS` (`ff_crew.h`) — below this, HEARD.
    public static let heardLiveMS: UInt32 = 120_000
    /// `FF_CREW_HEARD_LOST_MS` (`ff_crew.h`) — at or below this (the
    /// header's own "inclusive-toward-STALE" boundary), STALE; above
    /// it, LOST.
    public static let heardLostMS: UInt32 = 600_000

    /// Classify from the same two thresholds `ff_crew_presence`
    /// (`ff_crew.h`) uses. Not called through the C bridge: that
    /// function takes a live `ff_crew_member_t*` owned by
    /// `Bridge/CrewStore.swift` (slice B), which this slice does not
    /// touch — this is the pure boundary-comparison half, transcribed
    /// the same way `FireflyTheme` transcribes `ff_theme.h`'s palette,
    /// and cross-checked against the real `ff_sigview_presence` C
    /// function by `InboxViewModelTests`
    /// (`testPresenceTagAgreesWithFfSigviewPresence`).
    public static func classify(everHeard: Bool, heardAgeMS: UInt32) -> PresenceTag {
        guard everHeard else { return .linked }
        if heardAgeMS < heardLiveMS { return .heard }
        if heardAgeMS <= heardLostMS { return .stale }
        return .lost
    }

    /// The puck's own three-word vocabulary (`ff_sigview_presence_t`,
    /// `ff_sigview.h`) this tag collapses to — `.heard`/`.stale` -> SEEN,
    /// `.lost` -> LOST, `.linked` -> LINKED. The phone's row keeps the
    /// finer HEARD/STALE split (this app's own product choice, not a
    /// puck-parity requirement — A01 is explicit that the app is not
    /// obligated to mirror every puck screen decision); this computed
    /// property is what proves the split stays a strict refinement of,
    /// never a disagreement with, the shared core vocabulary.
    public var ffSigviewPresence: ff_sigview_presence_t {
        switch self {
        case .heard, .stale: return FF_PRESENCE_SEEN
        case .lost: return FF_PRESENCE_LOST
        case .linked: return FF_PRESENCE_LINKED
        }
    }
}

/// One feed item, ready to render as a conversation-list preview or a
/// thread bubble. Mirrors `ff_feed_item_t`/`ff_inbox_msg_t`
/// (`ff_feed.h`, `ff_inbox.h`) field for field; `id` is this app's
/// equivalent of `ff_feed_item_t.outbox_id` — assigned at push time,
/// before a packet id can exist, so the outbox/delivery-state setters
/// below can find this exact item again regardless of feed ordering.
public struct FeedMessage: Sendable, Equatable, Identifiable {
    public let id: UInt64
    public var kind: MessageKind
    public var direction: MessageDirection
    public var senderID: UInt32?
    public var senderName: String?
    public var text: String
    public var timestamp: Date
    public var unread: Bool
    /// FLARE's duration payload (`S04-firefly-protocol.md`: `[dur_s:2]`,
    /// default 300). `nil` for every other kind.
    public var flareDurationSeconds: UInt16?
    /// Destination for an OUT item: `meshBroadcastAddress` for a
    /// whole-crew send (`ff_feed_item_t.to_node == 0`'s convention,
    /// re-expressed with this app's own broadcast constant), a node id
    /// for a 1:1 send. Meaningless for inbound items.
    public var destination: UInt32?
    /// The packet id the radio assigned once this item reaches SENT.
    /// Also the echo-dedup key for INBOUND items: the mesh reflects a
    /// self-originated broadcast back with the SAME packet id (A01,
    /// "Routing ACK -> delivery state": "Inbound text is deduplicated on
    /// packet.id... without the guard your own sent row is overwritten
    /// and a phantom notification fires").
    public var packetID: UInt32?
    /// `nil` == `FF_SEND_NONE` — every inbound item, honestly claiming no
    /// delivery fact. Meaningful only when `direction == .out`.
    public var deliveryState: DeliveryState?
    /// Clock of the most recent `deliveryState` transition — what
    /// `ThreadViewModel.renderedDeliveryState(for:now:)`'s render-time
    /// no-ack window measures from.
    public var statusAt: Date

    public init(id: UInt64, kind: MessageKind, direction: MessageDirection, senderID: UInt32? = nil,
                senderName: String? = nil, text: String, timestamp: Date, unread: Bool = false,
                flareDurationSeconds: UInt16? = nil, destination: UInt32? = nil, packetID: UInt32? = nil,
                deliveryState: DeliveryState? = nil, statusAt: Date? = nil) {
        self.id = id
        self.kind = kind
        self.direction = direction
        self.senderID = senderID
        self.senderName = senderName
        self.text = text
        self.timestamp = timestamp
        self.unread = unread
        self.flareDurationSeconds = flareDurationSeconds
        self.destination = destination
        self.packetID = packetID
        self.deliveryState = deliveryState
        self.statusAt = statusAt ?? timestamp
    }
}

/// One conversation row, ready to render — mirrors `ff_inbox_conv_t`
/// (`ff_inbox.h`). Fields not relevant to the row's kind are `nil`
/// (CREW has no presence; a traffic-less row has no preview) — the same
/// "not relevant, zeroed" convention the C struct's own doc comment
/// states.
public struct InboxConversationRow: Sendable, Equatable, Identifiable {
    public var id: ConversationKind { kind }
    public let kind: ConversationKind
    public var displayName: String
    public var initial: Character?
    public var colorIndex: Int?

    public var unreadCount: Int
    public var itemCount: Int

    public var hasPreview: Bool
    public var previewKind: MessageKind?
    public var previewDirection: MessageDirection?
    public var previewText: String
    /// Seconds since the newest item, computed at render/build time —
    /// never a stored, staling number.
    public var previewAge: TimeInterval?
    public var previewFromName: String?
    /// This app's own addition, not in `ff_inbox_conv_t` (S24's Honest
    /// section explicitly scopes per-message status OUT of the puck's
    /// conversation-list preview — "a larger core change judged not
    /// worth it for a summary line"). The task for this slice asks for
    /// an outgoing status tag on the Inbox row, so this is a disclosed,
    /// deliberate product divergence for the phone, not an oversight.
    public var previewDeliveryState: DeliveryState?

    /// `nil` for CREW. `PresenceTag`/age for a member row.
    public var presence: PresenceTag?
    public var presenceAge: TimeInterval?

    public init(kind: ConversationKind, displayName: String, initial: Character? = nil, colorIndex: Int? = nil,
                unreadCount: Int = 0, itemCount: Int = 0, hasPreview: Bool = false, previewKind: MessageKind? = nil,
                previewDirection: MessageDirection? = nil, previewText: String = "", previewAge: TimeInterval? = nil,
                previewFromName: String? = nil, previewDeliveryState: DeliveryState? = nil,
                presence: PresenceTag? = nil, presenceAge: TimeInterval? = nil) {
        self.kind = kind
        self.displayName = displayName
        self.initial = initial
        self.colorIndex = colorIndex
        self.unreadCount = unreadCount
        self.itemCount = itemCount
        self.hasPreview = hasPreview
        self.previewKind = previewKind
        self.previewDirection = previewDirection
        self.previewText = previewText
        self.previewAge = previewAge
        self.previewFromName = previewFromName
        self.previewDeliveryState = previewDeliveryState
        self.presence = presence
        self.presenceAge = presenceAge
    }
}

/// What `InboxViewModel`/`ThreadViewModel` need from wherever
/// conversations and their messages live. In the merged product this is
/// `InboxBridge` (slice B); see this file's header comment. Every
/// mutating method mirrors one `ff_feed`/`ff_inbox` C entry point by
/// name in its doc comment, so a future `InboxBridge` conformance is a
/// direct translation, not a redesign.
public protocol InboxProviding: AnyObject, Sendable {
    /// Mirrors `ff_inbox_build` + reading every `ff_inbox_conv_at` row —
    /// the full ordered conversation list as of `now`. CREW is always
    /// present.
    func conversations(now: Date) -> [InboxConversationRow]
    /// Mirrors `ff_inbox_thread_build` + reading every
    /// `ff_inbox_thread_at` row — one conversation's messages, oldest
    /// first.
    func thread(for conversation: ConversationKind, now: Date) -> [FeedMessage]
    /// Mirrors `ff_inbox_mark_thread_read`. Returns the count newly
    /// marked read.
    @discardableResult
    func markRead(_ conversation: ConversationKind) -> Int

    /// Mirrors `ff_feed_push`, with an echo-dedup-by-packet-id guard
    /// scoped to MY OWN sent packet ids only (A01, "Routing ACK ->
    /// delivery state") — never a global "every id ever seen" set; see
    /// `InMemoryInboxStore.mySentPacketIDs`'s doc comment for why.
    func push(_ message: FeedMessage, into conversation: ConversationKind)
    /// Mirrors `ff_feed_mark_sent_by_outbox_id`: the WAITING -> SENT
    /// transition, stamping the packet id the radio assigned.
    func markSent(outboxID: UInt64, packetID: UInt32, at: Date)
    /// Mirrors `ff_feed_set_send_status_by_outbox_id` — any other
    /// transition addressed by outbox id (used for the bounded-outbox
    /// DROPPED transition, which has no packet id yet).
    func setStatus(outboxID: UInt64, state: DeliveryState, at: Date)
    /// Mirrors `ff_feed_set_ack_by_packet_id` — the routing-ack answer,
    /// addressed by packet id (the client's `deliveryUpdates()`
    /// correlation key).
    func setStatus(packetID: UInt32, state: DeliveryState, at: Date)
}

/// A monotonic id source for INBOUND `FeedMessage`s
/// (`InboxViewModel.ingest(_:)`), disjoint BY CONSTRUCTION from
/// `ThreadViewModel.swift`'s own `OutboxIDGenerator`. Both mint into the
/// same `FeedMessage.id: UInt64` space `InboxProviding.markSent`/
/// `setStatus(outboxID:)` key their `mutateLocked(outboxID:)` lookups
/// on; `OutboxIDGenerator` counts up from 1 with no reserved range, so
/// an inbound id drawn from that same low range could alias a real
/// outbox id and let a routing-ack-driven mutation silently land on the
/// wrong row. Reserving the top bit for every INBOUND id keeps the two
/// generators' output disjoint for as long as either could plausibly
/// run, not merely "in practice today".
private final class InboundFeedIDGenerator: @unchecked Sendable {
    static let shared = InboundFeedIDGenerator()
    private let lock = NSLock()
    private var counter: UInt64 = 0x8000_0000_0000_0000
    func next() -> UInt64 {
        lock.lock(); defer { lock.unlock() }
        let value = counter
        counter &+= 1
        return value
    }
}

/// A fixed-size FIFO of the most recent 64 packet ids — `InMemoryInboxStore`'s
/// echo-dedup memory (see `mySentPacketIDs`'s doc comment). Insertion
/// order determines eviction: the OLDEST id falls out once a 65th is
/// inserted, exactly the "ring of the last 64" the review asked for,
/// never an unbounded set.
private struct SentIDRing {
    static let capacity = 64
    private var order: [UInt32] = []
    private var members: Set<UInt32> = []

    mutating func insert(_ id: UInt32) {
        guard !members.contains(id) else { return }
        order.append(id)
        members.insert(id)
        if order.count > Self.capacity {
            members.remove(order.removeFirst())
        }
    }

    func contains(_ id: UInt32) -> Bool { members.contains(id) }
}

/// The M1 stand-in for `InboxBridge` — see this file's header comment.
/// Honest in the same way `StubMeshtasticClient` is honest: it invents
/// no traffic, no members and no presence. Everything it renders was put
/// there by `push`/`registerMember`/`setPresence`, or by `ThreadViewModel`
/// acting on a real (or injected) `MeshtasticClientProtocol`.
public final class InMemoryInboxStore: InboxProviding, @unchecked Sendable {
    private struct Member {
        var displayName: String
        var initial: Character
        var colorIndex: Int
        var presence: PresenceTag = .linked
        var presenceAgeMS: UInt32?
    }

    private let lock = NSLock()
    private var members: [UInt32: Member] = [:]
    private var messagesByConversation: [ConversationKind: [FeedMessage]] = [.crew: []]
    /// The echo-dedup guard's memory — bounded to MY OWN sent packet
    /// ids only (the ids `markSent` recorded, i.e. exactly what the
    /// client returned for MY sends), never a global "every id ever
    /// seen" set. Meshtastic packet ids are 32-bit values generated
    /// independently per device; over a multi-day festival with
    /// several paired members, two different senders reusing the same
    /// id is a real, non-adversarial birthday-collision risk, not just
    /// a hypothetical one — a global set would silently and
    /// permanently eat one of their messages (BLOCKING review item 1
    /// on this PR). Scoping to "ids I myself sent" means only a TRUE
    /// echo of my own broadcast is ever dropped; an unrelated sender's
    /// message that happens to reuse an id I never sent is always
    /// kept. `SentIDRing.capacity` bounds it to the most recent 64
    /// sends so this memory never grows unbounded either.
    private var mySentPacketIDs = SentIDRing()

    public init() {}

    /// Registers a paired crew member so their conversation row exists
    /// even with zero traffic (`ff_inbox.h`: "A conversation exists only
    /// for CREW and for PAIRED members").
    public func registerMember(_ id: UInt32, displayName: String, initial: Character, colorIndex: Int) {
        lock.lock(); defer { lock.unlock() }
        members[id] = Member(displayName: displayName, initial: initial, colorIndex: colorIndex)
        if messagesByConversation[.member(id)] == nil { messagesByConversation[.member(id)] = [] }
    }

    public func unregisterMember(_ id: UInt32) {
        lock.lock(); defer { lock.unlock() }
        members.removeValue(forKey: id)
        messagesByConversation.removeValue(forKey: .member(id))
    }

    /// Sets a member's honest presence tag — where this comes from in
    /// the merged product is slice B's `CrewStore` (not a slice E
    /// dependency per A01's Slices table); here, tests and app wiring
    /// call it directly.
    public func setPresence(_ tag: PresenceTag, ageMS: UInt32?, for member: UInt32) {
        lock.lock(); defer { lock.unlock() }
        members[member]?.presence = tag
        members[member]?.presenceAgeMS = ageMS
    }

    /// NIT 9 on this PR — membership note: unlike `ff_inbox_build`, which
    /// re-derives conversation membership from the WHOLE feed's own
    /// `dir`/`from_node`/`to_node` fields on every build, this stand-in
    /// takes `conversation` as an explicit destination from the caller.
    /// Harmless here since every call site already knows the right
    /// conversation, but a structurally different shape from the real
    /// `InboxBridge` (slice B) — re-verify the ordering/membership tests
    /// once that swap happens.
    public func push(_ message: FeedMessage, into conversation: ConversationKind) {
        lock.lock(); defer { lock.unlock() }
        // Echo-dedup: only an id THIS store's own `markSent` recorded —
        // never a global "every id ever seen" set. See
        // `mySentPacketIDs`'s doc comment.
        if message.direction != .out, let packetID = message.packetID, mySentPacketIDs.contains(packetID) {
            return // an echo of one of MY OWN sent packet ids — dropped before it ever reaches the feed
        }
        messagesByConversation[conversation, default: []].append(message)
    }

    public func markSent(outboxID: UInt64, packetID: UInt32, at: Date) {
        lock.lock(); defer { lock.unlock() }
        // The ONLY place a packet id enters the echo-dedup memory: the
        // moment the radio hands back an id for something I myself sent.
        mySentPacketIDs.insert(packetID)
        mutateLocked(outboxID: outboxID) {
            $0.packetID = packetID
            $0.deliveryState = .sent
            $0.statusAt = at
        }
    }

    public func setStatus(outboxID: UInt64, state: DeliveryState, at: Date) {
        lock.lock(); defer { lock.unlock() }
        mutateLocked(outboxID: outboxID) { $0.deliveryState = state; $0.statusAt = at }
    }

    public func setStatus(packetID: UInt32, state: DeliveryState, at: Date) {
        lock.lock(); defer { lock.unlock() }
        mutateLocked(packetID: packetID) { $0.deliveryState = state; $0.statusAt = at }
    }

    // Not found is a safe, silent no-op — the item may already have
    // scrolled out, the same honest default `ff_feed_set_ack_by_packet_id`
    // documents.
    private func mutateLocked(outboxID: UInt64, _ body: (inout FeedMessage) -> Void) {
        for (key, var msgs) in messagesByConversation {
            if let idx = msgs.firstIndex(where: { $0.id == outboxID }) {
                body(&msgs[idx])
                messagesByConversation[key] = msgs
                return
            }
        }
    }

    private func mutateLocked(packetID: UInt32, _ body: (inout FeedMessage) -> Void) {
        for (key, var msgs) in messagesByConversation {
            if let idx = msgs.firstIndex(where: { $0.packetID == packetID }) {
                body(&msgs[idx])
                messagesByConversation[key] = msgs
                return
            }
        }
    }

    public func thread(for conversation: ConversationKind, now: Date) -> [FeedMessage] {
        lock.lock(); defer { lock.unlock() }
        return (messagesByConversation[conversation] ?? []).sorted { $0.timestamp < $1.timestamp }
    }

    @discardableResult
    public func markRead(_ conversation: ConversationKind) -> Int {
        lock.lock(); defer { lock.unlock() }
        guard var msgs = messagesByConversation[conversation] else { return 0 }
        var count = 0
        for i in msgs.indices where msgs[i].unread {
            msgs[i].unread = false
            count += 1
        }
        messagesByConversation[conversation] = msgs
        return count
    }

    public func conversations(now: Date) -> [InboxConversationRow] {
        lock.lock(); defer { lock.unlock() }
        var rows: [InboxConversationRow] = [row(for: .crew, now: now)]
        for id in members.keys.sorted() { rows.append(row(for: .member(id), now: now)) }
        return Self.order(rows)
    }

    private func row(for kind: ConversationKind, now: Date) -> InboxConversationRow {
        let msgs = (messagesByConversation[kind] ?? []).sorted { $0.timestamp < $1.timestamp }
        let newest = msgs.last
        var displayName = "CREW"
        var initial: Character?
        var colorIndex: Int?
        var presence: PresenceTag?
        var presenceAge: TimeInterval?
        if case .member(let id) = kind, let member = members[id] {
            displayName = member.displayName
            initial = member.initial
            colorIndex = member.colorIndex
            presence = member.presence
            presenceAge = member.presence == .linked ? nil : member.presenceAgeMS.map { TimeInterval($0) / 1000 }
        }
        return InboxConversationRow(
            kind: kind,
            displayName: displayName,
            initial: initial,
            colorIndex: colorIndex,
            unreadCount: msgs.filter(\.unread).count,
            itemCount: msgs.count,
            hasPreview: newest != nil,
            previewKind: newest?.kind,
            previewDirection: newest?.direction,
            previewText: newest?.text ?? "",
            previewAge: newest.map { now.timeIntervalSince($0.timestamp) },
            previewFromName: newest.flatMap { $0.direction == .out ? nil : $0.senderName },
            previewDeliveryState: (newest?.direction == .out) ? newest?.deliveryState : nil,
            presence: presence,
            presenceAge: presenceAge)
    }

    /// `ff_inbox.h`'s "Ordering (S24 AC2)" rule, transcribed:
    /// 1. conversations with unread items — newest traffic first;
    /// 2. read conversations that have traffic — newest traffic first;
    /// 3. quiet conversations: CREW first, then members by presence
    ///    freshness (freshest first, LINKED last), ties by ascending
    ///    node id. Ties on traffic age (groups 1/2) break the same way:
    ///    CREW first, then ascending node id.
    static func order(_ rows: [InboxConversationRow]) -> [InboxConversationRow] {
        func group(_ r: InboxConversationRow) -> Int {
            if r.unreadCount > 0 { return 0 }
            if r.itemCount > 0 { return 1 }
            return 2
        }
        func nodeID(_ r: InboxConversationRow) -> Int64 {
            if case .member(let id) = r.kind { return Int64(id) }
            return -1 // CREW sorts first among ties
        }
        func presenceRank(_ r: InboxConversationRow) -> Int {
            switch r.presence {
            case .heard: return 0
            case .stale: return 1
            case .lost: return 2
            case .linked, .none: return 3
            }
        }
        return rows.sorted { a, b in
            let ga = group(a), gb = group(b)
            if ga != gb { return ga < gb }
            switch ga {
            case 0, 1:
                let ageA = a.previewAge ?? .greatestFiniteMagnitude
                let ageB = b.previewAge ?? .greatestFiniteMagnitude
                if ageA != ageB { return ageA < ageB } // newer (smaller age) first
                return nodeID(a) < nodeID(b)
            default:
                // CREW is always the quiet group's anchor row, ranked
                // ahead of every member regardless of presence.
                let crewA = (a.kind == .crew), crewB = (b.kind == .crew)
                if crewA != crewB { return crewA }
                let pa = presenceRank(a), pb = presenceRank(b)
                if pa != pb { return pa < pb }
                let ageA = a.presenceAge ?? .greatestFiniteMagnitude
                let ageB = b.presenceAge ?? .greatestFiniteMagnitude
                if ageA != ageB { return ageA < ageB }
                return nodeID(a) < nodeID(b)
            }
        }
    }
}

/// Preview-text truncation for the conversation-list row — a one-line
/// summary, never a full message. Truncates by `Character`
/// (extended-grapheme-cluster) count, which is inherently UTF-8-safe:
/// `String` never lets you slice mid-scalar or mid-cluster this way, so
/// a flag, a ZWJ family emoji, or a combining accent at the boundary is
/// either kept whole or dropped whole, never mangled into an invalid or
/// replacement-character byte sequence. `ff_feed_item_t.text`'s own
/// `FF_FEED_TEXT_LEN` (64 *bytes*) is a separate, wire-level concern this
/// presentation-layer helper does not reproduce.
public enum InboxText {
    public static func preview(_ text: String, maxLength: Int = 42) -> String {
        guard text.count > maxLength else { return text }
        return String(text.prefix(maxLength)) + "…"
    }
}

/// The Inbox screen's state: the ordered conversation list, kept live by
/// this view model's OWN `deliveryUpdates()` subscription (A01's Slice E
/// entry: "a fresh EventHub subscription, S1 — this view model's own,
/// independent of CoreStore's").
@MainActor
@Observable
public final class InboxViewModel {
    public private(set) var conversations: [InboxConversationRow] = []

    private let provider: any InboxProviding
    private let client: any MeshtasticClientProtocol
    /// Handed straight through to every `ThreadViewModel` this view
    /// model opens. `nil` — today's default live wiring — means FLARE
    /// renders disabled everywhere (`ThreadViewModel.flareAvailable`);
    /// see `ThreadViewModel.swift`'s `FireflyPacketSending` doc comment.
    private let flareSender: (any FireflyPacketSending)?
    private var deliveryObservation: Task<Void, Never>?
    /// PR #264 review, BLOCKING item 2's cross-slice wiring: the one
    /// place a decoded `IncomingText` (`MeshtasticClientProtocol`,
    /// slice A) becomes a `FeedMessage` this screen renders (slice E).
    /// A minimal, explicit integration edit — see this PR's own comment
    /// for why it lives here rather than in `CoreStore`: nothing in this
    /// tree currently wires `CoreStore.inbox` (the C-core `InboxBridge`)
    /// to the live UI at all — `AppDependencies`/`FireflyApp` construct
    /// this view model against `InMemoryInboxStore` directly — so this
    /// is the one place an inbound message can reach the screen today.
    private var incomingTextObservation: Task<Void, Never>?

    public init(provider: any InboxProviding, client: any MeshtasticClientProtocol,
                flareSender: (any FireflyPacketSending)? = nil) {
        self.provider = provider
        self.client = client
        self.flareSender = flareSender
    }

    /// Idempotent, like every other view model's `observe()`.
    public func observe() {
        guard deliveryObservation == nil else { return }
        let deliveries = client.deliveryUpdates()
        let incomingTexts = client.incomingTexts()
        deliveryObservation = Task { [weak self] in
            for await event in deliveries {
                guard let self else { return }
                // Same reasoning as `ThreadViewModel.observe()`: `.waiting`/
                // `.sent`/`.dropped` key off the CLIENT's own `OutboxID`, an
                // id space this view model never tracks (sends go through
                // `ThreadViewModel`, which marks SENT/DROPPED itself); only
                // `.delivered`/`.noAck` key off `packetID`, which the
                // provider already indexes (`setStatus(packetID:...)`), so
                // those are the only two cases forwarded here.
                switch event {
                case .delivered(let packetID):
                    self.provider.setStatus(packetID: packetID.rawValue, state: .delivered, at: Date())
                    self.refresh()
                case .noAck(let packetID):
                    self.provider.setStatus(packetID: packetID.rawValue, state: .noAck, at: Date())
                    self.refresh()
                case .waiting, .sent, .dropped:
                    break
                }
            }
        }
        incomingTextObservation = Task { [weak self] in
            for await incoming in incomingTexts {
                guard let self else { return }
                self.ingest(incoming)
            }
        }
        refresh()
    }

    /// Not a `deinit` — this type is `@MainActor`, same rule as
    /// `ConnectViewModel.stopObserving()`.
    public func stopObserving() {
        deliveryObservation?.cancel()
        deliveryObservation = nil
        incomingTextObservation?.cancel()
        incomingTextObservation = nil
    }

    /// Routes one decoded `IncomingText` into the provider as a
    /// `FeedMessage`. Echo-dedup (dropping a self-originated broadcast
    /// reflected back with my own packet id) is NOT done here —
    /// `InMemoryInboxStore.push` already guards on `mySentPacketIDs`
    /// (that method's own doc comment); this is purely routing +
    /// construction, same division of labor `ThreadViewModel.send`
    /// keeps between "decide what to push" and "the provider's own
    /// invariants about what it accepts".
    private func ingest(_ incoming: IncomingText) {
        let isBroadcast = incoming.to == meshBroadcastAddress
        let conversation: ConversationKind = isBroadcast ? .crew : .member(incoming.from)
        let message = FeedMessage(
            id: InboundFeedIDGenerator.shared.next(),
            kind: .text,
            direction: isBroadcast ? .broadcast : .direct,
            senderID: incoming.from,
            text: incoming.text,
            timestamp: incoming.rxTime ?? Date(),
            unread: true,
            packetID: incoming.packetID)
        provider.push(message, into: conversation)
        refresh()
    }

    public func refresh(now: Date = Date()) {
        conversations = provider.conversations(now: now)
    }

    /// Open a conversation's thread: marks it read (S24: "per-thread
    /// mark-read on open") and hands back a fresh `ThreadViewModel`
    /// bound to the SAME provider and client, so a send from the thread
    /// is reflected here on the next `refresh()`.
    public func openThread(_ conversation: ConversationKind) -> ThreadViewModel {
        provider.markRead(conversation)
        refresh()
        return ThreadViewModel(conversation: conversation, provider: provider, client: client, flareSender: flareSender)
    }
}
