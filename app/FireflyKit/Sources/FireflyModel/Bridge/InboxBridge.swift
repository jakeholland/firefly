//
//  InboxBridge.swift — the Swift-safe wrapper over
//  `firmware/core/ff_feed` + `ff_inbox` (docs/specs/A01-companion-app.md,
//  slice B).
//
//  Heap-owns one `ff_feed_t` (the event ring buffer) — the persistent
//  context. `ff_inbox_t`/`ff_inbox_thread_t` are NOT persistent: nothing
//  in `ff_inbox_build`/`ff_inbox_thread_build` stores those pointers
//  past the one call, so they are local, stack-allocated scratch
//  structs per `conversations()`/`thread()` call, decoded and discarded
//  — the "stack or heap struct the bridge owns for the duration of one
//  build call" the spec's "Memory ownership" section describes.
//
//  Deliberately does NOT import FireflyMesh (docs/specs/A01-companion-app.md,
//  slice B's "Depends on" — Bridge/* takes plain values so it is
//  testable with no client at all): `FeedSendStatus` below is this
//  module's OWN honest mirror of `ff_feed_send_status_t`, including the
//  `.none` case FireflyMesh's `DeliveryState` deliberately excludes (see
//  DeliveryState.swift's own doc comment) — `CoreStore`, which DOES
//  import both, is where the two vocabularies meet.
//
import FireflyCore
import Foundation

/// `ff_feed_kind_t`.
public enum FeedKind: Sendable, Equatable, CaseIterable {
    case text, rally, status, flare

    init(ffKind: ff_feed_kind_t) {
        switch ffKind {
        case FEED_RALLY: self = .rally
        case FEED_STATUS: self = .status
        case FEED_FLARE: self = .flare
        default: self = .text
        }
    }

    var ffValue: ff_feed_kind_t {
        switch self {
        case .text: return FEED_TEXT
        case .rally: return FEED_RALLY
        case .status: return FEED_STATUS
        case .flare: return FEED_FLARE
        }
    }
}

/// `ff_feed_dir_t` — `.unknown` is the wire zero value on purpose (a
/// zero-initialized/legacy item is honestly "direction not recorded",
/// never accidentally "broadcast").
public enum FeedDirection: Sendable, Equatable, CaseIterable {
    case unknown, broadcast, direct, out

    init(ffDir: ff_feed_dir_t) {
        switch ffDir {
        case FEED_DIR_BROADCAST: self = .broadcast
        case FEED_DIR_DIRECT: self = .direct
        case FEED_DIR_OUT: self = .out
        default: self = .unknown
        }
    }

    var ffValue: ff_feed_dir_t {
        switch self {
        case .unknown: return FEED_DIR_UNKNOWN
        case .broadcast: return FEED_DIR_BROADCAST
        case .direct: return FEED_DIR_DIRECT
        case .out: return FEED_DIR_OUT
        }
    }
}

/// `ff_feed_send_status_t`, ALL SIX values — unlike `FireflyMesh`'s
/// `DeliveryState` (which represents the C enum's zero value, `NONE`,
/// as Swift `nil` because it means "not an outbound item at all"), this
/// bridge-level enum keeps `.none` as an explicit case: it is a real
/// value `ff_feed.h` defines and this bridge reads back off every
/// inbound `ff_inbox_msg_t.send_status` verbatim, never silently
/// dropped.
public enum FeedSendStatus: Sendable, Equatable, CaseIterable {
    case none, waiting, sent, delivered, noAck, dropped

    init(ffStatus: ff_feed_send_status_t) {
        switch ffStatus {
        case FF_SEND_WAITING: self = .waiting
        case FF_SEND_SENT: self = .sent
        case FF_SEND_DELIVERED: self = .delivered
        case FF_SEND_NO_ACK: self = .noAck
        case FF_SEND_DROPPED: self = .dropped
        default: self = .none
        }
    }

    var ffValue: ff_feed_send_status_t {
        switch self {
        case .none: return FF_SEND_NONE
        case .waiting: return FF_SEND_WAITING
        case .sent: return FF_SEND_SENT
        case .delivered: return FF_SEND_DELIVERED
        case .noAck: return FF_SEND_NO_ACK
        case .dropped: return FF_SEND_DROPPED
        }
    }
}

/// `ff_conv_kind_t` + its `node_id` key, folded into one Swift value so
/// "which conversation" is never a (kind, id) pair that could disagree
/// with itself (`.crew`'s id is always 0, matching `FF_CONV_CREW`'s own
/// zero-value convention).
public enum ConversationKind: Sendable, Equatable, Hashable {
    case crew
    case member(UInt32)

