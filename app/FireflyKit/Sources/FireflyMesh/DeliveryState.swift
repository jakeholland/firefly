//
//  DeliveryState.swift — what happened to a message I sent.
//
//  This is not a new vocabulary. It is FIVE of the puck's SIX
//  `ff_feed_send_status_t` values (firmware/core/include/ff_feed.h,
//  docs/specs/S24-signals-inbox.md), so an outbox row means the same
//  thing on the phone and on the puck. The sixth, `FF_SEND_NONE`, is the
//  zero value every INBOUND item carries — deliberately zero so a
//  zero-initialized/legacy item never accidentally claims a delivery
//  fact it doesn't have — and has no `DeliveryState` case: there is
//  nothing to show for an item the outbox tracking never touched.
//  `ffSendStatus` converts Swift -> C for outbound values; `init?
//  (ffSendStatus:)` converts C -> Swift and returns `nil` for
//  `FF_SEND_NONE`, pinning that absence explicitly rather than inventing
//  a case for it. A unit test pins every case against the C enum's own
//  raw values — if someone reorders the C enum, the test fails here
//  rather than the two clients quietly disagreeing about what DELIVERED
//  means.
//
import FireflyCore
import Foundation

public enum DeliveryState: String, Sendable, CaseIterable, Equatable {
    /// Handed to the radio, no packet id yet.
    case waiting = "WAITING"
    /// The radio accepted it and gave us a packet id. For a broadcast
    /// this is as far as it ever gets — nobody acks a broadcast, and
    /// showing DELIVERED for one would be a lie.
    case sent = "SENT"
    /// A Routing ack came back for our packet id.
    case delivered = "DELIVERED"
    /// want_ack was set, the ack window expired, nothing came back.
    /// Distinct from `dropped`: we know it went out.
    case noAck = "NO ACK"
    /// The radio refused it (queue full, no route, bad payload).
    case dropped = "DROPPED"

    public var ffSendStatus: ff_feed_send_status_t {
        switch self {
        case .waiting: return FF_SEND_WAITING
        case .sent: return FF_SEND_SENT
        case .delivered: return FF_SEND_DELIVERED
        case .noAck: return FF_SEND_NO_ACK
        case .dropped: return FF_SEND_DROPPED
        }
    }

    /// The reverse mapping, used when `InboxBridge` reads `send_status`
    /// back out of the core. `FF_SEND_NONE` — the value every inbound
    /// item carries, and a zero-initialized/legacy item's default —
    /// returns `nil` rather than inventing a state or crashing.
    public init?(ffSendStatus status: ff_feed_send_status_t) {
        switch status {
        case FF_SEND_NONE: return nil
        case FF_SEND_WAITING: self = .waiting
        case FF_SEND_SENT: self = .sent
        case FF_SEND_DELIVERED: self = .delivered
        case FF_SEND_NO_ACK: self = .noAck
        case FF_SEND_DROPPED: self = .dropped
        default: return nil
        }
    }
}

/// The shell-assigned identity of an outgoing send, stamped BEFORE a
/// packet id can exist (`ff_feed_item_t.outbox_id`'s own doc comment,
/// `ff_feed.h`) — the retry queue's only way back to a specific pending
/// send. `0` is the documented "not tracked" sentinel, same as the C
/// field.
///
/// Wrapped as its own type (PR #261 review, finding 1) rather than a
/// bare `UInt32`: `CoreStore.apply(delivery:)` once passed a value
/// labeled `packetID` into an API that expected an `outbox_id`, and the
/// compiler had nothing to say about it because both were just
/// `UInt32`. `OutboxID`/`PacketID` being distinct types turns that class
/// of mistake into a compile error at every call site that matters —
/// `InboxBridge.markSent`/`setSendStatus`/`FeedItem.outboxID` and
/// `DeliveryEvent` below.
public struct OutboxID: Sendable, Equatable, Hashable {
    public let rawValue: UInt32
    public init(_ rawValue: UInt32) { self.rawValue = rawValue }
}

/// The `MeshPacket` id the radio assigns once a send is accepted
/// (`ff_feed_item_t.packet_id`'s own doc comment) — the
/// `mc_events_t.on_routing_ack` correlation key. Distinct type from
/// `OutboxID` — see that type's own doc comment for why.
public struct PacketID: Sendable, Equatable, Hashable {
    public let rawValue: UInt32
    public init(_ rawValue: UInt32) { self.rawValue = rawValue }
}

/// One delivery-state transition off `MeshtasticClientProtocol.
/// deliveryUpdates()`, shaped so each case carries ONLY the key(s) that
/// transition actually has to give — mirroring `firmware/app/ff_shell.c`
/// (`shell_send_or_queue_text`, `shell_ev_routing_ack`)'s own
/// partitioning of `ff_feed.h`'s three setters:
///  - `.waiting`/`.dropped` carry only `outboxID` — no packet exists yet
///    for `.waiting` (`shell_next_outbox_id` runs before the send is even
///    attempted), and none was ever formed for `.dropped` (outbox-full
///    eviction, or a send the transport refused outright) —
///    `ff_feed_set_send_status_by_outbox_id`.
///  - `.sent` carries BOTH: `outboxID` to find the WAITING item,
///    `packetID`/`wantAck` to stamp onto it — only the send attempt
///    itself knows either — `ff_feed_mark_sent_by_outbox_id`.
///  - `.delivered`/`.noAck` carry only `packetID` — a routing ack (or an
///    explicit NAK) knows nothing about the shell-local outbox id, only
///    the packet id it is answering — `ff_feed_set_ack_by_packet_id`.
/// The TIMEOUT half of NO_ACK (`ff_feed_expire_pending_acks`) is not a
/// per-message event at all — it is a tick-driven sweep over the whole
/// feed with no key — so it has no case here; see `CoreStore.tick(nowMs:)`.
public enum DeliveryEvent: Sendable, Equatable {
    case waiting(outboxID: OutboxID)
    case sent(outboxID: OutboxID, packetID: PacketID, wantAck: Bool)
    case delivered(packetID: PacketID)
    case noAck(packetID: PacketID)
    case dropped(outboxID: OutboxID)
}
