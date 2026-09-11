//
//  ClientPositionAndPrivateTests.swift — the two wire surfaces M1
//  integration added to `MeshtasticClient`: the phone-GPS uplink
//  (`POSITION_APP` / `LOC_EXTERNAL`, A01_AC9) and Firefly's own private
//  portnum 269 (S04), in both directions.
//
//  Everything here goes over `LoopbackTransport`: the bytes the client
//  writes are read back and decoded, so these assert the ACTUAL wire
//  format rather than that a function was called.
//
import FireflyMesh
import MeshtasticProto
import XCTest

// M3 / Swift 6: `@MainActor` so a `Task { ... }` created inside a
// test method (collecting an `AsyncStream` off a transport/client) can
// capture `self` without a 'sending closure risks data races' diagnostic
// — the closure is then isolated to the same actor `self` already is,
// not crossing an isolation boundary at all. XCTest already runs a
// test class's methods serially, so this changes no test's behavior.
@MainActor
final class ClientPositionAndPrivateTests: XCTestCase {


    // MARK: - Handshake helper

    private struct TestTimeout: Error {}

    private func waitForSentCount(_ n: Int, on transport: LoopbackTransport) async throws {
        for _ in 0..<400 {
            if transport.sentMessages.count >= n { return }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTFail("timed out waiting for \(n) sent message(s); saw \(transport.sentMessages.count)")
        throw TestTimeout()
    }

    /// The same two-phase drive `ClientHandshakeTests.completeHandshake`
    /// does — duplicated rather than shared because these two suites are
    /// separate files in the same target and the helper is five lines of
    /// injected frames, not a rulebook that could drift.
    private func completeHandshake(transport: LoopbackTransport, client: MeshtasticClient,
                                   myNodeNum: UInt32 = 48_621_524) async throws {
        func frame(_ build: (inout FromRadio) -> Void) throws -> Data {
            var fr = FromRadio()
            build(&fr)
            return try fr.serializedData()
        }
        let connectTask = Task { try await client.connect() }
        try await waitForSentCount(2, on: transport) // heartbeat, want_config(onlyConfig)
        var info = MyNodeInfo()
        info.myNodeNum = myNodeNum
        transport.inject(try frame { $0.myInfo = info })
        transport.inject(try frame { $0.configCompleteID = MeshtasticConfigNonce.onlyConfig })
        try await waitForSentCount(3, on: transport) // want_config(onlyNodeDB)
        transport.inject(try frame { $0.configCompleteID = MeshtasticConfigNonce.onlyNodeDB })
        try await connectTask.value
    }

    private func decodeSentPacket(_ transport: LoopbackTransport, index: Int = 0) throws -> MeshPacket {
        let sent = transport.sentMessages
        let bytes = try XCTUnwrap(sent.indices.contains(index) ? sent[index] : nil)
        let toRadio = try ToRadio(serializedBytes: bytes)
        guard case .packet(let packet)? = toRadio.payloadVariant else {
            throw XCTSkip("ToRadio carried no packet")
        }
        return packet
    }

    private func fix(latitude: Double = 47.708135, longitude: Double = -122.2820993,
                     altitude: Double? = 42, speed: Double? = nil, track: Double? = nil) -> ExternalPositionFix {
        ExternalPositionFix(latitude: latitude, longitude: longitude, altitudeMeters: altitude,
                             time: Date(timeIntervalSince1970: 1_780_000_000),
                             groundSpeedMetersPerSecond: speed, groundTrackDegrees: track)
    }

    // MARK: - Phone GPS -> node (A01_AC9)

    func testPositionGoesOutAsPositionAppWithLocExternal() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)

        try await client.sendPosition(fix(), to: 48_621_524)

        let packet = try decodeSentPacket(transport)
        XCTAssertEqual(packet.to, 48_621_524, "a phone fix is addressed to the CONNECTED node itself")
        XCTAssertFalse(packet.wantAck, "a position report is not a message anybody is waiting on")
        guard case .decoded(let data) = packet.payloadVariant else { return XCTFail("not a decoded payload") }
        XCTAssertEqual(data.portnum, .positionApp)

