//
//  InboundPacketOrderingTests.swift — the client half of the
//  2026-09-14 bench race (`FireflyMesh.InboundPacketEvent`,
//  `docs/specs/A02-crew-join.md` §4.1's ordering clause).
//
//  `MeshtasticClient.handle(meshPacket:)` has always produced a
//  packet's rx-meta node snapshot BEFORE that packet's decoded payload
//  — the same order `ff_shell.c` runs `shell_try_admit` off
//  `on_rx_meta` in before it dispatches the portnum. What it did not
//  have was any way for a consumer to OBSERVE that order: the two
//  halves went out on two independent `EventHub`s. These tests pin the
//  ordered pipeline that fixes it, at the level the client actually
//  controls: one stream, one sequence, node-before-payload, per packet.
//
//  Scripted `FromRadio` bytes through `LoopbackTransport` -> the real
//  `MeshtasticClient`, same harness discipline as
//  `ClientPositionAndPrivateTests`.
//
import FireflyMesh
import MeshtasticProto
import XCTest

final class InboundPacketOrderingTests: XCTestCase {

    private static let sender: UInt32 = 2_403_905_316   // the bench puck, !8f48af24
    private static let crewIndex: UInt32 = 0

    private func waitForSentCount(_ n: Int, on transport: LoopbackTransport) async throws {
        for _ in 0..<400 {
            if transport.sentMessages.count >= n { return }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTFail("timed out waiting for \(n) sent message(s); saw \(transport.sentMessages.count)")
    }

    /// The client only ingests while its receive loop is running, which
    /// `connect()` starts — same two-phase drive (and same reason for
    /// duplicating it rather than sharing) as
    /// `ClientPositionAndPrivateTests.completeHandshake`.
    private func completeHandshake(transport: LoopbackTransport, client: MeshtasticClient) async throws {
        let connectTask = Task { try await client.connect() }
        try await waitForSentCount(2, on: transport)
        var info = MyNodeInfo()
        info.myNodeNum = 48_621_524
        transport.inject(try frame { $0.myInfo = info })
        transport.inject(try frame { $0.configCompleteID = MeshtasticConfigNonce.onlyConfig })
        try await waitForSentCount(3, on: transport)
        transport.inject(try frame { $0.configCompleteID = MeshtasticConfigNonce.onlyNodeDB })
        try await connectTask.value
    }

    private func frame(_ build: (inout FromRadio) -> Void) throws -> Data {
        var fr = FromRadio()
        build(&fr)
        return try fr.serializedData()
    }

    private func packetFrame(from: UInt32, channel: UInt32, portnum: PortNum, payload: Data) throws -> Data {
        var pkt = MeshPacket()
        pkt.from = from
        pkt.to = meshBroadcastAddress
        pkt.channel = channel
        pkt.id = 77
        var data = DataMessage()
        data.portnum = portnum
        data.payload = payload
        pkt.decoded = data
        return try frame { $0.packet = pkt }
    }

    /// Portnum 269 is not a named `PortNum` enumerator — it arrives as
    /// `.UNRECOGNIZED(269)`, which is exactly how a real Firefly frame
    /// reaches this client (`WireFormatTests
    /// .testFireflyPortnumSurvivesAsUnrecognized`).
    private func fireflyFrame(from: UInt32, channel: UInt32, payload: Data) throws -> Data {
        try packetFrame(from: from, channel: channel, portnum: PortNum(rawValue: 269) ?? .privateApp,
                        payload: payload)
    }

    /// A minimal, well-formed FLARE body: `[ver:1][type:1][dur_s:2 LE]`
    /// (`ff_proto.h`). Built by hand here rather than through
    /// `FireflyPacket.encode()` — that type lives in `FireflyModel`,
    /// which depends on THIS module, and the client's contract is that
    /// these bytes are opaque to it anyway.
    private static let flareBody = Data([0x01, 0x02, 0x2C, 0x01])   // dur = 300s

    /// The property the whole fix rests on: for ONE packet, the node
    /// event precedes the payload event on the ordered stream. Not
    /// "usually", not "after a sleep" — first element, second element.
    func testPrivateFramePacketPublishesItsNodeSnapshotFirst() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        try await completeHandshake(transport: transport, client: client)
        // Subscribed after the handshake and before the packet under
        // test: this stream is multicast and never replays (S1), so it
        // carries exactly what the injection below produces and no
        // handshake noise.
        let stream = client.inboundPackets()

        transport.inject(try fireflyFrame(from: Self.sender, channel: Self.crewIndex, payload: Self.flareBody))

        var iterator = stream.makeAsyncIterator()
        guard case .node(let snapshot)? = await iterator.next() else {
            return XCTFail("first ordered element for a portnum-269 packet must be its node snapshot")
        }
        XCTAssertEqual(snapshot.num, Self.sender)
        // The facts A02 §4.1 is stated in terms of, off THIS packet.
        XCTAssertEqual(snapshot.rxMeta?.channelIndex, Self.crewIndex)
        XCTAssertEqual(snapshot.rxMeta?.portnum, 269)
        XCTAssertEqual(snapshot.rxMeta?.viaMQTT, false)
        XCTAssertEqual(snapshot.rxMeta?.decrypted, true)

        guard case .privateFrame(let packet)? = await iterator.next() else {
            return XCTFail("second ordered element must be the portnum-269 payload")
        }
        XCTAssertEqual(packet.from, Self.sender)
        XCTAssertEqual(packet.payload, Self.flareBody)

        await client.disconnect()
    }

