//
//  HistorySchema.swift — the SwiftData schema M3 persists message
//  history through (docs/specs/A01-companion-app.md, M3 + Persistence:
//  "M3 adds SwiftData for message history").
//
//  `firmware/core`'s own `ff_feed_t` is a `FF_FEED_CAP` (32) item RAM
//  ring (`firmware/core/include/ff_feed.h`) that `ff_feed_init` wipes on
//  every process launch — this schema is the ONLY place any message
//  this app has ever shown survives a relaunch at all. It stores plain
//  primitives, never this module's own enums directly, so a future
//  rename/refactor of `MessageKind`/`MessageDirection`/`DeliveryState`
//  cannot silently corrupt an on-disk row underneath it — the raw
//  strings/ints here are this schema's OWN wire format, decoded
//  defensively (`PersistedMessage.decoded()`), not a mirror of whatever
//  those Swift types currently look like.
//
//  Migration policy (documented, per the M3 task): a schema version
//  (`HistorySchemaV1`) plus DROP-AND-RECREATE on any mismatch
//  `HistoryMigrationPlan` does not cover — see that type's own doc
//  comment for the full justification. This is the one place in this
//  app that silently discards user data on purpose; `HistoryStore`'s
//  own header comment and the Settings "Clear history" action are the
//  other two places this is disclosed.
//
import FireflyMesh
import Foundation
import SwiftData

/// M3's schema, version 1 — the only version that has ever shipped.
public enum HistorySchemaV1: VersionedSchema {
    public static var versionIdentifier: Schema.Version { Schema.Version(1, 0, 0) }
    public static var models: [any PersistentModel.Type] { [PersistedMessage.self] }
}

/// The drop-and-recreate migration policy: message history is
/// convenience/context, not a safety- or identity-critical record
/// (contrast `CrewPairingStore`'s persisted pairing, which this app
/// never drops on a whim, or the Keychain-held channel PSKs) — so a
/// FUTURE schema this binary predates, or a corrupted store file, fails
/// SOFT: `HistoryStore.makeContainer` deletes whatever is on disk and
/// starts over empty, rather than throwing all the way up through
/// `AppGraph.init` and crashing the app on launch. `stages` is empty
/// today because `HistorySchemaV1` is the only version this app has
/// ever written; the day a `HistorySchemaV2` exists, its migration
/// stage belongs here, and only a drift this plan does not cover for
/// (a version older than this app has ever known how to read at all)
/// ever falls back to drop-and-recreate.
public enum HistoryMigrationPlan: SchemaMigrationPlan {
    public static var schemas: [any VersionedSchema.Type] { [HistorySchemaV1.self] }
    public static var stages: [MigrationStage] { [] }
}

/// One message, durable across a relaunch — the SwiftData mirror of a
/// `FeedMessage` (`InboxViewModel.swift`) this app has pushed at least
/// once, live or restored.
///
/// Every field is a primitive SwiftData stores natively — `Int64`/`Int`/
/// `String`/`Date`/`Bool`, `Optional` exactly where the source field is
/// — rather than this app's own enums directly. `FeedMessage.id: UInt64`
/// in particular does not fit `Int64`'s range as an unsigned value:
/// `InboundFeedIDGenerator` (`InboxViewModel.swift`) deliberately sets
/// the TOP BIT on every inbound id, so roughly half of all ids this app
/// ever mints are >= 2^63. `messageID` stores `Int64(bitPattern:)` —
/// a lossless, order-preserving reinterpretation of the same 64 bits,
/// never a truncation — and reads back with `UInt64(bitPattern:)`.
@Model
public final class PersistedMessage {
    /// `FeedMessage.id`'s bit pattern — the PRIMARY lookup key
    /// `markSent`/`setStatus(outboxID:)` use, exactly like `outbox_id`/
    /// the derived-hash id is for the live C ring (`ff_feed_item_t`'s
    /// own doc comments).
    public var messageID: Int64
    /// `ConversationKind.storageKey` — `"crew"` or `"member:<id>"`.
    public var conversationKey: String
    /// `MessageKind.rawValue`.
    public var kindRaw: String
    /// `MessageDirection.storageRaw` — that type carries no `RawValue`
    /// of its own (it predates this schema), so this file's own
    /// extension gives it one, symmetrical with every other `Raw`
    /// field here.
    public var directionRaw: String
    public var senderID: Int?
    public var senderName: String?
    public var text: String
    public var timestamp: Date
    public var unread: Bool
    public var flareDurationSeconds: Int?
    public var destination: Int?
    /// The routing-ack correlation key, when one exists. `nil`, never
    /// `0` — `0` is never assigned in practice (`OutboxID`/`PacketID`'s
    /// own "0 = not tracked" sentinel convention, `ff_feed.h`), and an
    /// explicit `Optional` reads honestly rather than overloading a
    /// magic number a SECOND time in a field that no longer needs one.
    public var packetID: Int?
    /// `DeliveryState.rawValue`, or `nil` for every inbound item — this
    /// schema's own spelling of `FF_SEND_NONE` (`DeliveryState.swift`'s
    /// own doc comment on why the live vocabulary already uses `nil`
    /// for exactly this).
    public var deliveryStateRaw: String?
    public var statusAt: Date