        let position = try Position(serializedBytes: data.payload)
        XCTAssertEqual(position.locationSource, .locExternal,
                        "an external FIX is a measurement with a time on it — never set_fixed_position")
        XCTAssertEqual(Double(position.latitudeI) * 1e-7, 47.708135, accuracy: 1e-6)
        XCTAssertEqual(Double(position.longitudeI) * 1e-7, -122.2820993, accuracy: 1e-6)
        XCTAssertEqual(position.time, 1_780_000_000, "the moment the fix was TAKEN, not the moment it was sent")
        XCTAssertEqual(position.altitude, 42)
    }

    func testPositionOmitsEveryFieldTheFixDoesNotActuallyCarry() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)

        try await client.sendPosition(fix(altitude: nil, speed: nil, track: nil), to: 1)

        let packet = try decodeSentPacket(transport)
        guard case .decoded(let data) = packet.payloadVariant else { return XCTFail("not a decoded payload") }
        let position = try Position(serializedBytes: data.payload)
        XCTAssertFalse(position.hasGroundSpeed, "an absent speed is not 0 m/s")
        XCTAssertFalse(position.hasGroundTrack, "an absent course is not due north")
        XCTAssertEqual(position.satsInView, 0, "CoreLocation reports no satellite count; nothing is claimed")
        XCTAssertEqual(position.precisionBits, 0,
                        "precision is the CHANNEL's setting, asserted by the node — never claimed by the phone")
    }

    func testGroundSpeedAndTrackAreSentOnlyWhenTheyMeanSomething() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)

        // 0 m/s and a 0-degree course are the spec's own "not meaningful"
        // values; 361 is out of range entirely.
        try await client.sendPosition(fix(speed: 0, track: 0), to: 1)
        try await client.sendPosition(fix(speed: 3.4, track: 361), to: 1)
        try await client.sendPosition(fix(speed: 3.4, track: 180), to: 1)

        func position(_ index: Int) throws -> Position {
            let packet = try decodeSentPacket(transport, index: index)
            guard case .decoded(let data) = packet.payloadVariant else { throw XCTSkip("no payload") }
            return try Position(serializedBytes: data.payload)
        }

        XCTAssertFalse(try position(0).hasGroundSpeed)
        XCTAssertFalse(try position(0).hasGroundTrack)
        XCTAssertTrue(try position(1).hasGroundSpeed)
        XCTAssertFalse(try position(1).hasGroundTrack, "a course > 360 is not a course")
        XCTAssertEqual(try position(2).groundSpeed, 3)
        XCTAssertEqual(try position(2).groundTrack, 180)
    }

    func testPositionPublishesNoDeliveryEvents() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        let deliveries = client.deliveryUpdates()

        try await client.sendPosition(fix(), to: 1)

        // A 30-second position heartbeat must not flood the Inbox's own
        // outbox bookkeeping with WAITING/SENT rows nothing renders.
        var events: [DeliveryEvent] = []
        let drain = Task { for await event in deliveries { events.append(event) } }
        try? await Task.sleep(nanoseconds: 100_000_000)
        drain.cancel()
        XCTAssertTrue(events.isEmpty)
    }

    // MARK: - Portnum 269, outbound

    func testPrivateSendUsesPortnum269AndKeepsThePayloadOpaque() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        let payload = Data([0x01, 0x02, 0x2C, 0x01])

        try await client.sendPrivate(payload, to: 0x02E6_06B0, wantAck: true)

        let packet = try decodeSentPacket(transport)
        guard case .decoded(let data) = packet.payloadVariant else { return XCTFail("not a decoded payload") }
        XCTAssertEqual(data.portnum.rawValue, 269, "Firefly's own portnum (S04), carried as UNRECOGNIZED(269)")
        XCTAssertEqual(data.payload, payload, "the ff_proto frame passes through byte for byte")
        XCTAssertTrue(packet.wantAck)
        XCTAssertEqual(packet.to, 0x02E6_06B0)
    }

    func testPrivateBroadcastNeverAsksForAnAck() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)

        try await client.sendPrivate(Data([0x01, 0x02]), to: meshBroadcastAddress, wantAck: true)

        let packet = try decodeSentPacket(transport)
        XCTAssertFalse(packet.wantAck, "nothing acks a broadcast, whatever the caller asked for")
    }

    // MARK: - Portnum 269, inbound

    func testInboundPrivatePacketIsRepublishedWithItsOwnRxMeta() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        // Subscribe BEFORE anything can publish (S1): `incomingPrivate()`
        // is multicast and does not replay.
        let stream = client.incomingPrivate()
        // The client only ingests while its receive loop is running,
        // which `connect()` starts — so the handshake has to complete.
        try await completeHandshake(transport: transport, client: client)

        var packet = MeshPacket()
        packet.id = 77
        packet.from = 0x02E6_06B0
        packet.to = 48_621_524
        packet.channel = 0
        packet.rxRssi = -71
        packet.rxSnr = 5.25
        packet.hopStart = 3
        packet.hopLimit = 3
        var data = DataMessage()
        data.portnum = PortNum(rawValue: 269) ?? .privateApp
        data.payload = Data([0x01, 0x08, 0x2A, 0x00, 0x00, 0x00])
        packet.decoded = data
        var fr = FromRadio()
        fr.packet = packet
        transport.inject(try fr.serializedData())

        var received: IncomingPrivate?
        for await incoming in stream {
            received = incoming
            break
        }
        let got = try XCTUnwrap(received)
        XCTAssertEqual(got.from, 0x02E6_06B0)
        XCTAssertEqual(got.packetID, 77)
        XCTAssertEqual(got.payload, data.payload, "opaque bytes — decoding an ff_proto frame is the bridge's job")
        XCTAssertEqual(got.rssiDbm, -71)
        XCTAssertEqual(got.snrDb, 5.25)
        XCTAssertEqual(got.direct, true, "hop_start == hop_limit is a DIRECT packet")

        await client.disconnect()
    }

    func testAnotherPortnumIsNotRepublishedAsPrivate() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        let stream = client.incomingPrivate()
        try await completeHandshake(transport: transport, client: client)

        var packet = MeshPacket()
        packet.id = 5
        packet.from = 9
        var data = DataMessage()
        data.portnum = .telemetryApp
        data.payload = Data([0x00])
        packet.decoded = data
        var fr = FromRadio()
        fr.packet = packet
        transport.inject(try fr.serializedData())

        var received: IncomingPrivate?
        let drain = Task { for await incoming in stream { received = incoming } }
        try? await Task.sleep(nanoseconds: 100_000_000)
        drain.cancel()
        XCTAssertNil(received, "telemetry has no consumer in M1 and must not be laundered onto portnum 269")

        await client.disconnect()
    }

    // MARK: - connectedNodeNum

    /// `connectedNodeNum` is the ONE piece of client state readable with
    /// no `await` — `PhoneGPSUplink`'s `destinationNodeNum` closure is
    /// synchronous, and an actor hop per GPS fix would be the cost of
    /// not having this.
    func testConnectedNodeNumIsNilUntilMyInfoArrivesAndIsReadableWithoutAnAwait() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        XCTAssertNil(client.connectedNodeNum, "no handshake yet means no node to address — never a guess")

        try await completeHandshake(transport: transport, client: client)

        XCTAssertEqual(client.connectedNodeNum, 48_621_524)
        await client.disconnect()
    }
}
