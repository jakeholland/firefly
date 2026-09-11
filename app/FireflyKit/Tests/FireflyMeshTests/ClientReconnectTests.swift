//
//  ClientReconnectTests.swift — M2's background-BLE reconnect behaviour,
//  driven entirely over a MOCKED transport (`LoopbackTransport`'s new
//  `simulateDisconnect()`/`simulateReconnect()`, added for exactly this
//  purpose — see that type's own doc comment). No radio, no CoreBluetooth,
//  no simulator: this is `MeshtasticClient`'s half of docs/specs/
//  A01-companion-app.md's M2 acceptance criterion ("background-connected
//  ...reconnecting after the node is power cycled"), and the client-level
//  analog of "no duplicate CBCentralManager" that only a real
//  `BLETransport` can be exercised for (that half needs real hardware —
//  see `FireflyHardwareTests`).
//
import FireflyMesh
import MeshtasticProto
import XCTest

/// Root-caused against CI run 34606690299 (`testHandshakeFailsHonestlyOnce
/// EveryBoundedRetryIsSpent` timed out "waiting for 3 sent message(s); saw
/// 2" after 12.9s): every `MeshtasticClient` this file constructs drives
/// the real `handleTransportReconnected()` retry loop, and even though
/// every test here already dials `handshakeRetryBaseDelay`/
/// `handshakeRetryMaxDelay` down to milliseconds, that loop used to wait
/// them out with a bare, real `Task.sleep` (`MeshtasticClient
/// .handshakeRetryClock`'s own doc comment) — real wall-clock time a
/// loaded CI runner's cooperative-thread-pool contention can inflate well
/// past even a generous test timeout, no matter how small the nominal
/// delay is. This resolves near-instantly instead, so every test in this
/// file exercises the retry LOOP's own logic (attempt counts, the
/// `.reconnecting`/`.failed` events it publishes) with no real elapsed
/// time riding on it — the pure `handshakeRetryDelay(forAttempt:)` table
/// itself (`testHandshakeRetryDelayDoublesAndCaps`/
/// `testHandshakeRetryDelayWorksAtSubSecondPrecision`, below) is
/// untouched by this and keeps pinning the real durations.
private struct ImmediateHandshakeRetryClock: HandshakeRetryClock {
    func sleep(for duration: Duration) async throws {
        // A real (tiny) suspension, not a busy-loop: lets the actor's
        // other queued work (the transport's next injected frame, the
        // test's own polling) interleave normally, same as a real sleep
        // would, but with no dependency on `duration` actually elapsing.
        try await Task.sleep(for: .zero)
    }
}

final class ClientReconnectTests: XCTestCase {

    // MARK: - FromRadio builders (same shapes as ClientHandshakeTests)

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

    private func nodeInfoFrame(num: UInt32, shortName: String, longName: String) -> Data {
        fromRadio { fr in
            var info = NodeInfo()
            info.num = num
            var user = User()
            user.shortName = shortName
            user.longName = longName
            info.user = user
            fr.nodeInfo = info
        }
    }

    private struct TestTimeout: Error {}

