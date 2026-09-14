//
//  ClientCrewRxMetaTests.swift — A02 slice C's `[api]` half: the
//  per-packet metadata `MeshNodeSnapshot` now carries (`MeshRxMeta`),
//  and the live `NODEINFO_APP` decode path that did not exist
//  (docs/specs/A02-crew-join.md §4.2.1 items 1 and 4).
//
//  Everything here is scripted `FromRadio` bytes through
//  `LoopbackTransport`, so these assert the ACTUAL wire behaviour rather
//  than that a function was called. No sleeps anywhere: the handshake
//  waits on `LoopbackTransport.waitForSentCount` (continuation-backed,
//  not polling) and every assertion drains a fixed number of elements
//  off `nodeUpdates()`, which suspends until they arrive.
//
import FireflyMesh
import MeshtasticProto
import XCTest

@MainActor
final class ClientCrewRxMetaTests: XCTestCase {

    // MARK: - Harness

    private func frame(_ build: (inout FromRadio) -> Void) throws -> Data {
        var fr = FromRadio()
        build(&fr)
        return try fr.serializedData()
    }

    private func completeHandshake(transport: LoopbackTransport, client: MeshtasticClient,
                                   myNodeNum: UInt32 = 48_621_524) async throws {
        let connectTask = Task { try await client.connect() }
        try await transport.waitForSentCount(2) // heartbeat, want_config(onlyConfig)
        var info = MyNodeInfo()
        info.myNodeNum = myNodeNum
        transport.inject(try frame { $0.myInfo = info })
        transport.inject(try frame { $0.configCompleteID = MeshtasticConfigNonce.onlyConfig })
        try await transport.waitForSentCount(3) // want_config(onlyNodeDB)
        transport.inject(try frame { $0.configCompleteID = MeshtasticConfigNonce.onlyNodeDB })
        try await connectTask.value
    }

    /// Drains exactly `count` snapshots. Deterministic: `for await`
    /// suspends until the elements the injected frames produce actually
    /// arrive, so nothing here depends on a timer.
    private func drain(_ stream: AsyncStream<MeshNodeSnapshot>, count: Int) async -> [MeshNodeSnapshot] {
        var out: [MeshNodeSnapshot] = []
        for await snapshot in stream {
            out.append(snapshot)
            if out.count == count { break }
        }
        return out
    }

    private func packet(from: UInt32, channel: UInt32, portnum: PortNum, payload: Data,
                        viaMqtt: Bool = false, to: UInt32 = meshBroadcastAddress) -> MeshPacket {
        var pkt = MeshPacket()
        pkt.from = from
        pkt.to = to
        pkt.channel = channel
        pkt.viaMqtt = viaMqtt
        var data = DataMessage()
        data.portnum = portnum
        data.payload = payload
        pkt.decoded = data
        return pkt
    }

    // MARK: - MeshRxMeta off a live packet

    func testALivePacketPublishesItsChannelIndexViaMqttAndPortnum() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        try await completeHandshake(transport: transport, client: client)
        let stream = client.nodeUpdates()

        transport.inject(try frame {
            $0.packet = packet(from: 7, channel: 3, portnum: .textMessageApp,
                               payload: Data("hi".utf8), viaMqtt: true)
        })

