//
//  HistoryStoreTests.swift — the SwiftData round trip (docs/specs/
//  A01-companion-app.md, M3).
//
//  Every test uses `HistoryStore.inMemory()` — never `.live()` — so
//  nothing here touches disk (this package's own "no hardware, no
//  network beyond dependency resolution" `swift test` rule extends to
//  "no real files" for exactly the same reason).
//
import FireflyMesh
import FireflyModel
import XCTest

@MainActor
final class HistoryStoreTests: XCTestCase {

    private func outboundMessage(id: UInt64 = 1, text: String = "hello", destination: UInt32 = 42,
                                  deliveryState: DeliveryState? = .waiting,
                                  timestamp: Date = Date()) -> FeedMessage {
        FeedMessage(id: id, kind: .text, direction: .out, text: text, timestamp: timestamp,
                    destination: destination, deliveryState: deliveryState)
    }

    // MARK: - Round trip

    func testRecordedMessageRoundTripsThroughLoadAllForRestore() {
        let store = HistoryStore.inMemory()
        let sent = outboundMessage(id: 0x8000_0000_0000_0001, text: "round trip me", deliveryState: .delivered)
        store.record(sent, in: .member(42))

        let loaded = store.loadAllForRestore()
        XCTAssertEqual(loaded.count, 1)
        let (conversation, message) = loaded[0]
        XCTAssertEqual(conversation, .member(42))
        XCTAssertEqual(message.id, sent.id, "the UInt64 id, including its top bit, survives the Int64 bit-pattern round trip")
        XCTAssertEqual(message.text, "round trip me")
        XCTAssertEqual(message.direction, .out)
        XCTAssertEqual(message.deliveryState, .delivered)
        XCTAssertEqual(message.destination, 42)
    }

    func testInboundMessageRoundTripsWithNilDeliveryState() {
        let store = HistoryStore.inMemory()
        let inbound = FeedMessage(id: 7, kind: .text, direction: .direct, senderID: 99, senderName: "Riley",
                                   text: "hi there", timestamp: Date())
        store.record(inbound, in: .crew)

        let (conversation, message) = try! XCTUnwrap(store.loadAllForRestore().first)
        XCTAssertEqual(conversation, .crew)
        XCTAssertEqual(message.senderID, 99)
        XCTAssertEqual(message.senderName, "Riley")
        XCTAssertNil(message.deliveryState, "FF_SEND_NONE's own Swift spelling — never a fabricated state")
    }

    func testRecordingTheSameIDTwiceUpdatesInPlaceRatherThanDuplicating() {
        let store = HistoryStore.inMemory()
        store.record(outboundMessage(id: 1, text: "first", deliveryState: .waiting), in: .member(42))
        store.record(outboundMessage(id: 1, text: "first (edited)", deliveryState: .sent), in: .member(42))

        let all = store.loadAllForRestore()
        XCTAssertEqual(all.count, 1, "a re-push of the same id updates the existing row, never inserts a second one")
        XCTAssertEqual(all[0].1.text, "first (edited)")
        XCTAssertEqual(all[0].1.deliveryState, .sent)
    }

    // MARK: - The four `InboxProviding`-shaped write methods

    func testMarkSentStampsPacketIDAndFlipsToSent() {
        let store = HistoryStore.inMemory()
        store.record(outboundMessage(id: 5, deliveryState: .waiting), in: .member(42))
        store.markSent(outboxID: 5, packetID: 777, at: Date())

        let message = try! XCTUnwrap(store.loadAllForRestore().first?.1)
        XCTAssertEqual(message.deliveryState, .sent)
        XCTAssertEqual(message.packetID, 777)
    }

    func testSetStatusByOutboxIDUpdatesTheMatchingRow() {
        let store = HistoryStore.inMemory()
        store.record(outboundMessage(id: 5, deliveryState: .waiting), in: .member(42))
        store.setStatus(outboxID: 5, state: .dropped, at: Date())

        XCTAssertEqual(store.loadAllForRestore().first?.1.deliveryState, .dropped)
    }

    func testSetStatusByPacketIDUpdatesTheMatchingRow() {
        let store = HistoryStore.inMemory()
        store.record(outboundMessage(id: 5, deliveryState: .waiting), in: .member(42))
        store.markSent(outboxID: 5, packetID: 900, at: Date())
        store.setStatus(packetID: 900, state: .delivered, at: Date())

        XCTAssertEqual(store.loadAllForRestore().first?.1.deliveryState, .delivered)
    }

    func testSetStatusForAnUnknownIDIsASilentNoOp() {
        let store = HistoryStore.inMemory()
        store.setStatus(outboxID: 999, state: .dropped, at: Date()) // nothing to find — must not crash
        store.setStatus(packetID: 999, state: .delivered, at: Date())
        XCTAssertTrue(store.loadAllForRestore().isEmpty)
    }

    // MARK: - pendingOutbox

