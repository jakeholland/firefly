//
//  ClientPayloadSizeTests.swift — hardening QA pass.
//
//  `sendText` bounded the text nowhere. Measured before the fix: a
//  400-character message serialized to a 421-byte `ToRadio` carrying a
//  400-byte `DataMessage.payload` — 167 bytes past Meshtastic's own
//  `Constants.DATA_PAYLOAD_LEN` (233) — and the client published
//  `.sent` for it.
//
//  On a real radio that message never reaches the mesh: the firmware
//  rejects an oversized payload, and over BLE the `.withoutResponse`
//  write past the negotiated ATT MTU fails with NO delegate callback at
//  all (`BLETransport.logOversizedWriteIfNeeded`'s own comment). So the
//  app claimed SENT for something that never left the phone — and for
//  the CREW conversation, which is a broadcast and which nothing acks,
//  it claimed SENT *forever*, since the 5-minute NO ACK window only
//  applies to a direct message.
//
//  That is a lost message the app lies about, in the one screen whose
//  whole job is telling you whether your crew got the message. Refused
//  locally now, and surfaced as DROPPED.
//
import Foundation
import MeshtasticProto
import XCTest
@testable import FireflyMesh

@MainActor
final class ClientPayloadSizeTests: XCTestCase {

    private let broadcast: UInt32 = 0xFFFF_FFFF

    /// The limit is read from the pinned protobufs, not retyped — a
    /// firmware bump that moves it moves this with it.
    func testTheLimitIsMeshtasticsOwnConstantNotALocalGuess() {
        XCTAssertEqual(MeshtasticClient.maxDataPayloadBytes, 233)
        XCTAssertEqual(MeshtasticClient.maxDataPayloadBytes, Int(Constants.dataPayloadLen.rawValue))
    }

    func testExactlyTheLimitIsAcceptedAndOneByteMoreIsNot() {
        let max = MeshtasticClient.maxDataPayloadBytes
        XCTAssertNil(MeshtasticClient.payloadSizeError(Data(repeating: 0x41, count: max)),
                     "the boundary itself must still send — an off-by-one here silently shortens every message")
        XCTAssertNil(MeshtasticClient.payloadSizeError(Data()))
        XCTAssertEqual(MeshtasticClient.payloadSizeError(Data(repeating: 0x41, count: max + 1)),
                       .payloadTooLarge(bytes: max + 1, max: max))
    }

    /// The limit is BYTES, and this app's users type at a festival: one
    /// emoji is four bytes, so a message well under 233 *characters* can
    /// still be over the wire limit. Pinned so a future "just count the
    /// characters" simplification fails here.
    func testTheLimitIsBytesNotCharacters() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        // 100 four-byte emoji = 400 bytes, but only 100 characters.
        let text = String(repeating: "🔥", count: 100)
        XCTAssertLessThan(text.count, MeshtasticClient.maxDataPayloadBytes,
                          "the fixture must be short in CHARACTERS to be worth anything")
        do {
            _ = try await client.sendText(text, to: broadcast, wantAck: false)
            XCTFail("400 bytes of emoji is still 400 bytes")
        } catch let error as MeshtasticClientError {
            XCTAssertEqual(error, .payloadTooLarge(bytes: 400, max: 233))
        }
    }

    /// The honesty assertion: nothing goes on the wire, and the WAITING
    /// event the caller already saw is resolved as DROPPED rather than
    /// left orphaned. An unresolved WAITING is exactly as dishonest as a
    /// false SENT — the message just spins instead of lying loudly.
    func testAnOversizedTextWritesNothingAndResolvesAsDroppedNotSent() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)

        let collected = DeliveryEventLog()
        let stream = client.deliveryUpdates()
        let drain = Task { for await event in stream { collected.append(event) } }
        // Let the subscription register before publishing into it —
        // `EventHub` is multicast, not replayed (S1).
        try await Task.sleep(for: .milliseconds(20))

        do {
            _ = try await client.sendText(String(repeating: "x", count: 400), to: broadcast, wantAck: false)
            XCTFail("an oversized text must not be reported as sent")
        } catch let error as MeshtasticClientError {
            XCTAssertEqual(error, .payloadTooLarge(bytes: 400, max: 233))
        }
        try await Task.sleep(for: .milliseconds(50))
        drain.cancel()

        XCTAssertTrue(transport.sentMessages.isEmpty,
                      "a message the mesh cannot carry must not be handed to the radio at all")
        let events = collected.events
        XCTAssertEqual(events.count, 2, "exactly WAITING then DROPPED: \(events)")
        guard case .waiting(let waitingID)? = events.first,
              case .dropped(let droppedID)? = events.last else {
            return XCTFail("expected .waiting then .dropped, got \(events)")
        }
        XCTAssertEqual(waitingID, droppedID, "the DROPPED must resolve the very outbox id that was left WAITING")
        for event in events {
            if case .sent = event { XCTFail("nothing may publish .sent for a message that never left the phone") }
        }
    }

    /// And the packet id space is untouched by a refusal — the refusal
    /// happens before a packet id is minted, so a rejected message does
    /// not burn an id the mesh will never see.
    func testARefusedTextDoesNotConsumeAPacketID() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        _ = try? await client.sendText(String(repeating: "x", count: 400), to: broadcast, wantAck: false)
        let first = try await client.sendText("ok", to: broadcast, wantAck: false)
        _ = try? await client.sendText(String(repeating: "x", count: 400), to: broadcast, wantAck: false)
        let second = try await client.sendText("also ok", to: broadcast, wantAck: false)
        XCTAssertEqual(second, first &+ 1,
                       "a refused send must not advance the packet id counter")
    }

    /// A normal message is completely unaffected — the guard must not
    /// have narrowed what actually sends.
    func testANormalMessageStillSendsAndReportsSent() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        _ = try await client.sendText("where are you", to: broadcast, wantAck: false)
        XCTAssertEqual(transport.sentMessages.count, 1)
    }

    /// `sendPrivate` (Firefly's own portnum 269) shares the wire limit.
    /// Real FLARE/RALLY frames are far smaller, so this pins that as a
    /// measured fact rather than an assumption.
    func testPrivateSendRefusesAnOversizedPayloadToo() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        do {
            _ = try await client.sendPrivate(Data(repeating: 0x7F, count: 300), to: broadcast, wantAck: false)
            XCTFail("portnum 269 is not exempt from the wire's own payload limit")
        } catch let error as MeshtasticClientError {
            XCTAssertEqual(error, .payloadTooLarge(bytes: 300, max: 233))
        }
        XCTAssertTrue(transport.sentMessages.isEmpty)
    }
}

/// A `Sendable` sink for the delivery stream — the drain task is
/// `@Sendable`, so a captured `var` array is a Swift 6 error.
private final class DeliveryEventLog: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [DeliveryEvent] = []
    func append(_ event: DeliveryEvent) { lock.lock(); storage.append(event); lock.unlock() }
    var events: [DeliveryEvent] { lock.lock(); defer { lock.unlock() }; return storage }
}