        let snapshots = await drain(stream, count: 1)
        let meta = try XCTUnwrap(snapshots.first?.rxMeta)
        XCTAssertEqual(meta.from, 7)
        XCTAssertEqual(meta.channelIndex, 3)
        XCTAssertTrue(meta.viaMQTT)
        XCTAssertEqual(meta.portnum, Int32(PortNum.textMessageApp.rawValue))
        XCTAssertTrue(meta.decrypted)
    }

    /// The load-bearing clause of A02 §4.1: a packet the radio could not
    /// decrypt carries no portnum at all, and says so rather than
    /// reporting one.
    func testAnEncryptedPacketReportsNoPortnumAndNotDecrypted() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        try await completeHandshake(transport: transport, client: client)
        let stream = client.nodeUpdates()

        var pkt = MeshPacket()
        pkt.from = 11
        pkt.channel = 0
        pkt.encrypted = Data([0xDE, 0xAD, 0xBE, 0xEF])
        transport.inject(try frame { $0.packet = pkt })

        let snapshots = await drain(stream, count: 1)
        let meta = try XCTUnwrap(snapshots.first?.rxMeta)
        XCTAssertFalse(meta.decrypted)
        XCTAssertNil(meta.portnum, "an undecryptable packet HAS no portnum — not a default of 0")
    }

    /// The gap §4.2.1 item 2 names: before this slice, a text or
    /// portnum-269 packet from an id with no nodeDB record produced no
    /// `nodeUpdates()` element at all, so the admission rule had nothing
    /// to run on.
    func testAPacketFromAnIdWithNoNodeDBRecordStillPublishesAnEvent() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        try await completeHandshake(transport: transport, client: client)
        let stream = client.nodeUpdates()

        transport.inject(try frame {
            $0.packet = packet(from: 4242, channel: 0, portnum: .textMessageApp, payload: Data("yo".utf8))
        })

        let snapshots = await drain(stream, count: 1)
        let snapshot = try XCTUnwrap(snapshots.first)
        XCTAssertEqual(snapshot.num, 4242)
        XCTAssertNil(snapshot.shortName, "nothing is invented for an id we know nothing about")
        XCTAssertNil(snapshot.longName)
        XCTAssertNil(snapshot.position)
        XCTAssertEqual(snapshot.rxMeta?.channelIndex, 0)
    }

    /// A snapshot published for a node the nodeDB already knows must
    /// carry that node's identity forward — a consumer that replaces by
    /// `num` (`NearbyNodesViewModel.apply(_:)`) must not lose a name to
    /// a packet that carried none.
    func testAPacketForAKnownNodeKeepsItsIdentity() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        try await completeHandshake(transport: transport, client: client)
        let stream = client.nodeUpdates()

        var user = User()
        user.longName = "Firefly 1"
        user.shortName = "FF1"
        transport.inject(try frame {
            $0.packet = packet(from: 55, channel: 0, portnum: .nodeinfoApp,
                               payload: try! user.serializedData())
        })
        transport.inject(try frame {
            $0.packet = packet(from: 55, channel: 0, portnum: .textMessageApp, payload: Data("hi".utf8))
        })

        // 3 events: the NodeInfo packet's own rx-meta event, the decoded
        // NodeInfo, then the text packet's rx-meta event.
        let snapshots = await drain(stream, count: 3)
        XCTAssertEqual(snapshots.last?.num, 55)
        XCTAssertEqual(snapshots.last?.longName, "Firefly 1")
        XCTAssertEqual(snapshots.last?.rxMeta?.portnum, Int32(PortNum.textMessageApp.rawValue))
    }

    // MARK: - Live NODEINFO_APP decode (§4.2.1 item 4)

    func testALiveNodeInfoPacketNamesTheNode() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        try await completeHandshake(transport: transport, client: client)
        let stream = client.nodeUpdates()

        var user = User()
        user.longName = "Deshawn"
        user.shortName = "DSH"
        user.hwModel = .heltecV3
        transport.inject(try frame {
            $0.packet = packet(from: 900, channel: 0, portnum: .nodeinfoApp,
                               payload: try! user.serializedData())
        })

        let snapshots = await drain(stream, count: 2)
        let named = try XCTUnwrap(snapshots.last)
        XCTAssertEqual(named.num, 900)
        XCTAssertEqual(named.longName, "Deshawn")
        XCTAssertEqual(named.shortName, "DSH")
        XCTAssertNotNil(named.observedAt, "a live NodeInfo packet IS an observation")
        XCTAssertEqual(named.rxMeta?.portnum, Int32(PortNum.nodeinfoApp.rawValue))
    }

    /// The node number comes from the PACKET, never from `User.id` — a
    /// self-declared string a node may not set, and which a hostile
    /// sender could set to somebody else's.
    func testALiveNodeInfoPacketTakesItsNumFromThePacketNotTheUserPayload() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        try await completeHandshake(transport: transport, client: client)
        let stream = client.nodeUpdates()

        var user = User()
        user.id = "!deadbeef"
        user.longName = "Impostor"
        transport.inject(try frame {
            $0.packet = packet(from: 1234, channel: 0, portnum: .nodeinfoApp,
                               payload: try! user.serializedData())
        })

        let snapshots = await drain(stream, count: 2)
        XCTAssertEqual(snapshots.last?.num, 1234)
    }

    /// #294's rule, applied at the decode site: a stamp ahead of our
    /// clock is not a measurement — `ff_crew`'s unsigned age arithmetic
    /// turns even a few seconds of forward skew into ~49 days.
    func testALiveNodeInfoPacketRefusesAFutureRxTime() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        try await completeHandshake(transport: transport, client: client)
        let stream = client.nodeUpdates()

        var user = User()
        user.longName = "Fast Clock"
        var pkt = packet(from: 77, channel: 0, portnum: .nodeinfoApp, payload: try! user.serializedData())
        pkt.rxTime = UInt32(Date().addingTimeInterval(3600).timeIntervalSince1970)
        transport.inject(try frame { $0.packet = pkt })

        let snapshots = await drain(stream, count: 2)
        XCTAssertNil(snapshots.last?.lastHeard,
                      "a timestamp ahead of our clock is refused, not clamped — the caller falls through to observedAt")
        XCTAssertNotNil(snapshots.last?.observedAt)
    }

    func testALiveNodeInfoPacketKeepsAPlausibleRxTime() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        try await completeHandshake(transport: transport, client: client)
        let stream = client.nodeUpdates()

        var user = User()
        user.longName = "Good Clock"
        let stamp = Date().addingTimeInterval(-30)
        var pkt = packet(from: 78, channel: 0, portnum: .nodeinfoApp, payload: try! user.serializedData())
        pkt.rxTime = UInt32(stamp.timeIntervalSince1970)
        transport.inject(try frame { $0.packet = pkt })

        let snapshots = await drain(stream, count: 2)
        let lastHeard = try XCTUnwrap(snapshots.last?.lastHeard)
        XCTAssertEqual(lastHeard.timeIntervalSince1970, stamp.timeIntervalSince1970, accuracy: 1)
    }

    /// The want_config nodeDB REPLAY is not a packet, so it carries no
    /// `MeshRxMeta` — which is the whole mechanism by which A02 §4.2's
    /// "replay admits nobody" falls out rather than needing a guard.
    func testTheNodeDBReplayCarriesNoPacketMetadata() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        let stream = client.nodeUpdates()

        let connectTask = Task { try await client.connect() }
        try await transport.waitForSentCount(2)
        var info = MyNodeInfo()
        info.myNodeNum = 48_621_524
        transport.inject(try frame { $0.myInfo = info })
        transport.inject(try frame { $0.configCompleteID = MeshtasticConfigNonce.onlyConfig })
        try await transport.waitForSentCount(3)
        var node = NodeInfo()
        node.num = 5150
        var user = User()
        user.longName = "Replayed Stranger"
        node.user = user
        node.channel = 0
        transport.inject(try frame { $0.nodeInfo = node })
        transport.inject(try frame { $0.configCompleteID = MeshtasticConfigNonce.onlyNodeDB })
        try await connectTask.value

        let snapshots = await drain(stream, count: 1)
        let replayed = try XCTUnwrap(snapshots.first)
        XCTAssertEqual(replayed.num, 5150)
        XCTAssertEqual(replayed.longName, "Replayed Stranger")
        XCTAssertNil(replayed.rxMeta, "a replay is a summary, not a packet")
        XCTAssertNil(replayed.observedAt, "and it is not an observation either")
    }
}
