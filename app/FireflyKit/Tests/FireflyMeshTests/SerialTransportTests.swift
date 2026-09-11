//
//  SerialTransportTests.swift — `SerialTransport`'s real POSIX
//  read/write pipeline, exercised with NO hardware and NO
//  `FIREFLY_HARDWARE` gate.
//
//  The trick: `openpty(3)` hands back a connected master/slave pseudo-
//  terminal pair. The slave side has a real `/dev/ttys*` path and
//  answers every termios ioctl `SerialPort.init` makes exactly like a
//  USB-serial adapter would — so `SerialTransport(path:)` runs its
//  actual init/configure/DispatchSourceRead code, unmodified, against a
//  device this test fully controls from the master side. This is what
//  the spec's Slice F "Must add" means by "serial framing tests against
//  recorded bytes (no port needed)": no board, no `FIREFLY_HARDWARE=1`,
//  and still real POSIX I/O rather than a fake.
//
//  Framing itself (0x94 0xC3, dribble, resync, oversize) is already
//  pinned against `StreamFramer` directly in StreamFramerTests.swift
//  (shared, not this slice's). What is uniquely SerialTransport's to
//  test is that raw bytes survive the real fd -> DispatchSourceRead ->
//  EventHub pipeline intact and in order, at arbitrary OS-chosen read
//  granularity — which is exactly what a pty, unlike an in-memory
//  buffer, actually exercises.
//
#if os(macOS)
import Darwin
import FireflyMesh
import XCTest

final class SerialTransportTests: XCTestCase {

    /// Opens a pty pair and returns (masterFD, slavePath). The caller
    /// owns `masterFD` and must close it; the slave path is opened
    /// fresh by `SerialTransport` itself, exactly as it would open a
    /// real `/dev/cu.*`.
    private func openPTYPair() throws -> (master: Int32, slavePath: String) {
        var master: Int32 = 0
        var slave: Int32 = 0
        guard openpty(&master, &slave, nil, nil, nil) == 0 else {
            throw XCTSkip("openpty unavailable in this sandbox")
        }
        guard let cName = ttyname(slave) else {
            close(master); close(slave)
            throw XCTSkip("ttyname unavailable for pty slave")
        }
        let path = String(cString: cName)
        // SerialTransport opens the slave path itself; this test does
        // not need to keep its own fd to it.
        close(slave)
        return (master, path)
    }

    func testConnectYieldsConnectingThenReady() async throws {
        let (master, path) = try openPTYPair()
        defer { close(master) }

        let transport = SerialTransport(path: path)
        let events = try await drainEventsWhileConnecting(transport, expected: 2)
        // TransportEvent (Transport.swift, shared/not this slice's) is
        // not Equatable, so match by case rather than XCTAssertEqual.
        XCTAssertEqual(events.count, 2)
        if case .connecting = events[0] {} else { XCTFail("expected .connecting, got \(events[0])") }
        if case .ready = events[1] {} else { XCTFail("expected .ready, got \(events[1])") }
        await transport.disconnect()
    }

    /// Bytes written on the master side (standing in for the radio)
    /// arrive on `events()` as raw, unframed `.received(Data)` — real
    /// fd plumbing, not a mock.
    func testBytesWrittenOnMasterArriveAsReceivedEvents() async throws {
        let (master, path) = try openPTYPair()
        defer { close(master) }

        let transport = SerialTransport(path: path)
        _ = try await drainEventsWhileConnecting(transport, expected: 2) // connecting, ready

        let stream = transport.events()
        let collector = Task<[Data], Never> {
            var chunks: [Data] = []
            for await event in stream {
                if case .received(let data) = event {
                    chunks.append(data)
                    if chunks.reduce(0, { $0 + $1.count }) >= 4 { break }
                }
            }
            return chunks
        }

        let payload: [UInt8] = [0x94, 0xC3, 0x00, 0x02]
        payload.withUnsafeBufferPointer { buf in
            _ = write(master, buf.baseAddress, buf.count)
        }

        let chunks = await withTimeout(seconds: 5) { await collector.value }
        let combined = (chunks ?? []).reduce(into: Data()) { $0.append($1) }
        XCTAssertEqual(Array(combined), payload)
        await transport.disconnect()
    }

    /// `send(_:)` writes exactly the bytes it is given (already framed
    /// by the caller — `SerialTransport` does not frame) onto the wire;
    /// the pty's master side sees them arrive.
    func testSendWritesExactBytesToThePort() async throws {
        let (master, path) = try openPTYPair()
        defer { close(master) }

        let transport = SerialTransport(path: path)
        _ = try await drainEventsWhileConnecting(transport, expected: 2)

        let framed = try XCTUnwrap(StreamFramer.frame(Data([0x08, 0x2A])))
        try await transport.send(framed)

        var buffer = [UInt8](repeating: 0, count: 64)
        let deadline = Date().addingTimeInterval(5)
        var total = 0
        while total < framed.count && Date() < deadline {
            let n = read(master, &buffer[total], buffer.count - total)
            if n > 0 { total += n } else { try await Task.sleep(nanoseconds: 20_000_000) }
        }
        XCTAssertEqual(Array(buffer[0..<total]), Array(framed))
        await transport.disconnect()
    }