    public init(messageID: Int64, conversationKey: String, kindRaw: String, directionRaw: String, senderID: Int?,
                senderName: String?, text: String, timestamp: Date, unread: Bool, flareDurationSeconds: Int?,
                destination: Int?, packetID: Int?, deliveryStateRaw: String?, statusAt: Date) {
        self.messageID = messageID
        self.conversationKey = conversationKey
        self.kindRaw = kindRaw
        self.directionRaw = directionRaw
        self.senderID = senderID
        self.senderName = senderName
        self.text = text
        self.timestamp = timestamp
        self.unread = unread
        self.flareDurationSeconds = flareDurationSeconds
        self.destination = destination
        self.packetID = packetID
        self.deliveryStateRaw = deliveryStateRaw
        self.statusAt = statusAt
    }
}

// MARK: - Conversions

extension ConversationKind {
    /// `PersistedMessage.conversationKey`'s wire format — `"crew"` or
    /// `"member:<node id>"`. A plain `String` rather than a second
    /// `Codable` derivation: this is a small, permanent, hand-written
    /// format this schema owns outright, not a mirror of whatever
    /// `ConversationKind`'s own case list happens to look like today.
    var storageKey: String {
        switch self {
        case .crew: return "crew"
        case .member(let id): return "member:\(id)"
        }
    }

    init?(storageKey: String) {
        if storageKey == "crew" { self = .crew; return }
        guard storageKey.hasPrefix("member:"), let id = UInt32(storageKey.dropFirst("member:".count)) else {
            return nil
        }
        self = .member(id)
    }
}

extension MessageDirection {
    /// `MessageDirection` carries no `RawValue` of its own (unlike
    /// `MessageKind`/`DeliveryState`) — this schema is the first thing
    /// that needs one, so it gets its own small, private-to-persistence
    /// String encoding rather than widening the type everywhere else in
    /// `FireflyModel` depends on it for.
    var storageRaw: String {
        switch self {
        case .unknown: return "unknown"
        case .broadcast: return "broadcast"
        case .direct: return "direct"
        case .out: return "out"
        }
    }

    init?(storageRaw: String) {
        switch storageRaw {
        case "unknown": self = .unknown
        case "broadcast": self = .broadcast
        case "direct": self = .direct
        case "out": self = .out
        default: return nil
        }
    }
}

extension PersistedMessage {
    /// A fresh row for `message` — used only on first insert
    /// (`HistoryStore.record(_:in:)`'s own "not already present" branch);
    /// an existing row is updated in place through `apply(_:in:)`
    /// instead, never replaced, so its SwiftData identity (and any
    /// relationship a future schema might add) survives an update.
    convenience init(_ message: FeedMessage, in conversation: ConversationKind) {
        self.init(
            messageID: Int64(bitPattern: message.id),
            conversationKey: conversation.storageKey,
            kindRaw: message.kind.rawValue,
            directionRaw: message.direction.storageRaw,
            senderID: message.senderID.map(Int.init),
            senderName: message.senderName,
            text: message.text,
            timestamp: message.timestamp,
            unread: message.unread,
            flareDurationSeconds: message.flareDurationSeconds.map(Int.init),
            destination: message.destination.map(Int.init),
            packetID: message.packetID.map(Int.init),
            deliveryStateRaw: message.deliveryState?.rawValue,
            statusAt: message.statusAt)
    }

    /// Overwrites every field from `message` — `HistoryStore.record(_:
    /// in:)`'s "already present" branch (a re-push of the same id, which
    /// does not happen in practice today but is handled rather than
    /// assumed impossible).
    func apply(_ message: FeedMessage, in conversation: ConversationKind) {
        conversationKey = conversation.storageKey
        kindRaw = message.kind.rawValue
        directionRaw = message.direction.storageRaw
        senderID = message.senderID.map(Int.init)
        senderName = message.senderName
        text = message.text
        timestamp = message.timestamp
        unread = message.unread
        flareDurationSeconds = message.flareDurationSeconds.map(Int.init)
        destination = message.destination.map(Int.init)
        packetID = message.packetID.map(Int.init)
        deliveryStateRaw = message.deliveryState?.rawValue
        statusAt = message.statusAt
    }

    /// `nil` for a row this process's own writer never produced (a
    /// future schema's row this migration plan somehow left in place, or
    /// a corrupted value) — decoded defensively, never force-unwrapped:
    /// this is the one seam reading back data a PAST launch wrote, not
    /// data this call stack just produced.
    func decoded() -> (ConversationKind, FeedMessage)? {
        guard let conversation = ConversationKind(storageKey: conversationKey),
              let kind = MessageKind(rawValue: kindRaw),
              let direction = MessageDirection(storageRaw: directionRaw) else {
            return nil
        }
        // `UInt32(exactly:)`/`UInt16(exactly:)`, never the trapping
        // `.init(_:)` — a corrupted or foreign `Int` value is dropped
        // (`nil`) rather than crashing the app on a row this launch did
        // not itself write.
        let message = FeedMessage(
            id: UInt64(bitPattern: messageID),
            kind: kind,
            direction: direction,
            senderID: senderID.flatMap { UInt32(exactly: $0) },
            senderName: senderName,
            text: text,
            timestamp: timestamp,
            unread: unread,
            flareDurationSeconds: flareDurationSeconds.flatMap { UInt16(exactly: $0) },
            destination: destination.flatMap { UInt32(exactly: $0) },
            packetID: packetID.flatMap { UInt32(exactly: $0) },
            deliveryState: deliveryStateRaw.flatMap { DeliveryState(rawValue: $0) },
            statusAt: statusAt)
        return (conversation, message)
    }
}