    var ffKind: ff_conv_kind_t {
        switch self {
        case .crew: return FF_CONV_CREW
        case .member: return FF_CONV_MEMBER
        }
    }

    var nodeID: UInt32 {
        switch self {
        case .crew: return 0
        case .member(let id): return id
        }
    }
}

/// `ff_sigview_presence_t` — reused by `ff_inbox` for a conversation's
/// presence field, never reimplemented (ff_sigview.h's own top comment).
public enum SigviewPresence: Sendable, Equatable, CaseIterable {
    case seen, lost, linked

    init(ffPresence: ff_sigview_presence_t) {
        switch ffPresence {
        case FF_PRESENCE_SEEN: self = .seen
        case FF_PRESENCE_LOST: self = .lost
        default: self = .linked
        }
    }
}

/// What a caller pushes into the feed. `atMs` is the caller's own clock
/// reading at receipt/creation — never re-read "now" for an event that
/// already happened.
public struct FeedItem: Sendable, Equatable {
    public var kind: FeedKind
    public var fromNode: UInt32
    public var atMs: UInt32
    public var text: String
    public var direction: FeedDirection
    /// Meaningful iff `direction == .out`: the destination, or 0 for a
    /// whole-crew broadcast.
    public var toNode: UInt32
    /// Meaningful iff `direction == .out`: the shell-assigned identity
    /// of this send, stamped BEFORE a packet id can exist — the only
    /// way `setSendStatus`/`markSent`/`setAck` can find their way back
    /// to this exact item later (ff_feed.h's own doc comment). `0` = not
    /// tracked (0 never matches — the documented sentinel).
    public var outboxID: UInt32

    public init(kind: FeedKind, fromNode: UInt32 = 0, atMs: UInt32, text: String = "",
                direction: FeedDirection = .unknown, toNode: UInt32 = 0, outboxID: UInt32 = 0) {
        self.kind = kind
        self.fromNode = fromNode
        self.atMs = atMs
        self.text = text
        self.direction = direction
        self.toNode = toNode
        self.outboxID = outboxID
    }
}

/// One conversation row (`ff_inbox_conv_t`), ready to render.
public struct InboxConversation: Sendable, Equatable, Identifiable {
    public var id: ConversationKind { kind }
    public let kind: ConversationKind
    public let name: String
    public let initial: Character?
    public let colorIndex: UInt8
    public let unread: UInt16
    public let itemCount: UInt8
    public let hasPreview: Bool
    public let previewKind: FeedKind
    public let previewDirection: FeedDirection
    public let previewText: String
    public let previewAgeMs: UInt32
    public let previewFromKnown: Bool
    public let previewFromName: String
    public let presenceValid: Bool
    public let presence: SigviewPresence
    public let presenceAgeMs: UInt32

    static func decode(_ c: ff_inbox_conv_t) -> InboxConversation {
        InboxConversation(
            kind: c.kind == FF_CONV_CREW ? .crew : .member(c.node_id),
            name: FixedCString.decode(c.name),
            initial: Character(ffInitial: c.initial),
            colorIndex: c.color_idx,
            unread: c.unread,
            itemCount: c.item_count,
            hasPreview: c.has_preview,
            previewKind: FeedKind(ffKind: c.preview_kind),
            previewDirection: FeedDirection(ffDir: c.preview_dir),
            previewText: FixedCString.decode(c.preview_text),
            previewAgeMs: c.preview_age_ms,
            previewFromKnown: c.preview_from_known,
            previewFromName: FixedCString.decode(c.preview_from_name),
            presenceValid: c.presence_valid,
            presence: SigviewPresence(ffPresence: c.presence),
            presenceAgeMs: c.presence_age_ms
        )
    }
}

/// One thread message (`ff_inbox_msg_t`), ready to render as a sided
/// bubble.
public struct InboxMessage: Sendable, Equatable {
    public let kind: FeedKind
    /// `.out` = my side; anything else = theirs.
    public let direction: FeedDirection
    public let identityKnown: Bool
    public let nodeID: UInt32
    public let name: String
    public let initial: Character?
    public let colorIndex: UInt8
    public let text: String
    public let ageMs: UInt32
    public let unread: Bool
    /// `.none` for anything but an outbound item.
    public let sendStatus: FeedSendStatus

