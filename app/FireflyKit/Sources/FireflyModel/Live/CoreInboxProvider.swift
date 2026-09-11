//
//  CoreInboxProvider.swift — `InboxProviding` over the REAL C core:
//  slice B's `InboxBridge` (`ff_feed_t` + `ff_inbox_build` /
//  `ff_inbox_thread_build`) and `CrewStore` (`ff_crew_t`), replacing
//  slice E's `InMemoryInboxStore` stand-in in the live graph.
//
//  This is the swap `InboxViewModel.swift`'s header comment promised:
//  "`InMemoryInboxStore` ... written to the exact membership/ordering
//  rules `ff_inbox.h` documents so swapping it for the real bridge later
//  is a wiring change, not a behavior change." What changes here is
//  WHERE those rules run — the same object code the puck runs, rather
//  than a Swift transcription of the header that could drift from it.
//  `InMemoryInboxStore` stays, TEST-ONLY: `InboxViewModelTests` pins the
//  transcription against the documented rules, and this type is what
//  pins the transcription against the C core (`CoreInboxProviderTests`).
//
//  The one thing this type owns beyond translation is the echo-dedup
//  guard, for the same reason `InMemoryInboxStore` owns it and the
//  client does not (`incomingTexts()`'s protocol doc comment): the push
//  site is the only place that knows which packet ids it minted itself.
//
import FireflyMesh
import Foundation

/// `InboxProviding` backed by `ff_feed`/`ff_inbox`/`ff_crew`.
///
/// `@MainActor`-confined like every other `ff_*` owner (A01, "Threading
/// model"). `InboxProviding` is declared `Sendable` because it was
/// written before a C-backed conformance existed, so this is
/// `@unchecked Sendable` with the confinement asserted at each entry
/// point rather than assumed — the same shape `CoreRadarComputing` uses.
@MainActor
public final class CoreInboxProvider: InboxProviding, @unchecked Sendable {
    private let inbox: InboxBridge
    private let crew: CrewStore
    private let now: @Sendable () -> Date

    /// MY OWN sent packet ids, most recent 64 — the echo-dedup memory,
    /// scoped exactly as `InMemoryInboxStore.mySentPacketIDs` documents
    /// (never a global "every id ever seen" set, which would eat an
    /// unrelated sender's colliding id).
    private var mySentPacketIDs = PacketIDRing()

    public init(inbox: InboxBridge, crew: CrewStore, now: @escaping @Sendable () -> Date = { Date() }) {
        self.inbox = inbox
        self.crew = crew
        self.now = now
    }

    // MARK: - Reads

    public nonisolated func conversations(now: Date) -> [InboxConversationRow] {
        MainActor.assumeIsolated {
            let nowMs = FireflyClock.millis(since: now)
            // ORDERING IS THE C CORE'S, not this type's: `ff_inbox_build`
            // already applies S24 AC2's rule (unread-first newest-first,
            // then read-with-traffic, then quiet members by presence
            // freshness, CREW first among equals), so the rows come back
            // in final order and are NOT re-sorted here. Re-sorting them
            // in Swift would be a second rulebook, which is the whole
            // thing linking the core in was meant to avoid.
            return inbox.conversations(crew: crew, now: nowMs).map { conversation in
                row(from: conversation, now: now, nowMs: nowMs)
            }
        }
    }

