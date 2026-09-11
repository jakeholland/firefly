//
//  SerialTransport.swift — `MeshTransport` over a `/dev/cu.*` USB-serial
//  port (docs/specs/A01-companion-app.md, "Serial — macOS only").
//
//  macOS only: there is no iOS equivalent, and this is `#if os(macOS)`
//  for the same reason `SerialPort` is.
//
//  `kind == .stream`: this transport hands `.received(Data)` up as raw,
//  UNFRAMED bytes exactly as they came off the wire — framing them into
//  `FromRadio` protobufs is the client's job, via `StreamFramer`, one
//  layer above this file (see `Transport.swift`'s header). Likewise
//  `send(_:)` writes exactly the bytes it is given; the caller has
//  already run them through `StreamFramer.frame(_:)`.
//
//  Contention warning, from the spec, because it will bite: a
//  Meshtastic node's serial port is single-client. If a `meshtastic`
//  CLI session or console already owns the port, `connect()` here fails
//  fast — an `open()` errno surfaced as `TransportError.writeFailed`,
//  not a hang — rather than the "looks like it connected, sees
//  nothing" failure mode a silent fallback would produce.
//
#if os(macOS)
import Foundation

public final class SerialTransport: MeshTransport, @unchecked Sendable {
    public let kind: TransportKind = .stream
    public let path: String

    private let hub = EventHub<TransportEvent>()
    private let lock = NSLock()
    private var port: SerialPort?

    public init(path: String) {
        self.path = path
    }

    public func events() -> AsyncStream<TransportEvent> {
        hub.subscribe()
    }

    public func connect() async throws {
        hub.yield(.connecting)
        let opened: SerialPort
        do {
            opened = try SerialPort(path: path)
        } catch {
            let message = "\(error)"
            hub.yield(.disconnected(reason: message))
            throw TransportError.writeFailed(message)
        }

        setPort(opened)

        opened.startReading(
            onData: { [hub] data in
                hub.yield(.received(data))
            },
            onError: { [weak self, hub] code in
                let reason = code == 0 ? "serial port closed (EOF)" : "serial read error \(code)"
                hub.yield(.disconnected(reason: reason))
                self?.clearPort()
            })

        // There is no handshake at this layer — a serial port is "up"
        // the moment it opens (unlike BLE, which must wait for the
        // FROMNUM subscription ACK before `.ready`). The client above
        // still runs its own want_config handshake before it considers
        // itself ready; this transport's `.ready` only means "bytes can
        // flow now."
        hub.yield(.ready)
    }

    public func disconnect() async {
        clearPort()
        hub.yield(.disconnected(reason: nil))
    }

    public func send(_ data: Data) async throws {
        guard let current = currentPort() else { throw TransportError.notConnected }
        do {
            try current.write(data)
        } catch {
            throw TransportError.writeFailed("\(error)")
        }
    }

    // MARK: - synchronous state, deliberately non-async (NSLock must
    // never straddle a suspension point — same pattern as TCPTransport).

    private func setPort(_ value: SerialPort?) {
        lock.lock(); port = value; lock.unlock()
    }

    private func currentPort() -> SerialPort? {
        lock.lock(); defer { lock.unlock() }
        return port
    }

    private func clearPort() {
        lock.lock()
        let existing = port
        port = nil
        lock.unlock()
        existing?.close()
    }
}
#endif
