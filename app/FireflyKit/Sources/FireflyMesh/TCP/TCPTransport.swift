//
//  TCPTransport.swift — `MeshTransport` over TCP port 4403
//  (docs/specs/A01-companion-app.md, "TCP — meshtasticd and the sim").
//
//  This is what lets the app talk to the same `meshtasticd` the
//  firmware's own e2e tests use, and — via `ffsim`'s TCP transport —
//  eventually the sim itself. Same caveat as the firmware's own tools:
//  one `meshtasticd` accepts one client at a time.
//
//  `kind == .stream`, same framing split as `SerialTransport`: this
//  file moves raw bytes, `StreamFramer` (one layer up, in the client)
//  turns them into `FromRadio` protobufs.
//
//  Built on `Network.framework` (`NWConnection`) rather than raw BSD
//  sockets, deliberately: it is available on both platforms this
//  package targets (iOS 17+/macOS 14+), it hands connection-state
//  changes to a callback instead of requiring a poll loop, and its
//  receive/send queue is already off the caller's thread — the same
//  off-main-actor rule the spec's threading model states for every
//  transport's own I/O.
//
import Foundation
import Network

public final class TCPTransport: MeshTransport, @unchecked Sendable {
    public let kind: TransportKind = .stream
    public let host: String
    public let port: UInt16

    /// Meshtastic's own TCP API port — same on `meshtasticd` and the
    /// sim.
    public static let defaultPort: UInt16 = 4403

    private let hub = EventHub<TransportEvent>()
    private let queue = DispatchQueue(label: "firefly.tcp.transport")
    private let lock = NSLock()
    private var connection: NWConnection?
    /// Set once `.ready` has been yielded, so a later state change (a
    /// `.cancelled` that follows a normal `disconnect()`) does not
    /// double-report itself as a failure.
    private var everReady = false

    public init(host: String, port: UInt16 = TCPTransport.defaultPort) {
        self.host = host
        self.port = port
    }

    public func events() -> AsyncStream<TransportEvent> {
        hub.subscribe()
    }

    public func connect() async throws {
        hub.yield(.connecting)
        setEverReady(false)

        // Meshtastic's TCP API is length/magic-framed at the application
        // layer (StreamFramer's job, one layer up). Disable Nagle so a
        // small frame — a Heartbeat is a handful of bytes — is not held
        // back waiting for more data to coalesce with.
        let tcpOptions = NWProtocolTCP.Options()
        tcpOptions.noDelay = true
        let params = NWParameters(tls: nil, tcp: tcpOptions)
        let conn = NWConnection(host: NWEndpoint.Host(host), port: NWEndpoint.Port(rawValue: port) ?? 4403, using: params)
        setConnection(conn)

        // `resumed` guards the ONE continuation this call owns against
        // being resumed twice — `stateUpdateHandler` keeps firing after
        // that (a later `.cancelled` following `.ready` is normal), so
        // this is a one-shot latch, checked and set under the same lock
        // as every other piece of mutable state here. Not `@escaping`
        // captured-var mutation (which the compiler correctly flags as
        // a Swift 6 error under concurrent delivery) — a dedicated
        // non-async helper, same pattern `LoopbackTransport.record(_:)`
        // uses for the same reason.
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            conn.stateUpdateHandler = { [weak self] state in
                guard let self else { return }
                switch state {
                case .ready:
                    self.setEverReady(true)
                    self.startReceiveLoop(conn)
                    self.hub.yield(.ready)
                    if self.tryLatchResumed() { continuation.resume() }
                case .failed(let error):
                    self.hub.yield(.disconnected(reason: "\(error)"))
                    if self.tryLatchResumed() {
                        continuation.resume(throwing: TransportError.writeFailed("\(error)"))
                    }
                case .cancelled:
                    if self.everReadyNow() {
                        self.hub.yield(.disconnected(reason: nil))
                    } else if self.tryLatchResumed() {
                        continuation.resume(throwing: TransportError.notConnected)
                    }
                case .waiting(let error):
                    // NWConnection's own default policy: `.waiting`
                    // means "a recoverable-looking error, keep retrying
                    // automatically" (Wi-Fi/cellular handover, a
                    // momentary DNS hiccup) rather than `.failed`
                    // (terminal) — and it treats a REFUSED connection
                    // (no `meshtasticd`/sim listening) exactly the same
                    // way, which means an un-handled `.waiting` retries
                    // silently forever. Verified against this package's
                    // own behaviour: an `NWConnection` to a closed local
                    // port sits in `.waiting(ECONNREFUSED)`, never
                    // `.failed`, never resolving on its own.
                    //
                    // Once this transport has been `.ready` at least
                    // once, a later `.waiting` is a real, recoverable
                    // hiccup on a LIVE session — exactly the case that
                    // default retry policy exists for — so it is left
                    // alone here. Only during the FIRST connection
                    // attempt is `.waiting` treated as a failure: this
                    // app has no interest in a `connect()` call that
                    // never returns because nothing is listening.
                    guard !self.everReadyNow() else { break }
                    self.hub.yield(.disconnected(reason: "\(error)"))
                    conn.cancel()
                    if self.tryLatchResumed() {
                        continuation.resume(throwing: TransportError.writeFailed("\(error)"))
                    }
                default:
                    break
                }
            }
            conn.start(queue: queue)
        }
    }

    public func disconnect() async {
        let conn = takeConnection()
        conn?.cancel()
        hub.yield(.disconnected(reason: nil))
    }

    public func send(_ data: Data) async throws {
        guard let conn = currentConnection() else { throw TransportError.notConnected }

        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            conn.send(content: data, completion: .contentProcessed { error in
                if let error {
                    continuation.resume(throwing: TransportError.writeFailed("\(error)"))
                } else {
                    continuation.resume()
                }
            })
        }
    }

    private func startReceiveLoop(_ conn: NWConnection) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 4096) { [weak self] data, _, isComplete, error in
            guard let self else { return }
            if let data, !data.isEmpty {
                self.hub.yield(.received(data))
            }
            if let error {
                self.hub.yield(.disconnected(reason: "\(error)"))
                return
            }
            if isComplete {
                self.hub.yield(.disconnected(reason: "connection closed by peer"))
                return
            }
            self.startReceiveLoop(conn)
        }
    }

    // MARK: - synchronous state, deliberately non-async (see connect()'s
    // comment — NSLock must never straddle a suspension point).

    private var resumedOnce = false

    private func tryLatchResumed() -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard !resumedOnce else { return false }
        resumedOnce = true
        return true
    }

    private func setEverReady(_ value: Bool) {
        lock.lock(); everReady = value; if value == false { resumedOnce = false }; lock.unlock()
    }

    private func everReadyNow() -> Bool {
        lock.lock(); defer { lock.unlock() }
        return everReady
    }

    private func setConnection(_ conn: NWConnection?) {
        lock.lock(); connection = conn; lock.unlock()
    }

    private func currentConnection() -> NWConnection? {
        lock.lock(); defer { lock.unlock() }
        return connection
    }

    private func takeConnection() -> NWConnection? {
        lock.lock()
        let conn = connection
        connection = nil
        lock.unlock()
        return conn
    }
}