    // 200 * 5ms = 1s worst-case ceiling — the package-wide "no test may
    // sleep more than ~1s total" rule (see `ImmediateHandshakeRetryClock`'s
    // own doc comment). Every `MeshtasticClient` this file constructs now
    // injects that clock, so in a genuinely passing run this loop resolves
    // in a handful of 5ms polls; this ceiling only bounds the FAILURE
    // case, and 1s is plenty to catch a real hang without letting a
    // loaded runner's contention alone stretch it out to CI-timeout-scale
    // like the un-injected retry backoff used to (CI run 34606690299).
    private func waitForSentCount(_ n: Int, on transport: LoopbackTransport, file: StaticString = #filePath, line: UInt = #line) async throws {
        for _ in 0..<200 {
            if transport.sentMessages.count >= n { return }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTFail("timed out waiting for \(n) sent message(s); saw \(transport.sentMessages.count)", file: file, line: line)
        throw TestTimeout()
    }

    @discardableResult
    private func completeHandshake(
        transport: LoopbackTransport, client: MeshtasticClient, myNodeNum: UInt32 = 0x1234
    ) async throws -> Task<Void, Error> {
        let connectTask = Task { try await client.connect() }
        try await waitForSentCount(2, on: transport) // heartbeat, want_config(onlyConfig)
        transport.inject(myInfoFrame(num: myNodeNum))
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyConfig))
        try await waitForSentCount(3, on: transport) // want_config(onlyNodeDB)
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyNodeDB))
        try await connectTask.value
        return connectTask
    }

    // MARK: - Backoff, as a pure function

    func testHandshakeRetryDelayDoublesAndCaps() {
        let base = Duration.seconds(2)
        let cap = Duration.seconds(60)
        XCTAssertEqual(MeshtasticClient.handshakeRetryDelay(forAttempt: 1, base: base, cap: cap), .seconds(2))
        XCTAssertEqual(MeshtasticClient.handshakeRetryDelay(forAttempt: 2, base: base, cap: cap), .seconds(4))
        XCTAssertEqual(MeshtasticClient.handshakeRetryDelay(forAttempt: 3, base: base, cap: cap), .seconds(8))
        XCTAssertEqual(MeshtasticClient.handshakeRetryDelay(forAttempt: 4, base: base, cap: cap), .seconds(16))
        XCTAssertEqual(MeshtasticClient.handshakeRetryDelay(forAttempt: 5, base: base, cap: cap), .seconds(32))
        // 2 * 2^5 = 64s would exceed the 60s cap.
        XCTAssertEqual(MeshtasticClient.handshakeRetryDelay(forAttempt: 6, base: base, cap: cap), .seconds(60))
        XCTAssertEqual(MeshtasticClient.handshakeRetryDelay(forAttempt: 20, base: base, cap: cap), .seconds(60),
                        "must stay bounded, never grow past the cap no matter how many attempts")
    }

    func testHandshakeRetryDelayWorksAtSubSecondPrecision() {
        // Every other test in this file uses millisecond-scale bases so
        // it runs fast — this pins that the conversion does not silently
        // truncate a sub-second `Duration` to a zero delay.
        let delay = MeshtasticClient.handshakeRetryDelay(
            forAttempt: 2, base: .milliseconds(30), cap: .milliseconds(500))
        let seconds = Double(delay.components.seconds) + Double(delay.components.attoseconds) / 1e18
        XCTAssertEqual(seconds, 0.060, accuracy: 0.001)
    }

    // MARK: - connect() reentrancy (the client-level "no duplicate" guard)

    func testConcurrentConnectCallsDoNotDuplicateTheReceiveSubscription() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport, handshakeRetryClock: ImmediateHandshakeRetryClock())

        // The FIRST call's reentrancy guard (`isConnectAttemptInFlight`)
        // is set synchronously, before its first suspension point — a
        // short sleep before racing the SECOND call in behind it is
        // enough to make the second the deterministic loser, exactly
        // the shape `AppGraph`'s launch auto-connect and a manual
        // CONNECT tap can now produce
        // (`AppGraph.autoConnectToLastKnownPeripheral`'s own doc
        // comment).
        let firstTask = Task { try await client.connect() }
        try await Task.sleep(for: .milliseconds(5))

        var secondError: Error?
        do {
            try await client.connect()
        } catch {
            secondError = error
        }
        XCTAssertEqual(secondError as? MeshtasticClientError, .alreadyConnecting,
                        "a second, overlapping connect() must be refused, not silently duplicate the session")

        try await waitForSentCount(2, on: transport)
        transport.inject(myInfoFrame(num: 7))
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyConfig))
        try await waitForSentCount(3, on: transport)
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyNodeDB))
        try await firstTask.value

        // The FIRST call still completes normally, and exactly ONE
        // handshake went out — `sentMessages` would show a doubled
        // want_config storm if the guard had failed to hold.
        XCTAssertEqual(transport.sentMessages.count, 3, "heartbeat + want_config x2, exactly once")
    }

    // MARK: - Reconnect-on-loss: nodeDB rebuilt once, handshake redone once

    func testReconnectAfterLossRebuildsNodeDBOnceAndRedoesTheHandshakeOnce() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport, handshakeRetryClock: ImmediateHandshakeRetryClock())

        let states = client.linkState()
        var readyCount = 0
        let stateCollector = Task {
            for await s in states {
                if s == .ready { readyCount += 1 }
                if readyCount == 2 { break }
            }
        }

        try await completeHandshake(transport: transport, client: client, myNodeNum: 1)
        transport.inject(nodeInfoFrame(num: 42, shortName: "F1", longName: "Friend"))
        try await Task.sleep(for: .milliseconds(20))
        let nodeBeforeLoss = await client.nodeSnapshot(42)
        XCTAssertNotNil(nodeBeforeLoss, "sanity: the node is known before the loss")

        let sentBeforeLoss = transport.sentMessages.count

        // The BLE-level half of "stays connected in a pocket, reconnects
        // on its own": the link drops and comes back on its own — never
        // an explicit `client.disconnect()`.
        transport.simulateDisconnect(reason: "out of range")
        transport.simulateReconnect()

        try await waitForSentCount(sentBeforeLoss + 2, on: transport) // heartbeat, want_config(onlyConfig)
        transport.inject(myInfoFrame(num: 1))
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyConfig))
        try await waitForSentCount(sentBeforeLoss + 3, on: transport) // want_config(onlyNodeDB)
        // Node 42 is deliberately NOT re-announced this time.
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyNodeDB))

        _ = await stateCollector.result
        XCTAssertEqual(readyCount, 2, "the client must reach .ready again after the reconnect")
        XCTAssertEqual(transport.sentMessages.count, sentBeforeLoss + 3,
                        "exactly one handshake retry went out — not a duplicated one")
        let nodeAfterReconnect = await client.nodeSnapshot(42)
        XCTAssertNil(nodeAfterReconnect,
                     "the nodeDB is rebuilt exactly once per reconnect; a node not re-announced must not survive it")
    }

    /// Root-caused 2026-09-11 against a REAL bench power-cycle failure
    /// (`FireflyHardwareTests.testReconnectsOnItsOwnAfterFirefly2IsPowerCycled`,
    /// full log: the node's USB was pulled for ~10s; `BLETransport`
    /// logged `didDisconnectPeripheral ... error=CBErrorDomain Code=6
    /// "The connection has timed out unexpectedly."`; the client logged
    /// `linkState -> disconnected`; then NOTHING — no
    /// `.reconnecting(attempt:)` at all — for the full 180s the
    /// acceptance test bounds itself to). Reproduces the exact LinkState
    /// sequence over the mocked transport (no CoreBluetooth, no radio):
    /// `.ready` -> an UNEXPECTED `.disconnected` (never
    /// `client.disconnect()` — the shape a real
    /// `didDisconnectPeripheral(error: CBError.connectionTimeout)`
    /// produces) -> the node absent for a while -> the node reappears.
    ///
    /// FAILS on pre-fix `main`: `consumeTransportEvents`'s `.disconnected`
    /// case published nothing but `.disconnected` itself, and
    /// `.reconnecting(attempt:)` was ONLY ever published from inside
    /// `handleTransportReconnected()` — reachable only once the transport
    /// has ALREADY reached `.ready` a second time. A client asked to
    /// prove "the UI says something honest while the node is still gone"
    /// could not, by construction, regardless of how long the node stayed
    /// gone. Passes once `consumeTransportEvents` publishes an honest
    /// `.reconnecting(attempt: 1)` the INSTANT an unexpected loss is
    /// detected (`hasCompletedInitialConnect` — the same gate that already
    /// tells a background reconnect apart from the very first connect).
    /// Actor-isolated, not a bare `var` captured by the collector
    /// `Task` below — that `Task` runs concurrently with this test
    /// method's own execution (deliberately: this test reads the
    /// collected states WHILE the node is still absent, well before the
    /// collector task itself ever finishes, so there is no `await task
    /// .result` join point to hang a happens-before relationship off of
    /// the way every OTHER test in this file does). A plain `var [LinkState]`
    /// read from both sides would be a genuine data race.
    private actor LinkStateRecorder {
        private(set) var states: [LinkState] = []
        func record(_ state: LinkState) { states.append(state) }
    }

    func testReconnectingIsPublishedWhileTheNodeIsStillAbsentNotOnlyAfterItReappears() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport, handshakeRetryClock: ImmediateHandshakeRetryClock())

        let states = client.linkState()
        let recorder = LinkStateRecorder()
        let collector = Task {
            for await s in states { await recorder.record(s) }
        }

        try await completeHandshake(transport: transport, client: client, myNodeNum: 1)

        // The BLE-level half of "stays connected in a pocket, reconnects
        // on its own": an UNEXPECTED loss, with the exact reason string
        // shape a real CoreBluetooth `CBError.connectionTimeout` produces
        // — never an explicit `client.disconnect()`.
        transport.simulateDisconnect(reason: "CBErrorDomain Code=6 \"The connection has timed out unexpectedly.\"")

        // The node stays gone for a while — standing in for the bench's
        // own 15-30s reboot window (docs/specs/A01-companion-app.md, M2)
        // at test speed, same "no test may sleep more than ~1s total"
        // ceiling this file's own `waitForSentCount` doc comment holds
        // every other test in it to. NO `simulateReconnect()` yet — the
        // node is still genuinely absent at this point.
        try await Task.sleep(for: .milliseconds(50))

        // Snapshot what has been published SO FAR, before the node ever
        // reappears — on pre-fix `main` this is exactly `[.disconnected]`,
        // the real bench failure's own signature.
        let publishedWhileStillAbsent = await recorder.states
        XCTAssertTrue(
            publishedWhileStillAbsent.contains(where: { if case .reconnecting = $0 { return true }; return false }),
            "expected an honest .reconnecting(attempt:) while the node was still absent, not silence — saw \(publishedWhileStillAbsent)")

        // Now the node reappears — the transport-level half of the
        // reconnect completing — and the handshake redoes as normal.
        let sentBeforeReappearing = transport.sentMessages.count
        transport.simulateReconnect()
        try await waitForSentCount(sentBeforeReappearing + 2, on: transport) // heartbeat, want_config(onlyConfig)
        transport.inject(myInfoFrame(num: 1))
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyConfig))
        try await waitForSentCount(sentBeforeReappearing + 3, on: transport) // want_config(onlyNodeDB)
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyNodeDB))

        // Bounded wait for the second `.ready` — same shape as every
        // other test in this file, just polled through the actor since
        // this test's own recorder gathers every state rather than
        // stopping early.
        for _ in 0..<200 {
            if await recorder.states.filter({ $0 == .ready }).count == 2 { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        collector.cancel()

        let seen = await recorder.states
        XCTAssertEqual(seen.filter { $0 == .ready }.count, 2, "the client must reach .ready again after the reconnect — saw \(seen)")
        // The published sequence is `.connecting -> .handshaking -> .ready`
        // (the INITIAL connect) `-> .disconnected -> .reconnecting(1) ->
        // .handshaking -> .ready` (the reconnect) — so `.reconnecting`
        // must land strictly BETWEEN the first `.ready` and the second
        // one, never before the first (there is nothing to reconnect to
        // yet) and never after the second (that would mean the link
        // said "still trying" even once it was honestly back up).
        guard let firstReadyIndex = seen.firstIndex(of: .ready) else {
            return XCTFail("expected an initial .ready before ever losing the link — saw \(seen)")
        }
        let secondReadyIndex = seen[seen.index(after: firstReadyIndex)...].firstIndex(of: .ready)
        let reconnectingIndex = seen.firstIndex(where: { if case .reconnecting = $0 { return true }; return false })
        XCTAssertNotNil(secondReadyIndex, "expected a SECOND .ready after the reconnect — saw \(seen)")
        XCTAssertNotNil(reconnectingIndex, "expected .reconnecting(attempt:) somewhere in the sequence — saw \(seen)")
        if let reconnectingIndex, let secondReadyIndex {
            XCTAssertGreaterThan(reconnectingIndex, firstReadyIndex,
                                  ".reconnecting must be published AFTER the initial .ready — saw \(seen)")
            XCTAssertLessThan(reconnectingIndex, secondReadyIndex,
                               ".reconnecting must be published before the link reaches .ready again, not after — saw \(seen)")
        }
    }

    /// Two `.ready` events in a row (no `.disconnected` between them) —
    /// a transport is not expected to do this, but the client must not
    /// let a fast double-fire start two concurrent handshake-retry
    /// loops. `reconnectTask`'s cancel-and-replace (`MeshtasticClient
    /// .consumeTransportEvents`'s own doc comment) is what pins this.
    func testRapidDoubleReadyDoesNotStartConcurrentHandshakeAttempts() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport, handshakeRetryClock: ImmediateHandshakeRetryClock())

        let states = client.linkState()
        var readyCount = 0
        let stateCollector = Task {
            for await s in states {
                if s == .ready { readyCount += 1 }
                if readyCount == 2 { break }
            }
        }

        try await completeHandshake(transport: transport, client: client, myNodeNum: 1)
        let sentBeforeLoss = transport.sentMessages.count

        transport.simulateDisconnect()
        transport.simulateReconnect()
        transport.simulateReconnect() // fires again before any reply arrives

        // Only ONE handshake attempt should actually be answerable —
        // whichever `reconnectTask` is still alive once the dust
        // settles. Answer the first (heartbeat, want_config(onlyConfig))
        // sent after the loss; if a duplicate loop had also fired, a
        // SECOND unanswered want_config(onlyConfig) would sit forever
        // and `readyCount` would never reach 2.
        try await waitForSentCount(sentBeforeLoss + 2, on: transport)
        transport.inject(myInfoFrame(num: 1))
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyConfig))
        try await waitForSentCount(sentBeforeLoss + 3, on: transport)
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyNodeDB))

        _ = await stateCollector.result
        XCTAssertEqual(readyCount, 2)
    }

    // MARK: - BLOCKING 1 (PR #272 review): a real reboot arriving MID-RETRY
    // must route through the SAME reconnectTask, not a second, untracked one

    private func rebootedFrame() -> Data {
        fromRadio { fr in fr.rebooted = true }
    }

    /// The exact scenario BLOCKING item 1 named: `FromRadio.rebooted`
    /// arrives WHILE a transport-`.ready`-triggered handshake-retry
    /// attempt is already outstanding (mid-retry — the first attempt has
    /// sent its `want_config` but has not been answered or timed out
    /// yet). Before the fix, `.rebooted` ran through an untracked
    /// `Task { handleRebooted() }` that awaited
    /// `handleTransportReconnected()` INLINE — a second, fully
    /// concurrent handshake-retry loop, each independently calling
    /// `nodeDB.reset()`/`pendingSends.removeAll()` and each sending its
    /// own `want_config`. This pins that exactly ONE new
    /// `want_config(onlyConfig)` round trip follows the reboot (not two,
    /// which a duplicate concurrent loop would produce) and that the
    /// nodeDB was rebuilt exactly once for the post-reboot session (a
    /// node known before the reboot, and never re-announced after it,
    /// must not survive).
    func testRebootArrivingMidRetryRoutesThroughTheSameReconnectTask() async throws {
        let transport = LoopbackTransport()
        // A long config-phase timeout: the reboot must interrupt the
        // FIRST retry attempt while it is still genuinely awaiting a
        // reply, not race a timeout that was about to fire anyway.
        let client = MeshtasticClient(
            transport: transport,
            configPhaseTimeout: .seconds(5),
            nodeDBPhaseTimeout: .seconds(5),
            handshakeRetryLimit: 3,
            handshakeRetryBaseDelay: .milliseconds(30),
            handshakeRetryMaxDelay: .milliseconds(300),
            handshakeRetryClock: ImmediateHandshakeRetryClock())

        let states = client.linkState()
        var readyCount = 0
        let stateCollector = Task {
            for await s in states {
                if s == .ready { readyCount += 1 }
                if readyCount == 2 { break }
            }
        }

        try await completeHandshake(transport: transport, client: client, myNodeNum: 1)
        transport.inject(nodeInfoFrame(num: 42, shortName: "F1", longName: "Friend"))
        try await Task.sleep(for: .milliseconds(20))
        let nodeBeforeLoss = await client.nodeSnapshot(42)
        XCTAssertNotNil(nodeBeforeLoss, "sanity: the node is known before the loss")

        let sentBeforeLoss = transport.sentMessages.count

        // The transport reconnects on its own — `reconnectTask` attempt 1
        // starts: heartbeat + want_config(onlyConfig), deliberately never
        // answered.
        transport.simulateDisconnect(reason: "out of range")
        transport.simulateReconnect()
        try await waitForSentCount(sentBeforeLoss + 2, on: transport)

        // A REAL reboot lands WHILE that first retry attempt is still
        // outstanding — mid-retry, exactly BLOCKING item 1's scenario.
        transport.inject(rebootedFrame())

        // Exactly ONE new want_config(onlyConfig) round trip follows —
        // not two, which a second, untracked concurrent retry loop would
        // produce.
        try await waitForSentCount(sentBeforeLoss + 4, on: transport) // heartbeat, want_config(onlyConfig)
        transport.inject(myInfoFrame(num: 1))
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyConfig))
        try await waitForSentCount(sentBeforeLoss + 5, on: transport) // want_config(onlyNodeDB)
        // Node 42 is deliberately NOT re-announced this time.
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyNodeDB))

        _ = await stateCollector.result
        XCTAssertEqual(readyCount, 2, "the client must reach .ready again after the post-reboot handshake")
        XCTAssertEqual(transport.sentMessages.count, sentBeforeLoss + 5,
                        "exactly one want_config handshake followed the mid-retry reboot — " +
                        "a duplicate concurrent retry loop would send more")
        let nodeAfterReboot = await client.nodeSnapshot(42)
        XCTAssertNil(nodeAfterReboot, "nodeDB.reset() ran exactly once for the post-reboot session — " +
                     "a node from before it, never re-announced, must not survive")
    }

    // MARK: - Bounded exponential backoff, end to end

    func testHandshakeTimeoutRetriesWithBackoffPublishingReconnectingThenSucceeds() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(
            transport: transport,
            configPhaseTimeout: .milliseconds(60),
            nodeDBPhaseTimeout: .seconds(5),
            handshakeRetryLimit: 3,
            handshakeRetryBaseDelay: .milliseconds(30),
            handshakeRetryMaxDelay: .milliseconds(300),
            handshakeRetryClock: ImmediateHandshakeRetryClock())

        let states = client.linkState()
        var seen: [LinkState] = []
        var readyCount = 0
        let collector = Task {
            for await s in states {
                seen.append(s)
                if s == .ready { readyCount += 1 }
                if readyCount == 2 { break }
            }
        }

        try await completeHandshake(transport: transport, client: client, myNodeNum: 1)
        let sentBeforeLoss = transport.sentMessages.count

        transport.simulateDisconnect()
        transport.simulateReconnect()

        // Attempt 1: heartbeat + want_config(onlyConfig) — deliberately
        // never answered, so `configPhaseTimeout` (60ms) fires and the
        // client retries rather than failing outright.
        try await waitForSentCount(sentBeforeLoss + 2, on: transport)

        // Attempt 2, after the ~30ms backoff: heartbeat + want_config
        // (onlyConfig) again — THIS one is answered, through to .ready.
        try await waitForSentCount(sentBeforeLoss + 4, on: transport)
        transport.inject(myInfoFrame(num: 1))
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyConfig))
        try await waitForSentCount(sentBeforeLoss + 5, on: transport)
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyNodeDB))

        _ = await collector.result
        XCTAssertEqual(readyCount, 2)
        XCTAssertTrue(seen.contains(.reconnecting(attempt: 2)),
                      "a retried handshake attempt must be reported honestly, not silently as .handshaking")
    }

    func testHandshakeFailsHonestlyOnceEveryBoundedRetryIsSpent() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(
            transport: transport,
            configPhaseTimeout: .milliseconds(30),
            nodeDBPhaseTimeout: .seconds(5),
            handshakeRetryLimit: 2, // one initial attempt + one retry, then give up
            handshakeRetryBaseDelay: .milliseconds(10),
            handshakeRetryMaxDelay: .milliseconds(50),
            handshakeRetryClock: ImmediateHandshakeRetryClock())

        let states = client.linkState()
        var seen: [LinkState] = []
        let collector = Task {
            for await s in states {
                seen.append(s)
                if case .failed = s { break }
            }
        }

        try await completeHandshake(transport: transport, client: client, myNodeNum: 1)

        // Never answered at all — both attempts time out.
        transport.simulateDisconnect()
        transport.simulateReconnect()

        _ = await collector.result
        XCTAssertTrue(seen.contains(.reconnecting(attempt: 2)))
        guard case .failed = seen.last else {
            return XCTFail("expected the bounded retry loop to end in .failed, saw \(seen.last as Any)")
        }
    }
}