    /// A byte-at-a-time dribble over the real pty still frames correctly
    /// once `StreamFramer` runs over the raw bytes `SerialTransport`
    /// hands up — the same AC1 guarantee `StreamFramerTests` pins in
    /// memory, now proven across a real fd boundary too.
    func testDribbledBytesStillFrameCorrectlyThroughStreamFramer() async throws {
        let (master, path) = try openPTYPair()
        defer { close(master) }

        let transport = SerialTransport(path: path)
        _ = try await drainEventsWhileConnecting(transport, expected: 2)

        let payload = Data([0x08, 0x2A, 0x10, 0x01])
        let framed = try XCTUnwrap(StreamFramer.frame(payload))

        var framer = StreamFramer()
        var decoded: [Data] = []
        let stream = transport.events()
        let collector = Task {
            for await event in stream {
                if case .received(let raw) = event {
                    decoded.append(contentsOf: framer.feed(raw))
                    if !decoded.isEmpty { break }
                }
            }
        }

        for byte in framed {
            let one = [byte]
            one.withUnsafeBufferPointer { buf in
                _ = write(master, buf.baseAddress, 1)
            }
            try await Task.sleep(nanoseconds: 5_000_000) // force separate reads
        }

        _ = await withTimeout(seconds: 5) { await collector.value }
        XCTAssertEqual(decoded, [payload])
        await transport.disconnect()
    }

    /// Garbage bytes, then a header whose declared length is nowhere
    /// close to what actually follows it (an oversize/"truncated"
    /// header — it promises 0xFFFF payload bytes but the very next
    /// bytes on the wire are a completely different, valid frame, not
    /// 65535 bytes of continued payload), then a real valid frame — all
    /// injected across separate real `write(2)` calls on the pty's
    /// master side so `DispatchSourceRead` actually fires more than
    /// once at OS-chosen granularity, exactly like a live serial link
    /// dropping mid-frame and recovering. Only the valid frame must
    /// come out the other end: this is `StreamFramerTests`'s
    /// `testGarbagePrefixResyncs` / `testOversizeStatedLengthIsDroppedAndStreamRecovers`
    /// (in-memory) proven again across the real fd -> DispatchSourceRead
    /// -> EventHub pipeline this file exists to test.
    func testGarbageAndTruncatedHeaderThenValidFrameResyncThroughRealPTY() async throws {
        let (master, path) = try openPTYPair()
        defer { close(master) }

        let transport = SerialTransport(path: path)
        _ = try await drainEventsWhileConnecting(transport, expected: 2)

        let payload = Data([0x08, 0x2A, 0x10, 0x01])
        let framed = try XCTUnwrap(StreamFramer.frame(payload))

        var framer = StreamFramer()
        var decoded: [Data] = []
        let stream = transport.events()
        let collector = Task {
            for await event in stream {
                if case .received(let raw) = event {
                    decoded.append(contentsOf: framer.feed(raw))
                    if !decoded.isEmpty { break }
                }
            }
        }

        func writeChunk(_ bytes: [UInt8]) {
            bytes.withUnsafeBufferPointer { buf in
                _ = write(master, buf.baseAddress, buf.count)
            }
        }

        // 1. Plain garbage — never matches the magic sequence, discarded
        //    one byte at a time while hunting for 0x94 0xC3.
        writeChunk([0x00, 0xFF, 0x11, 0x22])
        try await Task.sleep(nanoseconds: 10_000_000)

        // 2. A "truncated" header: magic + a declared length (0xFFFF)
        //    that the bytes actually following it do not — and never
        //    will — satisfy. Must resync rather than wait forever (or
        //    misinterpret the valid frame below as its payload).
        writeChunk([0x94, 0xC3, 0xFF, 0xFF])
        try await Task.sleep(nanoseconds: 10_000_000)

        // 3. A real, complete, correctly-framed message.
        writeChunk(Array(framed))

        _ = await withTimeout(seconds: 5) { await collector.value }
        XCTAssertEqual(decoded, [payload], "resync must recover exactly the one valid frame, nothing from the garbage or the truncated header")
        await transport.disconnect()
    }

    func testDisconnectClosesThePortAndSendFailsAfter() async throws {
        let (master, path) = try openPTYPair()
        defer { close(master) }

        let transport = SerialTransport(path: path)
        _ = try await drainEventsWhileConnecting(transport, expected: 2)
        await transport.disconnect()

        do {
            try await transport.send(Data([0x01]))
            XCTFail("send after disconnect should throw")
        } catch {
            // expected: TransportError.notConnected
        }
    }

    // MARK: - helpers

    private func drainEventsWhileConnecting(_ transport: SerialTransport, expected: Int) async throws -> [TransportEvent] {
        let stream = transport.events()
        let collector = Task<[TransportEvent], Never> {
            var out: [TransportEvent] = []
            for await event in stream {
                out.append(event)
                if out.count >= expected { break }
            }
            return out
        }
        try await transport.connect()
        return await withTimeout(seconds: 5) { await collector.value } ?? []
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
}
#endif