    /// Same property for TEXT_MESSAGE_APP — the other portnum whose
    /// payload consumer can ask a membership question about the sender.
    func testTextPacketPublishesItsNodeSnapshotFirst() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        try await completeHandshake(transport: transport, client: client)
        let stream = client.inboundPackets()

        transport.inject(try packetFrame(from: Self.sender, channel: Self.crewIndex,
                                         portnum: .textMessageApp, payload: Data("where are you".utf8)))

        var iterator = stream.makeAsyncIterator()
        guard case .node(let snapshot)? = await iterator.next() else {
            return XCTFail("first ordered element for a text packet must be its node snapshot")
        }
        XCTAssertEqual(snapshot.num, Self.sender)
        XCTAssertEqual(snapshot.rxMeta?.portnum, Int32(PortNum.textMessageApp.rawValue))

        guard case .text(let text)? = await iterator.next() else {
            return XCTFail("second ordered element must be the decoded text")
        }
        XCTAssertEqual(text.text, "where are you")

        await client.disconnect()
    }

    /// The ordered stream is not a REPLACEMENT that could quietly drop
    /// events: every element the per-kind streams publish appears on it
    /// too. A POSITION_APP packet publishes two node events (the
    /// generic rx-meta one, then the position decode) and no payload
    /// event — both must be on the pipeline, in that order.
    func testPositionPacketPublishesBothOfItsNodeEventsInOrder() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        try await completeHandshake(transport: transport, client: client)
        let ordered = client.inboundPackets()
        let perKind = client.nodeUpdates()

        var position = Position()
        position.latitudeI = 477_081_350
        position.longitudeI = -1_222_820_993
        position.locationSource = .locInternal
        transport.inject(try packetFrame(from: Self.sender, channel: Self.crewIndex,
                                         portnum: .positionApp, payload: try position.serializedData()))

        var orderedIterator = ordered.makeAsyncIterator()
        var perKindIterator = perKind.makeAsyncIterator()
        for _ in 0..<2 {
            guard case .node(let fromOrdered)? = await orderedIterator.next(),
                  let fromPerKind = await perKindIterator.next() else {
                return XCTFail("the ordered pipeline must carry every node event nodeUpdates() does")
            }
            XCTAssertEqual(fromOrdered.num, fromPerKind.num)
            XCTAssertEqual(fromOrdered.rxMeta?.portnum, fromPerKind.rxMeta?.portnum)
        }

        await client.disconnect()
    }
}
