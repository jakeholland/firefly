//
//  Transport.swift — the byte/message transport seam.
//
//  The C library's seam is a vtable (`mc_transport_t`, three function
//  pointers). The Swift seam is a protocol with an `AsyncStream` of
//  inbound messages, for the same reason: nothing above this line knows
//  whether the bytes came from CoreBluetooth, a USB-serial device, or a
//  TCP socket to meshtasticd.
//
//  Framing lives on ONE side of this seam: a `.stream` transport hands
//  up raw bytes and the client runs `StreamFramer` over them; a
//  `.message` transport (BLE) hands up whole `FromRadio` protobufs.
//  `kind` is how the client knows which, rather than guessing.
//
import Foundation

public enum TransportKind: Sendable {
    /// Byte stream — serial, TCP. Needs `StreamFramer`.
    case stream
    /// Already message-framed — BLE GATT.
    case message
}

public enum TransportEvent: Sendable {
    case connecting
    /// The link is up AND, for BLE, the FROMNUM subscription is ACKed —
    /// see `FromRadioDrainPolicy`. Only now may a client send
    /// `want_config`.
    case ready
    case received(Data)
    case disconnected(reason: String?)
}

public protocol MeshTransport: AnyObject, Sendable {
    var kind: TransportKind { get }
    /// A fresh, independent stream of inbound events for the caller.
    /// Multicast via `EventHub` (docs/specs/A01-companion-app.md, S1):
    /// more than one subscriber is expected, and each gets its own
    /// `.bufferingNewest(4096)` stream rather than competing with the
    /// others for one shared `AsyncStream`. The transport finishes every
    /// outstanding stream on permanent failure.
    func events() -> AsyncStream<TransportEvent>
    func connect() async throws
    func disconnect() async
    /// Write one `ToRadio` message. For `.stream` transports the client
    /// has already applied `StreamFramer.frame(_:)`.
    func send(_ data: Data) async throws
}

public enum TransportError: Error, Equatable, Sendable {
    case notConnected
    case writeFailed(String)
    case unsupportedOnThisPlatform(String)
}

/// A transport that is wired to nothing — the milestone-1 stand-in, and
/// permanently useful as the thing unit tests inject.
///
/// It is deliberately NOT a fake radio: it invents no nodes, no
/// positions and no messages. Anything it reported would be fabricated
/// data on a screen whose entire design promise is that nothing on it is
/// fabricated (docs/ARCHITECTURE.md, "Honest state"). Tests that need
/// traffic push exact bytes in with `inject(_:)`.
public final class LoopbackTransport: MeshTransport, @unchecked Sendable {
    public let kind: TransportKind
    private let hub = EventHub<TransportEvent>()
    private let lock = NSLock()
    private var sent: [Data] = []

    public init(kind: TransportKind = .message) {
        self.kind = kind
    }

    public func events() -> AsyncStream<TransportEvent> {
        hub.subscribe()
    }

    public func connect() async throws {
        hub.yield(.connecting)
        hub.yield(.ready)
    }

    public func disconnect() async {
        hub.yield(.disconnected(reason: nil))
        hub.finish()
    }

    public func send(_ data: Data) async throws {
        record(data)
    }

    // Deliberately non-async: NSLock may not be held across a suspension
    // point, so the critical section is its own synchronous function.
    private func record(_ data: Data) {
        lock.lock(); defer { lock.unlock() }
        sent.append(data)
    }

    /// Everything written to this transport so far, in order.
    public var sentMessages: [Data] {
        lock.lock(); defer { lock.unlock() }
        return sent
    }

    /// Deliver bytes as if the radio had sent them.
    public func inject(_ data: Data) {
        hub.yield(.received(data))
    }
}