    private func row(from conversation: InboxConversation, now: Date, nowMs: UInt32) -> InboxConversationRow {
        // `ff_inbox_conv_t` carries no preview SEND STATUS — S24
        // explicitly scopes per-message status out of the puck's
        // conversation list, while `InboxConversationRow.previewDeliveryState`
        // is this app's own disclosed divergence (that property's own doc
        // comment). So it is read from the newest raw feed record for
        // this conversation: a keyed read of the same item the preview
        // came from, never a guess.
        let newest = inbox.records(in: conversation.kind).last
        let previewDeliveryState: DeliveryState? = {
            guard conversation.previewDirection == .out, let newest, newest.direction == .out else { return nil }
            return DeliveryState(ffSendStatus: newest.sendStatus.ffValue)
        }()

        let isCrew = (conversation.kind == .crew)
        return InboxConversationRow(
            kind: conversation.kind,
            // `ff_inbox_conv_t.name` is MEMBER-only and empty for CREW
            // (the header's own "Identity — MEMBER only"), so the CREW
            // row's label is the app's, exactly as it is in
            // `InMemoryInboxStore.row(for:now:)`.
            displayName: isCrew ? "CREW" : conversation.name,
            initial: isCrew ? nil : conversation.initial,
            colorIndex: isCrew ? nil : Int(conversation.colorIndex),
            unreadCount: Int(conversation.unread),
            itemCount: Int(conversation.itemCount),
            hasPreview: conversation.hasPreview,
            previewKind: conversation.hasPreview ? MessageKind(feedKind: conversation.previewKind) : nil,
            previewDirection: conversation.hasPreview
                ? MessageDirection(feedDirection: conversation.previewDirection) : nil,
            previewText: conversation.hasPreview ? conversation.previewText : "",
            previewAge: conversation.hasPreview ? TimeInterval(conversation.previewAgeMs) / 1000 : nil,
            // Empty unless the core itself joined a PAIRED sender —
            // `preview_from_known` is the core's own "never fabricated"
            // gate, carried through rather than second-guessed.
            previewFromName: conversation.previewFromKnown ? conversation.previewFromName : nil,
            previewDeliveryState: previewDeliveryState,
            // The TAG comes from `ff_crew_presence` (the four-value
            // HEARD/STALE/LOST/NEVER axis this app renders), not from the
            // conversation row's three-value `ff_sigview_presence` — that
            // one collapses HEARD and STALE into SEEN, and mapping SEEN
            // back would have to pick one of the two the core never
            // asserted. Same source, finer projection, no guessing.
            presence: conversation.presenceValid ? presenceTag(for: conversation.kind, nowMs: nowMs) : nil,
            // LINKED has no honest age (`ff_sigview.h`), so it gets none
            // here either — not a zero.
            presenceAge: (conversation.presenceValid && conversation.presence != .linked)
                ? TimeInterval(conversation.presenceAgeMs) / 1000 : nil)
    }

    /// `ff_crew_presence`'s four-value HEARD axis for a member row, read
    /// straight off the roster the core itself joined the row against.
    private func presenceTag(for kind: ConversationKind, nowMs: UInt32) -> PresenceTag? {
        guard case .member(let nodeID) = kind, let member = crew.member(nodeID: nodeID, now: nowMs) else {
            return nil
        }
        switch member.heardPresence {
        case .heard: return .heard
        case .stale: return .stale
        case .lost: return .lost
        case .never: return .linked
        }
    }

    public nonisolated func thread(for conversation: ConversationKind, now: Date) -> [FeedMessage] {
        MainActor.assumeIsolated {
            let nowMs = FireflyClock.millis(since: now)
            // Read from the RAW records rather than `ff_inbox_thread_build`'s
            // render projection: `ff_inbox_msg_t` deliberately carries no
            // `outbox_id`/`packet_id`/absolute timestamp, and `FeedMessage`
            // is keyed on exactly those (`InboxBridge.records(in:)`'s own
            // doc comment). Membership and order are still the core's —
            // `records(in:)` uses `ff_inbox_item_in_conv`, the same
            // predicate thread-building uses.
            return inbox.records(in: conversation).map { record in
                message(from: record, nowMs: nowMs, now: now)
            }
        }
    }

    private func message(from record: FeedItemRecord, nowMs: UInt32, now: Date) -> FeedMessage {
        let isOut = (record.direction == .out)
        // Identity is joined from the crew roster for inbound items only,
        // through the same PAIRED-only gate `ff_inbox`'s own join
        // applies (`ff_inbox_msg_t.identity_known`: "joined by from_node
        // to a PAIRED roster member (never fabricated)"). Merely being
        // in the roster is not enough — an inbound message upserts its
        // sender so the node's existence is recorded, and an upserted
        // slot has an EMPTY name until a NodeInfo arrives. Rendering
        // that empty string as a name would be worse than rendering
        // none, so an unnamed or unpaired sender gets `nil`.
        let member = isOut ? nil : crew.member(nodeID: record.fromNode, now: nowMs)
            .flatMap { $0.paired && !$0.displayName.isEmpty ? $0 : nil }
        return FeedMessage(
            id: CoreInboxProvider.feedMessageID(for: record),
            kind: MessageKind(feedKind: record.kind),
            direction: MessageDirection(feedDirection: record.direction),
            senderID: isOut ? nil : (record.fromNode == 0 ? nil : record.fromNode),
            senderName: member?.displayName,
            text: record.text,
            // `at_ms` is a 32-bit truncated epoch-millisecond reading
            // (`FireflyClock`'s own convention), so the absolute Date is
            // reconstructed as "now minus the age the core reports"
            // rather than by widening the truncated value — the age is
            // the part that is actually meaningful across a wrap.
            timestamp: now.addingTimeInterval(-TimeInterval(nowMs &- record.atMs) / 1000),
            unread: record.unread,
            destination: isOut ? (record.toNode == 0 ? meshBroadcastAddress : record.toNode) : nil,
            packetID: record.packetID.rawValue == 0 ? nil : record.packetID.rawValue,
            deliveryState: DeliveryState(ffSendStatus: record.sendStatus.ffValue),
            statusAt: now.addingTimeInterval(-TimeInterval(nowMs &- record.statusAtMs) / 1000))
    }

