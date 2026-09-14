//
//  ClientRestorationTests.swift — A03_AC3, the restoration path's
//  CLIENT half, over a mocked transport.
//
//  §5 calls A03_AC3 "the single most important automated test in this
//  spec", and audit 2.2.2 is what it closes: `MeshtasticClient`
//  subscribed to the transport only from inside `connect()`, so on a
//  CoreBluetooth background relaunch — where nothing calls `connect()`
//  at all — the restored session's `.ready` reached nobody and the app
//  sat with a live BLE link and no handshake, forever.
//
//  Both orderings are exercised, because the restore and the client's
//  own attach genuinely race on a background relaunch:
//   * the `.ready` arrives AFTER `beginListening()` subscribed, and
//   * the session was already up BEFORE it did (`isLinkReady`).
//
import FireflyMesh
import MeshtasticProto
import XCTest

/// Same injection `ClientReconnectTests` uses and for the same reason
/// (see that file's own doc comment): the handshake-retry loop's real
/// backoff is real wall-clock time a loaded CI runner can stretch, and
/// nothing in this file is testing the DURATIONS — the pure
/// `handshakeRetryDelay(forAttempt:base:cap:)` table is pinned there.
private struct ImmediateHandshakeRetryClock: HandshakeRetryClock {
    func sleep(for duration: Duration) async throws {
        try await Task.sleep(for: .zero)
    }
}

@MainActor
final class ClientRestorationTests: XCTestCase {

    private func fromRadio(_ build: (inout FromRadio) -> Void) -> Data {
        var fr = FromRadio()
        build(&fr)
        return (try? fr.serializedData()) ?? Data()
    }

    private func myInfoFrame(num: UInt32) -> Data {
        fromRadio { fr in
            var info = MyNodeInfo()
            info.myNodeNum = num
            fr.myInfo = info
        }
    }

    private func configCompleteFrame(_ id: UInt32) -> Data {
        fromRadio { fr in fr.configCompleteID = id }
    }

    private func nodeInfoFrame(num: UInt32, shortName: String) -> Data {
        fromRadio { fr in
            var info = NodeInfo()
            info.num = num
            var user = User()
            user.shortName = shortName
            user.longName = shortName
            info.user = user
            fr.nodeInfo = info
        }
    }

    /// Bounded, and cancellation-safe, for the reasons
    /// `ClientReconnectTests.waitForSentCount` documents at length: a
    /// polling wait is real wall-clock time that eats into the client's
    /// own `configPhaseTimeout`, and an unbounded one turns a regression
    /// into a hang with no diagnostic.
    private func waitForSentCount(_ n: Int, on transport: LoopbackTransport,
                                   timeout: Duration = .seconds(10),
                                   file: StaticString = #filePath, line: UInt = #line) async throws {
        let waiter = Task { try await transport.waitForSentCount(n) }
        let watchdog = Task {
            try? await Task.sleep(for: timeout)
            waiter.cancel()
        }
        defer { watchdog.cancel() }
        do {
            try await waiter.value
        } catch {
            XCTFail("timed out waiting for \(n) sent message(s); saw \(transport.sentMessages.count)",
                     file: file, line: line)
            throw error
        }
    }

    private func waitForCollector(_ task: Task<Void, Never>, timeout: Duration = .seconds(10)) async {
        let watchdog = Task {
            try? await Task.sleep(for: timeout)
            task.cancel()
        }
        _ = await task.value
        watchdog.cancel()
    }