    static func decode(_ m: ff_inbox_msg_t) -> InboxMessage {
        InboxMessage(
            kind: FeedKind(ffKind: m.kind),
            direction: FeedDirection(ffDir: m.dir),
            identityKnown: m.identity_known,
            nodeID: m.node_id,
            name: FixedCString.decode(m.name),
            initial: Character(ffInitial: m.initial),
            colorIndex: m.color_idx,
            text: FixedCString.decode(m.text),
            ageMs: m.age_ms,
            unread: m.unread,
            sendStatus: FeedSendStatus(ffStatus: m.send_status)
        )
    }
}

/// Heap-owns one `ff_feed_t`.
public final class InboxBridge {
    private let context: UnsafeMutablePointer<ff_feed_t>

    public init() {
        context = UnsafeMutablePointer<ff_feed_t>.allocate(capacity: 1)
        context.initialize(to: ff_feed_t())
        ff_feed_init(context)
    }

    deinit {
        context.deinitialize(count: 1)
        context.deallocate()
    }

    /// Internal-only: the live `ff_feed_t` this bridge owns — never
    /// exposed outside `FireflyModel`.
    var raw: UnsafeMutablePointer<ff_feed_t> { context }

    public func push(_ item: FeedItem, unread: Bool = true) {
        var raw = ff_feed_item_t()
        raw.kind = item.kind.ffValue
        raw.from_node = item.fromNode
        raw.at_ms = item.atMs
        FixedCString.encode(item.text, into: &raw.text)
        raw.unread = unread
        raw.dir = item.direction.ffValue
        raw.to_node = item.toNode
        raw.outbox_id = item.outboxID
        ff_feed_push(context, &raw)
    }

    public func markAllRead() { ff_feed_mark_all_read(context) }

    public var unreadCount: UInt16 { ff_feed_unread_count(context) }
    public var itemCount: Int { Int(ff_feed_count(context)) }

    /// The WAITING -> SENT transition: stamps `packetID`/`wantAck` too,
    /// so a later ack/timeout can find this item again.
    public func markSent(outboxID: UInt32, packetID: UInt32, wantAck: Bool, atMs: UInt32) {
        ff_feed_mark_sent_by_outbox_id(context, outboxID, packetID, wantAck, atMs)
    }

    public func setSendStatus(outboxID: UInt32, status: FeedSendStatus, atMs: UInt32) {
        ff_feed_set_send_status_by_outbox_id(context, outboxID, status.ffValue, atMs)
    }

    /// Returns `true` iff a matching WAITING/SENT item was found and
    /// resolved to DELIVERED (`ok`) or NO_ACK (`!ok`).
    @discardableResult
    public func setAck(packetID: UInt32, ok: Bool, atMs: UInt32) -> Bool {
        ff_feed_set_ack_by_packet_id(context, packetID, ok, atMs)
    }

    public func expirePendingAcks(now: UInt32, timeoutMs: UInt32) {
        ff_feed_expire_pending_acks(context, now, timeoutMs)
    }

    /// `ff_inbox_build` — the ordered conversation list, as of `now`.
    /// CREW is always present, even with no traffic and no crew.
    public func conversations(crew: CrewStore, now: UInt32) -> [InboxConversation] {
        var ib = ff_inbox_t()
        ff_inbox_build(&ib, context, crew.raw, now)
        let n = Int(ff_inbox_conv_count(&ib))
        guard n > 0 else { return [] }
        return (0..<n).compactMap { ff_inbox_conv_at(&ib, UInt8($0))?.pointee }.map(InboxConversation.decode)
    }

    /// `ff_inbox_thread_build` — one conversation's messages, oldest
    /// first.
    public func thread(_ kind: ConversationKind, crew: CrewStore, now: UInt32) -> [InboxMessage] {
        var t = ff_inbox_thread_t()
        ff_inbox_thread_build(&t, context, crew.raw, kind.ffKind, kind.nodeID, now)
        let n = Int(ff_inbox_thread_count(&t))
        guard n > 0 else { return [] }
        return (0..<n).compactMap { ff_inbox_thread_at(&t, UInt8($0))?.pointee }.map(InboxMessage.decode)
    }

    /// Clears the unread flag of ONLY this conversation's items. Returns
    /// the number newly marked read.
    @discardableResult
    public func markThreadRead(_ kind: ConversationKind) -> Int {
        Int(ff_inbox_mark_thread_read(context, kind.ffKind, kind.nodeID))
    }
}