    /// `FeedMessage.id` must be STABLE across rebuilds (SwiftUI identity)
    /// and must key the outbox setters.
    ///
    /// For an OUT item it IS the outbox id — the same value
    /// `ThreadViewModel` minted and handed to `push`, so
    /// `markSent(outboxID:)` finds exactly the row it created.
    ///
    /// An inbound item has no outbox id by design (`ff_feed_item_t`'s
    /// own "0 = not tracked ... every inbound item"), so its id is
    /// DERIVED — deterministically, from the three facts the core stores
    /// about it — with the top bit set, keeping it disjoint from the
    /// outbox-id space exactly as `InboundFeedIDGenerator` does. Derived,
    /// not invented: the same item always yields the same id, and no id
    /// is minted for an item that does not exist.
    static func feedMessageID(for record: FeedItemRecord) -> UInt64 {
        if record.direction == .out, record.outboxID.rawValue != 0 {
            return UInt64(record.outboxID.rawValue)
        }
        var hash: UInt64 = 0xcbf2_9ce4_8422_2325 // FNV-1a offset basis
        func mix(_ byte: UInt8) {
            hash ^= UInt64(byte)
            hash = hash &* 0x1000_0000_01b3
        }
        withUnsafeBytes(of: record.fromNode.littleEndian) { $0.forEach(mix) }
        withUnsafeBytes(of: record.atMs.littleEndian) { $0.forEach(mix) }
        record.text.utf8.forEach(mix)
        return hash | 0x8000_0000_0000_0000
    }

    // MARK: - Writes

    public nonisolated func markRead(_ conversation: ConversationKind) -> Int {
        MainActor.assumeIsolated { inbox.markThreadRead(conversation) }
    }

    public nonisolated func push(_ message: FeedMessage, into conversation: ConversationKind) {
        MainActor.assumeIsolated {
            // Echo-dedup, scoped to MY OWN sent packet ids — see this
            // file's header and `InMemoryInboxStore.mySentPacketIDs`.
            if let packetID = message.packetID, message.direction != .out, mySentPacketIDs.contains(packetID) {
                return
            }
            // An inbound item's sender must exist in the roster for the
            // core to be able to join an identity onto it — and, for a
            // DIRECT message, for the conversation to exist at all
            // ("A conversation exists only for CREW and for PAIRED
            // members"). Upserting the sender records the FACT that a
            // node exists; it does NOT pair them (`setPaired` is the
            // user's decision, made on the Connect screen) and it
            // invents no name.
            if message.direction != .out, let senderID = message.senderID, senderID != 0 {
                crew.upsert(nodeID: senderID)
            }

            let atMs = FireflyClock.millis(since: message.timestamp)
            inbox.push(
                FeedItem(
                    kind: message.kind.feedKind,
                    fromNode: message.senderID ?? 0,
                    atMs: atMs,
                    text: message.text,
                    direction: message.direction.feedDirection,
                    // The core's own broadcast sentinel is 0, not
                    // 0xFFFFFFFF: "core stays mesh-agnostic: mapping
                    // MC_ADDR_BROADCAST -> 0 is the push site's job"
                    // (ff_feed.h). This is that push site.
                    toNode: (message.destination == meshBroadcastAddress) ? 0 : (message.destination ?? 0),
                    outboxID: OutboxID(UInt32(truncatingIfNeeded: message.id))),
                unread: message.unread)

            // WAITING is a real, stored transition — the row has to show
            // it the instant SEND is pressed (S24's 2026-09-07
            // amendment) — but `ff_feed_push` writes FF_SEND_NONE, so the
            // state the caller pushed is applied through the sanctioned
            // setter immediately after.
            if message.direction == .out, let state = message.deliveryState, state == .waiting || state == .dropped {
                inbox.setSendStatus(outboxID: OutboxID(UInt32(truncatingIfNeeded: message.id)),
                                     status: FeedSendStatus(delivery: state), atMs: atMs)
            }
        }
    }