    /// Drives the two want_config phases a handshake actually sends,
    /// from whatever the transport's send count was before it started.
    private func answerHandshake(transport: LoopbackTransport, sentBefore: Int,
                                 myNodeNum: UInt32, nodeDB: [Data] = []) async throws {
        try await waitForSentCount(sentBefore + 2, on: transport) // heartbeat, want_config(onlyConfig)
        transport.inject(myInfoFrame(num: myNodeNum))
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyConfig))
        try await waitForSentCount(sentBefore + 3, on: transport) // want_config(onlyNodeDB)
        for frame in nodeDB { transport.inject(frame) }
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyNodeDB))
    }

    // MARK: - A03_AC3

    /// **A03_AC3.** A transport `.ready` that arrives with NO `connect()`
    /// continuation outstanding starts a handshake and drives the client
    /// to `.ready`, with the nodeDB rebuilt exactly once.
    ///
    /// This is a background relaunch, end to end on the client side:
    /// nothing ever calls `connect()`, because nothing on that path
    /// does — iOS relaunched the process and CoreBluetooth handed the
    /// session back. On pre-S1b `main` this test hangs at the first
    /// `waitForSentCount`: the `.ready` branch was gated on
    /// `hasCompletedInitialConnect`, which a relaunched process has
    /// never set.
    func testA03_AC3_AReadyNobodyIsAwaitingRunsTheHandshakeAndRebuildsTheNodeDBOnce() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport, handshakeRetryClock: ImmediateHandshakeRetryClock())

        let states = client.linkState()
        let collector = Task {
            for await state in states where state == .ready { break }
        }

        // The whole of the app's involvement on a background relaunch:
        // `AppGraph.start()` attaches a listener. No `connect()`.
        await client.beginListening()
        transport.simulateReconnect()

        try await answerHandshake(transport: transport, sentBefore: 0, myNodeNum: 0x1234,
                                  nodeDB: [nodeInfoFrame(num: 42, shortName: "F1")])

        await waitForCollector(collector)
        let nodeNum = client.connectedNodeNum
        XCTAssertEqual(nodeNum, 0x1234, "the handshake ran: my_info arrived and was kept")
        let node = await client.nodeSnapshot(42)
        XCTAssertNotNil(node, "the nodeDB was rebuilt from the restored session's own want_config")
        XCTAssertEqual(transport.sentMessages.count, 3,
                        "exactly ONE handshake went out — heartbeat + two want_config phases, not a duplicated pair")
    }

    /// The other ordering, and the one a listener alone cannot fix: the
    /// restore adopted the session and it reached `.ready` BEFORE this
    /// client existed. `EventHub` is multicast but never replayed (S1),
    /// so that event is simply gone — `beginListening()` has to ASK.
    func testASessionAlreadyUpBeforeTheClientAttachedStillGetsAHandshake() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport, handshakeRetryClock: ImmediateHandshakeRetryClock())

        // Adopted during the launch cycle, with nobody listening: no
        // `.ready` event is published at all here, deliberately.
        transport.simulateRestoredSessionWithNoListener()

        let states = client.linkState()
        let collector = Task {
            for await state in states where state == .ready { break }
        }

        await client.beginListening()

        try await answerHandshake(transport: transport, sentBefore: 0, myNodeNum: 0x4321)
        await waitForCollector(collector)
        let nodeNum = client.connectedNodeNum
        XCTAssertEqual(nodeNum, 0x4321)
    }

    /// Attaching to a transport that is NOT up does nothing at all — no
    /// handshake, no traffic. A client that speculatively handshakes an
    /// absent radio would spend a background wake's whole ~10 s budget
    /// (§1.1) on a want_config nobody can answer.
    func testAttachingToATransportThatIsNotUpSendsNothing() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport, handshakeRetryClock: ImmediateHandshakeRetryClock())

        await client.beginListening()
        try await Task.sleep(for: .milliseconds(150))

        XCTAssertEqual(transport.sentMessages.count, 0)
        let nodeNum = client.connectedNodeNum
        XCTAssertNil(nodeNum)
    }

    /// The riskiest edit in this slice is that the `.ready` gate changed
    /// under the M1 connect path, so this pins that path unchanged: an
    /// ORDINARY `connect()` — after `beginListening()` has already
    /// attached, which is now what every launch does — still runs
    /// exactly one handshake, not two.
    ///
    /// The failure this forbids is not hypothetical: the `.ready` an
    /// explicit connect produces is delivered through the event stream
    /// and can be processed after `connect()` has already returned, at
    /// which point a gate that merely asks "has this client connected
    /// before?" answers yes and starts a second want_config.
    func testAnOrdinaryConnectAfterBeginListeningStillRunsExactlyOneHandshake() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport, handshakeRetryClock: ImmediateHandshakeRetryClock())

        await client.beginListening()
        let connectTask = Task { try await client.connect() }
        try await answerHandshake(transport: transport, sentBefore: 0, myNodeNum: 7)
        try await connectTask.value

        // Long enough for a duplicated handshake to have gone out.
        try await Task.sleep(for: .milliseconds(200))
        XCTAssertEqual(transport.sentMessages.count, 3,
                        "one handshake: heartbeat + two want_config phases")
        let nodeNum = client.connectedNodeNum
        XCTAssertEqual(nodeNum, 7)
    }

    /// …and a session that came back through the RESTORED path is a
    /// session that has reached `.ready`, so the next loss publishes an
    /// honest `.reconnecting` rather than silence. Before S1b nothing
    /// set `hasCompletedInitialConnect` on this path — it could only be
    /// reached after a `connect()` had already set it — so a restored
    /// session's first drop looked exactly like having given up.
    func testALossAfterARestoredSessionPublishesReconnecting() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport, handshakeRetryClock: ImmediateHandshakeRetryClock())

        // Two independent subscriptions (`linkState()` is multicast —
        // S1), so the second one cannot miss anything while the first is
        // still running: the loss must not be injected until the
        // restored session has genuinely reached `.ready`, or this test
        // would be measuring a handshake interrupted mid-flight rather
        // than a reconnect. `connectedNodeNum` is NOT that signal — it
        // becomes non-nil the moment my_info arrives, which is one
        // want_config phase before the handshake is done.
        let readyStates = client.linkState()
        let reconnectingStates = client.linkState()
        let readyCollector = Task {
            for await state in readyStates where state == .ready { break }
        }
        let reconnectingCollector = Task {
            for await state in reconnectingStates {
                if case .reconnecting = state { break }
            }
        }

        await client.beginListening()
        transport.simulateReconnect()
        try await answerHandshake(transport: transport, sentBefore: 0, myNodeNum: 9)
        await waitForCollector(readyCollector)
        XCTAssertTrue(readyCollector.isCancelled == false, "the restored session reached .ready")

        transport.simulateDisconnect(reason: "out of range")
        await waitForCollector(reconnectingCollector)
        XCTAssertFalse(reconnectingCollector.isCancelled,
                        "a loss after a RESTORED session must publish .reconnecting, not silence")
    }
}

