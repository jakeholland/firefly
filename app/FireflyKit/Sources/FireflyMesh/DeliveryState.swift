//
//  DeliveryState.swift — what happened to a message I sent.
//
//  This is not a new vocabulary. It is the SAME five states the puck's
//  `ff_feed_send_status_t` uses (firmware/core/include/ff_feed.h,
//  docs/specs/S24-signals-inbox.md), so an outbox row means the same
//  thing on the phone and on the puck. `ffSendStatus` converts, and a
//  unit test pins every case against the C enum's own raw values — if
//  someone reorders the C enum, the test fails here rather than the two
//  clients quietly disagreeing about what DELIVERED means.
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
}