    public nonisolated func markSent(outboxID: UInt64, packetID: UInt32, at: Date) {
        MainActor.assumeIsolated {
            let key = OutboxID(UInt32(truncatingIfNeeded: outboxID))
            // `want_ack` is not the caller's to state twice: it is a fact
            // about the destination (nothing acks a broadcast — A01), and
            // the item the core already holds is where that destination
            // lives. Read it back rather than re-deriving it here.
            let wantAck = recordAnywhere(outboxID: key).map { $0.toNode != 0 } ?? false
            mySentPacketIDs.insert(packetID)
            inbox.markSent(outboxID: key, packetID: PacketID(packetID), wantAck: wantAck,
                            atMs: FireflyClock.millis(since: at))
        }
    }

    public nonisolated func setStatus(outboxID: UInt64, state: DeliveryState, at: Date) {
        MainActor.assumeIsolated {
            inbox.setSendStatus(outboxID: OutboxID(UInt32(truncatingIfNeeded: outboxID)),
                                 status: FeedSendStatus(delivery: state),
                                 atMs: FireflyClock.millis(since: at))
        }
    }

    public nonisolated func setStatus(packetID: UInt32, state: DeliveryState, at: Date) {
        MainActor.assumeIsolated {
            // Keyed by packet id, gated inside the C library by its own
            // SENT + want_ack precondition — this wrapper adds none of
            // its own (`InboxBridge.setAck`'s doc comment). Anything but
            // DELIVERED/NO ACK has no packet-id-keyed meaning and is
            // ignored rather than forced through as an ack.
            switch state {
            case .delivered: inbox.setAck(packetID: PacketID(packetID), ok: true, atMs: FireflyClock.millis(since: at))
            case .noAck: inbox.setAck(packetID: PacketID(packetID), ok: false, atMs: FireflyClock.millis(since: at))
            case .waiting, .sent, .dropped: break
            }
        }
    }

    /// Every conversation the feed currently holds an item for, searched
    /// for one outbox id. Used only by `markSent`, which has an id but
    /// not the conversation it belongs to.
    private func recordAnywhere(outboxID: OutboxID) -> FeedItemRecord? {
        var kinds: Set<ConversationKind> = [.crew]
        for member in crew.members(now: FireflyClock.nowMillis()) { kinds.insert(.member(member.nodeID)) }
        for kind in kinds {
            if let match = inbox.records(in: kind).first(where: { $0.outboxID == outboxID }) { return match }
        }
        return nil
    }
}

// MARK: - Vocabulary translation

/// A fixed-size FIFO of the most recent 64 packet ids — the same bounded
/// echo-dedup memory `InMemoryInboxStore`'s private `SentIDRing` is, as
/// a separate type because that one is `private` to its own file.
private struct PacketIDRing {
    static let capacity = 64
    private var order: [UInt32] = []
    private var members: Set<UInt32> = []

    mutating func insert(_ id: UInt32) {
        guard !members.contains(id) else { return }
        order.append(id)
        members.insert(id)
        if order.count > Self.capacity { members.remove(order.removeFirst()) }
    }

    func contains(_ id: UInt32) -> Bool { members.contains(id) }
}

extension MessageKind {
    /// `FeedKind` (slice B's `ff_feed_kind_t` mirror) -> this module's
    /// own `MessageKind`. The two enumerate the SAME four live kinds —
    /// both already fold the retired PULSE slot into `.text` at their
    /// own C boundary (`FeedKind.init(ffKind:)`'s `default`), so this
    /// mapping is total and lossless.
    init(feedKind: FeedKind) {
        switch feedKind {
        case .text: self = .text
        case .rally: self = .rally
        case .status: self = .status
        case .flare: self = .flare
        }
    }

    var feedKind: FeedKind {
        switch self {
        case .text: return .text
        case .rally: return .rally
        case .status: return .status
        case .flare: return .flare
        }
    }
}

extension MessageDirection {
    init(feedDirection: FeedDirection) {
        switch feedDirection {
        case .out: self = .out
        case .broadcast: self = .broadcast
        case .direct: self = .direct
        case .unknown: self = .unknown
        }
    }

    var feedDirection: FeedDirection {
        switch self {
        case .out: return .out
        case .broadcast: return .broadcast
        case .direct: return .direct
        case .unknown: return .unknown
        }
    }
}

extension FeedSendStatus {
    init(delivery: DeliveryState) {
        switch delivery {
        case .waiting: self = .waiting
        case .sent: self = .sent
        case .delivered: self = .delivered
        case .noAck: self = .noAck
        case .dropped: self = .dropped
        }
    }
}