/// A `LoopbackTransport` whose `.ready` is held back until the test
/// releases it, so the ONE ordering neither `LoopbackTransport` nor a
/// real `BLETransport` will produce on demand can be driven
/// deterministically: the `.ready` an explicit `connect()` produced,
/// processed by the event listener AFTER `connect()` has already
/// returned.
///
/// That ordering is the "older race" A03_AC3's own notes claim the new
/// `.ready` gate closes, and nothing else in this file reaches it —
/// `LoopbackTransport` publishes `.ready` from inside `connect()`, where
/// the listener always gets its turn during the handshake and the OLD
/// `hasCompletedInitialConnect` gate therefore still reads `false`.
/// Everything else is delegated to a real `LoopbackTransport`, so the
/// handshake under test is the real one.
///
/// PR #317 review (Tier 3): added because reverting the gate to
/// `hasCompletedInitialConnect` left every existing test in this file
/// green except A03_AC3's own, which means the duplicate-handshake half
/// of the claim was asserted and not pinned.
private final class LateReadyTransport: MeshTransport, @unchecked Sendable {
    private let inner = LoopbackTransport()
    private let lock = NSLock()
    private var continuations: [AsyncStream<TransportEvent>.Continuation] = []
    private var readyHeld = false
    private var readyReleased = false
    private var forwarder: Task<Void, Never>?

    var kind: TransportKind { inner.kind }
    var sentMessages: [Data] { inner.sentMessages }
    func inject(_ data: Data) { inner.inject(data) }
    func waitForSentCount(_ n: Int) async throws { try await inner.waitForSentCount(n) }

