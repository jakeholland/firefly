//
//  ClientHandshakeTests.swift — drives `MeshtasticClient`'s handshake
//  and routing-ack → delivery-state translation with injected
//  `FromRadio` bytes over `LoopbackTransport`. No radio, no simulator.
//
//  Covers the spec's three handshake absence/edge rules
//  (docs/specs/A01-companion-app.md, "Handshake" and slice A's own
//  "Must add" list): both phases via the two firmware sentinel nonces,
//  a `config_complete_id` matching neither sentinel, and `rebooted`
//  mid-session — plus routing-ack → delivery-state, including the
//  broadcast case and the (render-time-only) no-ack window.
//
import FireflyMesh
import MeshtasticProto
import XCTest

final class ClientHandshakeTests: XCTestCase {

    // MARK: - FromRadio builders

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

    private func metadataFrame(firmware: String) -> Data {
        fromRadio { fr in
            var meta = DeviceMetadata()
            meta.firmwareVersion = firmware
            fr.metadata = meta
        }
    }

    private func configCompleteFrame(_ id: UInt32) -> Data {
        fromRadio { fr in fr.configCompleteID = id }
    }

    private func nodeInfoFrame(num: UInt32, shortName: String? = nil, longName: String? = nil) -> Data {
        fromRadio { fr in
            var info = NodeInfo()
            info.num = num
            if let shortName, let longName {
                var user = User()
                user.shortName = shortName
                user.longName = longName
                info.user = user
            }
            fr.nodeInfo = info
        }
    }

    private func rebootedFrame() -> Data {
        fromRadio { fr in fr.rebooted = true }
    }

    private func routingFrame(requestID: UInt32, ok: Bool) -> Data {
        fromRadio { fr in
            var packet = MeshPacket()
            var data = DataMessage()
            data.portnum = .routingApp
            data.requestID = requestID
            var routing = Routing()
            if !ok { routing.errorReason = .noRoute }
            data.payload = (try? routing.serializedData()) ?? Data()
            packet.decoded = data
            fr.packet = packet
        }
    }

    // MARK: - Test helpers