    func testPendingOutboxReturnsOnlyWaitingOutboundItemsOldestFirst() {
        let store = HistoryStore.inMemory()
        let now = Date()
        store.record(outboundMessage(id: 1, text: "older", deliveryState: .waiting,
                                      timestamp: now.addingTimeInterval(-100)), in: .member(1))
        store.record(outboundMessage(id: 2, text: "newer", deliveryState: .waiting,
                                      timestamp: now.addingTimeInterval(-10)), in: .member(1))
        store.record(outboundMessage(id: 3, text: "already delivered", deliveryState: .delivered,
                                      timestamp: now.addingTimeInterval(-50)), in: .member(1))
        store.record(FeedMessage(id: 4, kind: .text, direction: .direct, text: "inbound", timestamp: now),
                     in: .member(1))

        let pending = store.pendingOutbox(cap: 8)
        XCTAssertEqual(pending.map(\.1.text), ["older", "newer"], "WAITING only, oldest first")
    }

    func testPendingOutboxRespectsTheCap() {
        let store = HistoryStore.inMemory()
        for i in 0..<5 {
            store.record(outboundMessage(id: UInt64(i + 1), text: "m\(i)", deliveryState: .waiting,
                                          timestamp: Date().addingTimeInterval(TimeInterval(i))), in: .member(1))
        }
        XCTAssertEqual(store.pendingOutbox(cap: 3).count, 3)
        XCTAssertEqual(store.pendingOutbox(cap: 3).map(\.1.text), ["m0", "m1", "m2"])
    }

    // MARK: - Clear

    func testClearAllRemovesEveryRow() {
        let store = HistoryStore.inMemory()
        store.record(outboundMessage(id: 1), in: .member(1))
        store.record(outboundMessage(id: 2), in: .crew)
        XCTAssertEqual(store.loadAllForRestore().count, 2)

        store.clearAll()
        XCTAssertTrue(store.loadAllForRestore().isEmpty)
    }

    // MARK: - Pruning

    func testStoreBeyondPruneCapEvictsOldestFirst() {
        let store = HistoryStore.inMemory()
        let cap = HistoryStore.pruneCap
        let base = Date().addingTimeInterval(-TimeInterval(cap + 5) * 60)
        for i in 0..<(cap + 5) {
            store.record(
                FeedMessage(id: UInt64(i + 1), kind: .text, direction: .direct, text: "m\(i)",
                            timestamp: base.addingTimeInterval(TimeInterval(i) * 60)),
                in: .crew)
        }
        let all = store.loadAllForRestore()
        XCTAssertEqual(all.count, cap, "bounded, drop-oldest — the same policy ff_feed_t's own ring documents")
        XCTAssertFalse(all.contains { $0.1.text == "m0" }, "the oldest 5 were pruned")
        XCTAssertTrue(all.contains { $0.1.text == "m\(cap + 4)" }, "the newest survives")
    }

    // MARK: - ID generator watermarks (PR #281 review, BLOCKING 1)

    func testWatermarkDefaultsToZeroForAnUnknownKey() {
        let store = HistoryStore.inMemory()
        XCTAssertEqual(store.watermark(for: HistoryStore.outboxWatermarkKey), 0,
                        "a fresh store (or an old on-disk V1 store from before this fix) has no watermark row yet")
    }

    func testRaiseWatermarkPersistsAndNeverLowers() {
        let store = HistoryStore.inMemory()
        store.raiseWatermark(for: HistoryStore.outboxWatermarkKey, to: 50)
        XCTAssertEqual(store.watermark(for: HistoryStore.outboxWatermarkKey), 50)

        store.raiseWatermark(for: HistoryStore.outboxWatermarkKey, to: 10) // lower — must be a no-op
        XCTAssertEqual(store.watermark(for: HistoryStore.outboxWatermarkKey), 50,
                        "a watermark must never regress, the same monotonic contract OutboxIDGenerator.seed(atLeast:) carries")

        store.raiseWatermark(for: HistoryStore.outboxWatermarkKey, to: 75)
        XCTAssertEqual(store.watermark(for: HistoryStore.outboxWatermarkKey), 75)
    }

    func testWatermarksForDifferentKeysAreIndependent() {
        let store = HistoryStore.inMemory()
        store.raiseWatermark(for: HistoryStore.outboxWatermarkKey, to: 5)
        store.raiseWatermark(for: HistoryStore.inboundWatermarkKey, to: 0x8000_0000_0000_0005)
        XCTAssertEqual(store.watermark(for: HistoryStore.outboxWatermarkKey), 5)
        XCTAssertEqual(store.watermark(for: HistoryStore.inboundWatermarkKey), 0x8000_0000_0000_0005)
    }

    // MARK: - Isolation between instances

    func testTwoInMemoryStoresNeverShareData() {
        let a = HistoryStore.inMemory()
        let b = HistoryStore.inMemory()
        a.record(outboundMessage(id: 1, text: "only in a"), in: .crew)
        XCTAssertTrue(b.loadAllForRestore().isEmpty, "each in-memory store is its own container")
    }
}
