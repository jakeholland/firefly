//
//  TCPTransportTests.swift — `TCPTransport` against a local socket, no
//  hardware, no `meshtasticd`, no `FIREFLY_HARDWARE` gate.
//
//  The loopback "server" the tests connect to is deliberately plain
//  POSIX sockets (`socket`/`bind`/`listen`/`accept`), NOT
//  `Network.framework` — this is a test-scaffolding choice, not a
//  statement about `TCPTransport` itself (which correctly uses
//  `NWConnection`, see that file). A raw BSD socket is the simplest
//  possible stand-in for "something is listening on a TCP port," and
//  keeping the fixture that plain sidesteps `NWListener`-specific
//  environment quirks entirely — the thing under test is `TCPTransport`
//  as a CLIENT, which is what this app actually ships.
//
//  This is what the spec's Slice F "Must add" means by "a TCP transport
//  test against a local socket": real TCP, a real accepted connection,
//  just not a radio.
//
import Darwin
import FireflyMesh
import XCTest

final class TCPTransportTests: XCTestCase {

    /// A minimal accept-and-capture loopback server on plain POSIX
    /// sockets. Binds to port 0 so the OS assigns a free ephemeral
    /// port — parallel test runs never collide.
    private final class LoopbackServer: @unchecked Sendable {
        private let listenFD: Int32
        let port: UInt16
        private let lock = NSLock()
        private var acceptedFD: Int32?
        private var received = Data()
        private var acceptThread: Thread?

        /// - Parameter deferListen: when `true`, `bind()` happens in
        ///   `init` (so `port` is known and reserved up front) but
        ///   `listen()` does not — the fd sits bound-but-not-listening
        ///   until `beginListening()` is called. A client connecting to
        ///   a bound-but-not-listening port sees the same refusal
        ///   behaviour as connecting to a port nothing has touched at
        ///   all, which is what makes this a faithful stand-in for "the
        ///   server process hasn't started listening yet" without
        ///   racing to learn a port number only after that process
        ///   starts.
        init(deferListen: Bool = false) throws {
            let fd = socket(AF_INET, SOCK_STREAM, 0)
            guard fd >= 0 else { throw POSIXError(.init(rawValue: errno) ?? .EIO) }
            var reuse: Int32 = 1
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, socklen_t(MemoryLayout<Int32>.size))

            var addr = sockaddr_in()
            addr.sin_family = sa_family_t(AF_INET)
            addr.sin_addr.s_addr = inet_addr("127.0.0.1")
            addr.sin_port = 0 // ask the OS for a free port
            let bindResult = withUnsafePointer(to: &addr) { ptr -> Int32 in
                ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    Darwin.bind(fd, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
            guard bindResult == 0 else {
                Darwin.close(fd)
                throw POSIXError(.init(rawValue: errno) ?? .EIO)
            }

            var bound = sockaddr_in()
            var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            _ = withUnsafeMutablePointer(to: &bound) { ptr -> Int32 in
                ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    Darwin.getsockname(fd, sa, &len)
                }
            }

            self.listenFD = fd
            self.port = UInt16(bigEndian: bound.sin_port)

            if !deferListen {
                guard Darwin.listen(fd, 1) == 0 else {
                    Darwin.close(fd)
                    throw POSIXError(.init(rawValue: errno) ?? .EIO)
                }
            }
        }

        /// Transitions a `deferListen: true` server from
        /// bound-but-not-listening to actually listening (and starts
        /// accepting) — simulating a peer process that finishes
        /// starting up some time after the client's first connection
        /// attempt.
        func beginListening() {
            guard Darwin.listen(listenFD, 1) == 0 else { return }
            acceptOneClient()
        }

        /// Blocks (on a background thread) until one client connects.
        func acceptOneClient() {
            let thread = Thread { [weak self] in
                guard let self else { return }
                let client = Darwin.accept(self.listenFD, nil, nil)
                guard client >= 0 else { return }
                self.lock.lock(); self.acceptedFD = client; self.lock.unlock()
                var buffer = [UInt8](repeating: 0, count: 4096)
                while true {
                    let n = Darwin.read(client, &buffer, buffer.count)
                    if n <= 0 { break }
                    self.lock.lock()
                    self.received.append(contentsOf: buffer[0..<n])
                    self.lock.unlock()
                }
            }
            thread.start()
            lock.lock(); acceptThread = thread; lock.unlock()
        }

        func waitForClient(timeout: TimeInterval) -> Bool {
            let deadline = Date().addingTimeInterval(timeout)
            while Date() < deadline {
                lock.lock(); let has = acceptedFD != nil; lock.unlock()
                if has { return true }
                usleep(10_000)
            }
            return false
        }

        func sendToClient(_ data: [UInt8]) -> Bool {
            lock.lock(); let fd = acceptedFD; lock.unlock()
            guard let fd else { return false }
            return data.withUnsafeBufferPointer { buf -> Bool in
                var offset = 0
                while offset < buf.count {
                    let n = Darwin.write(fd, buf.baseAddress!.advanced(by: offset), buf.count - offset)
                    if n <= 0 { return false }
                    offset += n
                }
                return true
            }
        }

        func receivedSoFar() -> [UInt8] {
            lock.lock(); defer { lock.unlock() }
            return Array(received)
        }

        func stop() {
            lock.lock()
            let client = acceptedFD
            acceptedFD = nil
            lock.unlock()
            if let client { Darwin.close(client) }
            Darwin.close(listenFD)
        }
    }

