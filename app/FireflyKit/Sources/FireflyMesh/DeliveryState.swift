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