    func events() -> AsyncStream<TransportEvent> {
        let (stream, continuation) = AsyncStream<TransportEvent>.makeStream(
            bufferingPolicy: .bufferingNewest(4096))
        addContinuation(continuation)
        startForwardingIfNeeded()
        return stream
    }

    func connect() async throws { try await inner.connect() }
    func disconnect() async { await inner.disconnect() }
    func send(_ data: Data) async throws { try await inner.send(data) }
    var isLinkReady: Bool { get async { await inner.isLinkReady } }

    /// Deliver the `.ready` that was held back. Called after
    /// `client.connect()` has returned, which is the whole point.
    func releaseReady() {
        lock.lock()
        let held = readyHeld
        readyHeld = false
        readyReleased = true
        let targets = continuations
        lock.unlock()
        guard held else { return }
        for continuation in targets { continuation.yield(.ready) }
    }

    // MARK: - Plumbing (synchronous, because `NSLock` may not be held
    // across a suspension point — the same shape `LoopbackTransport
    // .record(_:)` uses).

    private func addContinuation(_ continuation: AsyncStream<TransportEvent>.Continuation) {
        lock.lock(); continuations.append(continuation); lock.unlock()
    }

    private func startForwardingIfNeeded() {
        lock.lock()
        let alreadyRunning = forwarder != nil
        lock.unlock()
        guard !alreadyRunning else { return }
        let stream = inner.events()
        let task = Task { [weak self] in
            for await event in stream {
                guard let self else { return }
                self.forward(event)
            }
        }
        lock.lock(); forwarder = task; lock.unlock()
    }

    private func forward(_ event: TransportEvent) {
        lock.lock()
        if case .ready = event, !readyReleased {
            readyHeld = true
            lock.unlock()
            return
        }
        let targets = continuations
        lock.unlock()
        for continuation in targets { continuation.yield(event) }
    }
}

extension ClientRestorationTests {

    /// The "older race" A03_AC3's notes name, driven rather than
    /// asserted: the `.ready` an explicit `connect()` produced is
    /// processed by the listener AFTER `connect()` has already returned.
    ///
    /// Under the OLD `hasCompletedInitialConnect` gate that `.ready`
    /// reads as "this client has connected before, so this must be a
    /// reconnect" and starts a SECOND want_config against a link that is
    /// already up. Under the new "is a `connect()` awaiting this?" gate
    /// the claim is still outstanding, the event is consumed, and
    /// nothing extra goes out.
    func testAReadyProcessedAfterConnectReturnedDoesNotStartASecondHandshake() async throws {
        let transport = LateReadyTransport()
        let client = MeshtasticClient(transport: transport, handshakeRetryClock: ImmediateHandshakeRetryClock())

        await client.beginListening()
        let connectTask = Task { try await client.connect() }
        // The handshake runs with the transport's `.ready` still held
        // back, exactly as it does today when the listener simply has
        // not had its turn yet.
        try await waitForSentCount(2, on: transport)
        transport.inject(myInfoFrame(num: 0x2468))
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyConfig))
        try await waitForSentCount(3, on: transport)
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyNodeDB))
        try await connectTask.value

        // `connect()` has returned — NOW the listener sees its `.ready`.
        transport.releaseReady()
        try await Task.sleep(for: .milliseconds(250))

        XCTAssertEqual(transport.sentMessages.count, 3,
                        "one handshake: heartbeat + two want_config phases — a `.ready` processed after " +
                        "connect() returned must not start a second one")
    }

    /// Bounded wait against the wrapper, same shape as the
    /// `LoopbackTransport` one above and for the same reasons.
    private func waitForSentCount(_ n: Int, on transport: LateReadyTransport,
                                   timeout: Duration = .seconds(10),
                                   file: StaticString = #filePath, line: UInt = #line) async throws {
        let waiter = Task { try await transport.waitForSentCount(n) }
        let watchdog = Task {
            try? await Task.sleep(for: timeout)
            waiter.cancel()
        }
        defer { watchdog.cancel() }
        do {
            try await waiter.value
        } catch {
            XCTFail("timed out waiting for \(n) sent message(s); saw \(transport.sentMessages.count)",
                     file: file, line: line)
            throw error
        }
    }
}