    private func withTimeout<T: Sendable>(seconds: TimeInterval, _ operation: @escaping @Sendable () async -> T) async -> T? {
        await withTaskGroup(of: T?.self) { group in
            group.addTask { await operation() }
            group.addTask {
                try? await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
                return nil
            }
            let result = await group.next() ?? nil
            group.cancelAll()
            return result
        }
    }

    func testConnectSendAndReceiveOverLoopback() async throws {
        let server = try LoopbackServer()
        defer { server.stop() }
        server.acceptOneClient()
        guard server.port != 0 else { throw XCTSkip("loopback listener did not bind a port") }

        let transport = TCPTransport(host: "127.0.0.1", port: server.port)
        let stream = transport.events()
        let collector = Task<[TransportEvent], Never> {
            var out: [TransportEvent] = []
            for await event in stream {
                out.append(event)
                if out.count >= 2 { break }
            }
            return out
        }
        try await transport.connect()
        let events = await withTimeout(seconds: 5) { await collector.value } ?? []
        XCTAssertEqual(events.count, 2)
        if case .connecting = events[0] {} else { XCTFail("expected .connecting") }
        if case .ready = events[1] {} else { XCTFail("expected .ready") }

        XCTAssertTrue(server.waitForClient(timeout: 5), "server never observed an accepted connection")

        // App -> "radio": send() reaches the loopback server.
        let framed = try XCTUnwrap(StreamFramer.frame(Data([0x08, 0x2A])))
        try await transport.send(framed)
        let deadline = Date().addingTimeInterval(5)
        while server.receivedSoFar().count < framed.count && Date() < deadline {
            try await Task.sleep(nanoseconds: 20_000_000)
        }
        XCTAssertEqual(server.receivedSoFar(), Array(framed))

        // "Radio" -> app: bytes pushed from the server side arrive as
        // raw, unframed `.received(Data)`.
        let inboundStream = transport.events()
        let inboundCollector = Task<Data, Never> {
            var out = Data()
            for await event in inboundStream {
                if case .received(let data) = event {
                    out.append(data)
                    if out.count >= 4 { break }
                }
            }
            return out
        }
        XCTAssertTrue(server.sendToClient([0x94, 0xC3, 0x00, 0x00]))
        let inbound = await withTimeout(seconds: 5) { await inboundCollector.value } ?? Data()
        XCTAssertEqual(Array(inbound), [0x94, 0xC3, 0x00, 0x00])

        await transport.disconnect()
    }

    func testConnectFailsWithinTheGraceWindowWhenNothingIsListening() async throws {
        // Port 1 on loopback: reserved, nothing binds it in a sandboxed
        // test run, so `NWConnection` sits in `.waiting(ECONNREFUSED)`
        // the whole time (verified: it never transitions to `.failed`
        // on its own) — proving `connect()` still surfaces a failure,
        // bounded by `TCPTransport`'s ~3s grace window, instead of
        // hanging forever when there is no `meshtasticd` (or, on the
        // serial side, no board) actually there.
        let transport = TCPTransport(host: "127.0.0.1", port: 1)
        let start = Date()
        do {
            try await withThrowingTimeout(seconds: 10) { try await transport.connect() }
            XCTFail("connecting to a closed port should throw")
        } catch is TimeoutError {
            XCTFail("connect() hung well past its own bounded grace window")
        } catch {
            // expected: TransportError, once the grace window elapses.
        }
        let elapsed = Date().timeIntervalSince(start)
        XCTAssertGreaterThan(elapsed, 1.0, "a genuine refusal should not fail before the grace window has had a chance to run")
        XCTAssertLessThan(elapsed, 8.0, "connect() should fail at (or shortly after) the ~3s grace window, not hang")
    }

    func testConnectSucceedsWhenListenerStartsAfterOneSecondDelay() async throws {
        // The server is bound (so its port is reserved and known) but
        // deliberately NOT listening yet — a stand-in for "meshtasticd
        // hasn't finished starting up." The very first `.waiting`
        // TCPTransport.connect() sees here is therefore
        // indistinguishable, in the moment, from the closed-port case
        // above; it is only the bounded grace window that lets this
        // one still succeed once the listener appears a moment later.
        let server = try LoopbackServer(deferListen: true)
        defer { server.stop() }
        guard server.port != 0 else { throw XCTSkip("loopback listener did not bind a port") }

        DispatchQueue.global().asyncAfter(deadline: .now() + 1.0) {
            server.beginListening()
        }

        let transport = TCPTransport(host: "127.0.0.1", port: server.port)
        let start = Date()
        try await withThrowingTimeout(seconds: 8) { try await transport.connect() }
        let elapsed = Date().timeIntervalSince(start)
        XCTAssertGreaterThanOrEqual(elapsed, 0.9, "connect() should not have succeeded before the listener existed")
        XCTAssertLessThan(elapsed, 3.0, "connect() should succeed once the listener appears, well inside the ~3s grace window")

        XCTAssertTrue(server.waitForClient(timeout: 5), "server never observed an accepted connection")
        await transport.disconnect()
    }

    private struct TimeoutError: Error {}

    private func withThrowingTimeout<T: Sendable>(seconds: TimeInterval, _ operation: @escaping @Sendable () async throws -> T) async throws -> T {
        try await withThrowingTaskGroup(of: T.self) { group in
            group.addTask { try await operation() }
            group.addTask {
                try await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
                throw TimeoutError()
            }
            let result = try await group.next()!
            group.cancelAll()
            return result
        }
    }
}
