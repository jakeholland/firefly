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
    /// Inbound events. Exactly one consumer; the transport finishes the
    /// stream on permanent failure.
    var events: AsyncStream<TransportEvent> { get }
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
    public let events: AsyncStream<TransportEvent>
    private let continuation: AsyncStream<TransportEvent>.Continuation
    private let lock = NSLock()
    private var sent: [Data] = []

    public init(kind: TransportKind = .message) {
        self.kind = kind
        var cont: AsyncStream<TransportEvent>.Continuation!
        self.events = AsyncStream { cont = $0 }
        self.continuation = cont
    }

    public func connect() async throws {
        continuation.yield(.connecting)
        continuation.yield(.ready)
    }

    public func disconnect() async {
        continuation.yield(.disconnected(reason: nil))
        continuation.finish()
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
        continuation.yield(.received(data))
    }
}