    /// Polls `transport.sentMessages.count` instead of a blind sleep, so
    /// these tests are fast and not racy against the actor's own
    /// scheduling.
    private func waitForSentCount(_ n: Int, on transport: LoopbackTransport, file: StaticString = #filePath, line: UInt = #line) async throws {
        for _ in 0..<400 {
            if transport.sentMessages.count >= n { return }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTFail("timed out waiting for \(n) sent message(s); saw \(transport.sentMessages.count)", file: file, line: line)
        throw TestTimeout()
    }

    private struct TestTimeout: Error {}

    /// Drives a full, successful two-phase handshake: heartbeat,
    /// want_config(onlyConfig) → my_info/metadata/config_complete,
    /// want_config(onlyNodeDB) → config_complete. Leaves the client
    /// `.ready`.
    @discardableResult
    private func completeHandshake(
        transport: LoopbackTransport, client: MeshtasticClient, myNodeNum: UInt32 = 0x1234
    ) async throws -> Task<Void, Error> {
        let connectTask = Task { try await client.connect() }
        try await waitForSentCount(2, on: transport) // heartbeat, want_config(onlyConfig)
        transport.inject(myInfoFrame(num: myNodeNum))
        transport.inject(metadataFrame(firmware: "2.7.26"))
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyConfig))
        try await waitForSentCount(3, on: transport) // want_config(onlyNodeDB)
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyNodeDB))
        try await connectTask.value
        return connectTask
    }

    // MARK: - Handshake

    func testCompletesBothPhasesAndReachesReady() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)

        let states = client.linkState()
        var seen: [LinkState] = []
        let collector = Task {
            for await s in states {
                seen.append(s)
                if s == .ready { break }
            }
        }

        try await completeHandshake(transport: transport, client: client, myNodeNum: 48621524)
        transport.inject(nodeInfoFrame(num: 0x02e6_06b0, shortName: "F1", longName: "Firefly 1"))
        _ = await collector.result

        XCTAssertEqual(seen, [.connecting, .handshaking, .ready])
        let myNum = await client.currentMyNodeNum
        XCTAssertEqual(myNum, 48621524)
    }

    /// PR #265 review, should-fix: `disconnect()` must clear
    /// `myNodeNum`/`connectedNodeNum`, not just tear down the transport
    /// and the two background tasks. Before the fix, `connectedNodeNum`
    /// kept reporting the LAST session's node between a disconnect and
    /// the next successful handshake — `PhoneGPSUplink
    /// .destinationNodeNum` and Diagnostics both read it synchronously,
    /// so a phone fix arriving in that window would have gone to a node
    /// this client is no longer connected to.
    func testDisconnectClearsConnectedNodeNum() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)

        try await completeHandshake(transport: transport, client: client, myNodeNum: 48621524)
        XCTAssertEqual(client.connectedNodeNum, 48621524)

        await client.disconnect()

        XCTAssertNil(client.connectedNodeNum, "disconnect must clear connectedNodeNum, not just stop the transport")
    }

    /// A01_AC4-shaped, without hardware: the node dump populates the
    /// nodeDB, and only AFTER config_complete for phase B.
    func testNodeDBPopulatesFromPhaseBDump() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)

        let connectTask = Task { try await client.connect() }
        try await waitForSentCount(2, on: transport)
        transport.inject(myInfoFrame(num: 1))
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyConfig))
        try await waitForSentCount(3, on: transport)
        transport.inject(nodeInfoFrame(num: 0x02e6_06b0, shortName: "F1", longName: "Firefly 1"))
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyNodeDB))
        try await connectTask.value

        let node = await client.nodeSnapshot(0x02e6_06b0)
        XCTAssertEqual(node?.shortName, "F1")
        XCTAssertEqual(node?.longName, "Firefly 1")
    }

    /// A `config_complete_id` that matches NEITHER sentinel must be
    /// ignored — the handshake keeps waiting rather than completing on
    /// the wrong signal.
    func testConfigCompleteIDMatchingNeitherSentinelIsIgnored() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport, configPhaseTimeout: .seconds(10))

        let connectTask = Task { try await client.connect() }
        try await waitForSentCount(2, on: transport)

        transport.inject(configCompleteFrame(424_242)) // neither 69420 nor 69421
        // A short, fixed wait — long enough for the wrong-nonce event to
        // be dispatched, short enough that phase A's real 10s timeout
        // could not have fired on its own. The handshake must still be
        // sitting on phase A (want_config(onlyConfig) was its only send)
        // rather than having advanced or completed.
        try await Task.sleep(for: .milliseconds(150))
        XCTAssertEqual(transport.sentMessages.count, 2,
                        "a config_complete_id matching neither sentinel must not advance the handshake")

        // The correct sentinel still works afterward.
        transport.inject(myInfoFrame(num: 1))
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyConfig))
        try await waitForSentCount(3, on: transport)
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyNodeDB))
        try await connectTask.value
    }

    /// A phase that never completes must time out with the phase's own
    /// sentinel named in the error — not hang forever.
    func testPhaseATimesOutWithNoCompletion() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport, configPhaseTimeout: .milliseconds(80),
                                       nodeDBPhaseTimeout: .seconds(5))
        do {
            try await client.connect()
            XCTFail("expected a handshake timeout")
        } catch let error as MeshtasticClientError {
            XCTAssertEqual(error, .handshakeTimeout(phase: MeshtasticConfigNonce.onlyConfig))
        }
    }

    /// `FromRadio.rebooted` is an immediate session loss: the client
    /// drops straight into a fresh handshake using the SAME two
    /// sentinels, not fresh ones (docs/specs/A01-companion-app.md,
    /// "Handshake").
    func testRebootedReissuesBothPhasesFromScratch() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)

        let states = client.linkState()
        var readyCount = 0
        var handshakingCount = 0
        let collector = Task {
            for await s in states {
                if s == .ready { readyCount += 1 }
                if s == .handshaking { handshakingCount += 1 }
                if readyCount == 2 { break }
            }
        }

        try await completeHandshake(transport: transport, client: client, myNodeNum: 1)

        // Mid-session reboot.
        transport.inject(rebootedFrame())
        try await waitForSentCount(5, on: transport) // + heartbeat, want_config(onlyConfig) again
        transport.inject(myInfoFrame(num: 1))
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyConfig))
        try await waitForSentCount(6, on: transport)
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyNodeDB))

        _ = await collector.result
        XCTAssertEqual(readyCount, 2)
        XCTAssertEqual(handshakingCount, 2)
    }

    /// The node database is rebuilt each handshake (M1): a node known
    /// before a reboot must not silently survive a fresh handshake that
    /// never re-announces it.
    func testRebootedRebuildsNodeDBFromScratch() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)

        try await completeHandshake(transport: transport, client: client, myNodeNum: 1)
        transport.inject(nodeInfoFrame(num: 42, shortName: "F1", longName: "Friend"))
        try await Task.sleep(for: .milliseconds(20))
        var node = await client.nodeSnapshot(42)
        XCTAssertNotNil(node)

        transport.inject(rebootedFrame())
        try await waitForSentCount(5, on: transport)
        transport.inject(myInfoFrame(num: 1))
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyConfig))
        try await waitForSentCount(6, on: transport)
        // No re-announcement of node 42 this time.
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyNodeDB))
        try await Task.sleep(for: .milliseconds(20))

        node = await client.nodeSnapshot(42)
        XCTAssertNil(node, "a node not re-announced after a reboot must not survive the rebuild")
    }

    // MARK: - Routing ACK → delivery state

    /// Maps a `DeliveryEvent` onto the bare `DeliveryState` label the
    /// old tuple-shaped `deliveryUpdates()` used to report — keeps these
    /// tests' assertions reading the same way after PR #264's review
    /// (BLOCKING item 1: adopt slice B's `OutboxID`/`PacketID`/
    /// `DeliveryEvent` split) while still exercising the real event
    /// shape end to end.
    private func label(_ event: DeliveryEvent) -> DeliveryState {
        switch event {
        case .waiting: return .waiting
        case .sent: return .sent
        case .delivered: return .delivered
        case .noAck: return .noAck
        case .dropped: return .dropped
        }
    }

    func testDirectMessageDeliveredOnRoutingAck() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        try await completeHandshake(transport: transport, client: client)

        let deliveries = client.deliveryUpdates()
        var seen: [DeliveryState] = []
        let collector = Task {
            for await event in deliveries {
                let state = label(event)
                seen.append(state)
                if state == .delivered { break }
            }
        }

        let id = try await client.sendText("WHERE", to: 0x02e6_06b0, wantAck: true)
        transport.inject(routingFrame(requestID: id, ok: true))
        _ = await collector.result

        XCTAssertEqual(seen, [.waiting, .sent, .delivered])
    }

    /// A routing NAK answers a packet id that WAS accepted and sent —
    /// the honest terminal state is NO ACK, not DROPPED (PR #264 review,
    /// BLOCKING item 1's partitioning: `.dropped` is reserved for an
    /// outbox-full eviction or a send the transport refused outright,
    /// neither of which ever got a packet id).
    func testDirectMessageNoAckOnRoutingNak() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        try await completeHandshake(transport: transport, client: client)

        let deliveries = client.deliveryUpdates()
        var seen: [DeliveryState] = []
        let collector = Task {
            for await event in deliveries {
                let state = label(event)
                seen.append(state)
                if state == .noAck { break }
            }
        }

        let id = try await client.sendText("WHERE", to: 0x02e6_06b0, wantAck: true)
        transport.inject(routingFrame(requestID: id, ok: false))
        _ = await collector.result

        XCTAssertEqual(seen, [.waiting, .sent, .noAck])
    }

    /// Nobody acks a broadcast: "delivered to the mesh" is not delivery
    /// to a person, so a broadcast must never be promoted past SENT even
    /// if a routing packet somehow references its id.
    func testBroadcastNeverPromotedPastSent() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        try await completeHandshake(transport: transport, client: client)

        let deliveries = client.deliveryUpdates()
        var seen: [DeliveryState] = []
        let collector = Task {
            for await event in deliveries { seen.append(label(event)) }
        }

        let id = try await client.sendText("HERE", to: meshBroadcastAddress, wantAck: true)
        transport.inject(routingFrame(requestID: id, ok: true))
        try await Task.sleep(for: .milliseconds(100))
        collector.cancel()

        XCTAssertEqual(seen, [.waiting, .sent])
        XCTAssertFalse(seen.contains(.delivered))
    }

    // MARK: - The no-ack window (render-time only, not a client timer)

    func testNoAckWindowIsRenderTimeOnly() {
        let longAgo = Date().addingTimeInterval(-(MeshtasticClient.noAckWindow + 1))
        let justNow = Date()

        XCTAssertEqual(
            MeshtasticClient.renderedDeliveryState(base: .sent, wantAck: true, isBroadcast: false, sentAt: longAgo, now: Date()),
            .noAck)
        XCTAssertEqual(
            MeshtasticClient.renderedDeliveryState(base: .sent, wantAck: true, isBroadcast: false, sentAt: justNow, now: Date()),
            .sent)
        // A broadcast never becomes NO ACK, no matter how old.
        XCTAssertEqual(
            MeshtasticClient.renderedDeliveryState(base: .sent, wantAck: false, isBroadcast: true, sentAt: longAgo, now: Date()),
            .sent)
        // A non-SENT base (e.g. already DELIVERED/DROPPED) is untouched.
        XCTAssertEqual(
            MeshtasticClient.renderedDeliveryState(base: .delivered, wantAck: true, isBroadcast: false, sentAt: longAgo, now: Date()),
            .delivered)
    }
}
