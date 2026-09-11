//
//  DeliveryStateTests.swift — the app and the puck agree about what
//  happened to a message.
//
import FireflyCore
import FireflyMesh
import XCTest

final class DeliveryStateTests: XCTestCase {

    /// Every Swift case maps onto the puck's own `ff_feed_send_status_t`.
    /// If someone reorders the C enum, this fails here instead of the
    /// two products quietly disagreeing about DELIVERED.
    func testEveryStateMapsToTheCEnum() {
        XCTAssertEqual(DeliveryState.waiting.ffSendStatus, FF_SEND_WAITING)
        XCTAssertEqual(DeliveryState.sent.ffSendStatus, FF_SEND_SENT)
        XCTAssertEqual(DeliveryState.delivered.ffSendStatus, FF_SEND_DELIVERED)
        XCTAssertEqual(DeliveryState.noAck.ffSendStatus, FF_SEND_NO_ACK)
        XCTAssertEqual(DeliveryState.dropped.ffSendStatus, FF_SEND_DROPPED)
        XCTAssertEqual(DeliveryState.allCases.count, 5,
                       "a new delivery state needs a matching ff_feed_send_status_t")
    }

    /// The labels the Inbox shows are the S24 vocabulary verbatim.
    func testLabelsAreTheS24Vocabulary() {
        XCTAssertEqual(DeliveryState.waiting.rawValue, "WAITING")
        XCTAssertEqual(DeliveryState.sent.rawValue, "SENT")
        XCTAssertEqual(DeliveryState.delivered.rawValue, "DELIVERED")
        XCTAssertEqual(DeliveryState.noAck.rawValue, "NO ACK")
    }

    /// A stub has no mesh, so it must never report DELIVERED. The same
    /// rule holds for a real broadcast, which nobody acks.
    func testStubClientNeverFabricatesDelivery() async throws {
        let transport = LoopbackTransport()
        let client = StubMeshtasticClient(transport: transport)

        var seen: [DeliveryState] = []
        let collector = Task {
            for await (_, state) in client.deliveryUpdates {
                seen.append(state)
                if seen.count == 2 { break }
            }
        }
        try await client.connect()
        let id = try await client.sendText("WHERE", to: meshBroadcastAddress, wantAck: false)
        _ = await collector.result

        XCTAssertGreaterThan(id, 0)
        XCTAssertEqual(seen, [.waiting, .sent])
        XCTAssertFalse(seen.contains(.delivered))
        XCTAssertEqual(transport.sentMessages.count, 1)
    }
}
