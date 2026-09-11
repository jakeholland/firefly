//
//  DemoMeshtasticClientTests.swift — the demo client's own contract,
//  independent of `DemoRunner`/`CoreStore`: it plays exactly the
//  script it is handed, never invents anything beyond that, and its
//  scripted ack outcomes land as real `DeliveryEvent`s.
//
import FireflyMesh
import XCTest

final class DemoMeshtasticClientTests: XCTestCase {

    private func node(_ num: UInt32, rssi: Int16 = -60) -> MeshNodeSnapshot {
        MeshNodeSnapshot(num: num, shortName: "N\(num)", longName: nil, position: nil,
                          lastHeard: Date(), rssiDbm: rssi, snrDb: nil, hopsAway: 0)
    }

    func testConnectPlaysConnectingHandshakingReadyAndDumpsTheScriptedNodes() async throws {
        let client = DemoMeshtasticClient(myNodeNum: 900_001, nodes: [node(900_002), node(900_003)])

        let states = client.linkState()
        let nodes = client.nodeUpdates()
        var seenStates: [LinkState] = []
        var seenNodes: [UInt32] = []
        let collector = Task {
            for await s in states {
                seenStates.append(s)
                if s == .ready { break }
            }
        }
        let nodeCollector = Task {
            for await n in nodes {
                seenNodes.append(n.num)
                if seenNodes.count == 2 { break }
            }
        }

        try await client.connect()
        _ = await collector.result
        _ = await nodeCollector.result

        XCTAssertEqual(seenStates, [.connecting, .handshaking, .ready])
        XCTAssertEqual(seenNodes, [900_002, 900_003])
        XCTAssertEqual(client.connectedNodeNum, 900_001)
    }

    func testDisconnectClearsConnectedNodeNum() async throws {
        let client = DemoMeshtasticClient(myNodeNum: 900_001, nodes: [])
        try await client.connect()
        XCTAssertEqual(client.connectedNodeNum, 900_001)

        await client.disconnect()

        XCTAssertNil(client.connectedNodeNum)
    }

    /// The demo client's own version of "a DM I send that goes
    /// WAITING -> SENT -> DELIVERED, one that ends NO ACK": two
    /// `wantAck: true` sends consume the FIFO in order.
    func testScriptedAckOutcomesFireInOrderForWantAckSends() async throws {
        let client = DemoMeshtasticClient(myNodeNum: 1, nodes: [], ackOutcomes: [.delivered, .noAck],
                                          ackDelayMs: 20)
        let deliveries = client.deliveryUpdates()
        var events: [DeliveryEvent] = []
        let collector = Task {
            for await event in deliveries {
                events.append(event)
                if events.count == 6 { break } // 2 sends x (waiting, sent, ack) each
            }
        }

        _ = try await client.sendText("hi", to: 2, wantAck: true)
        _ = try await client.sendText("you there?", to: 2, wantAck: true)
        _ = await collector.result

        // First send: WAITING, SENT, then DELIVERED for its own packet id.
        guard case .waiting = events[0] else { return XCTFail("expected waiting, got \(events[0])") }
        guard case .sent(_, let firstPacketID, true) = events[1] else {
            return XCTFail("expected sent, got \(events[1])")
        }
        guard case .waiting = events[2] else { return XCTFail("expected waiting, got \(events[2])") }
        guard case .sent(_, let secondPacketID, true) = events[3] else {
            return XCTFail("expected sent, got \(events[3])")
        }

        let acks = Array(events[4...5])
        XCTAssertTrue(acks.contains(.delivered(packetID: firstPacketID)),
                       "the first scripted outcome must resolve DELIVERED")
        XCTAssertTrue(acks.contains(.noAck(packetID: secondPacketID)),
                       "the second scripted outcome must resolve NO ACK")
    }

    func testBroadcastNeverGetsAnAckEvenWhenOutcomesAreQueued() async throws {
        let client = DemoMeshtasticClient(myNodeNum: 1, nodes: [], ackOutcomes: [.delivered], ackDelayMs: 10)
        let deliveries = client.deliveryUpdates()
        var events: [DeliveryEvent] = []
        let collector = Task {
            for await event in deliveries {
                events.append(event)
                if events.count == 2 { break }
            }
        }

        _ = try await client.sendText("crew update", to: meshBroadcastAddress, wantAck: false)
        _ = await collector.result

        try? await Task.sleep(nanoseconds: 60_000_000)
        XCTAssertEqual(events.count, 2, "a broadcast must stop at SENT, never DELIVERED")
    }

    func testInjectMethodsPlayExactlyWhatTheyAreHanded() async {
        let client = DemoMeshtasticClient(myNodeNum: 1, nodes: [])
        let texts = client.incomingTexts()
        let incoming = IncomingText(from: 2, to: 1, channel: 0, packetID: 5, text: "hey", rxTime: Date(),
                                     rssiDbm: -50, snrDb: nil, direct: true)
        var seen: [IncomingText] = []
        let collector = Task {
            for await t in texts { seen.append(t); break }
        }
        client.injectIncomingText(incoming)
        _ = await collector.result
        XCTAssertEqual(seen, [incoming])
    }
}
