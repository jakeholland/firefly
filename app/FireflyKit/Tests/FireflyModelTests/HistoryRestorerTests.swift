//
//  HistoryRestorerTests.swift — the cold-launch restore algorithm,
//  pinned against `InMemoryInboxStore` (no C core, no disk — pure logic;
//  `AppGraphTests`' own M3 section covers the real `CoreInboxProvider` +
//  a real client's want_config-shaped replay end to end).
//
import FireflyMesh
import FireflyModel
import XCTest

@MainActor
final class HistoryRestorerTests: XCTestCase {

    private func outbound(id: UInt64, text: String, deliveryState: DeliveryState?, ageSeconds: TimeInterval,
                           packetID: UInt32? = nil, destination: UInt32 = 42) -> (ConversationKind, FeedMessage) {
        let timestamp = Date().addingTimeInterval(-ageSeconds)
        return (.member(destination), FeedMessage(id: id, kind: .text, direction: .out, text: text,
                                                    timestamp: timestamp, destination: destination,
                                                    packetID: packetID, deliveryState: deliveryState,
                                                    statusAt: timestamp))
    }

    private func inbound(id: UInt64, text: String, ageSeconds: TimeInterval,
                          conversation: ConversationKind = .crew) -> (ConversationKind, FeedMessage) {
        let timestamp = Date().addingTimeInterval(-ageSeconds)
        return (conversation, FeedMessage(id: id, kind: .text, direction: .broadcast, text: text,
                                           timestamp: timestamp))
    }

    // MARK: - SENT -> NO ACK

    func testSentRestoresAsNoAckNeverAsStillSent() {
        let provider = InMemoryInboxStore()
        let all = [outbound(id: 1, text: "was sent, no ack yet", deliveryState: .sent, ageSeconds: 3600,
                             packetID: 555)]

        HistoryRestorer.restore(all, into: provider)

        let message = provider.thread(for: .member(42), now: Date()).first
        XCTAssertEqual(message?.deliveryState, .noAck,
                        "an ack cannot arrive after relaunch — SENT restores as NO ACK, never as still-pending SENT")
    }

    func testDeliveredAndDroppedRestoreUnchanged() {
        let provider = InMemoryInboxStore()
        let all = [
            outbound(id: 1, text: "delivered", deliveryState: .delivered, ageSeconds: 3600, packetID: 1),
            outbound(id: 2, text: "dropped", deliveryState: .dropped, ageSeconds: 3600),
        ]
        HistoryRestorer.restore(all, into: provider)

        let thread = provider.thread(for: .member(42), now: Date()).sorted { $0.timestamp < $1.timestamp }
        XCTAssertEqual(thread.map(\.deliveryState), [.delivered, .dropped])
    }

    func testAlreadyNoAckStaysNoAck() {
        let provider = InMemoryInboxStore()
        let all = [outbound(id: 1, text: "already no ack", deliveryState: .noAck, ageSeconds: 3600, packetID: 1)]
        HistoryRestorer.restore(all, into: provider)
        XCTAssertEqual(provider.thread(for: .member(42), now: Date()).first?.deliveryState, .noAck)
    }

    // MARK: - WAITING is unconditional

    func testWaitingItemsAreAlwaysIncludedEvenBeyondTheCap() {
        let provider = InMemoryInboxStore()
        // `cap` other, older messages that would otherwise fill the
        // whole ring, PLUS one WAITING item older than all of them.
        var all: [(ConversationKind, FeedMessage)] = []
        for i in 0..<HistoryRestorer.coreReseedCap {
            all.append(inbound(id: UInt64(i + 100), text: "filler \(i)", ageSeconds: TimeInterval(i)))
        }
        all.append(outbound(id: 999, text: "still waiting, very old", deliveryState: .waiting, ageSeconds: 999_999))

        let restoredIDs = HistoryRestorer.restore(all, into: provider, cap: HistoryRestorer.coreReseedCap)

        let waitingMessage = provider.thread(for: .member(42), now: Date()).first
        XCTAssertEqual(waitingMessage?.text, "still waiting, very old",
                        "a WAITING item is never dropped for being old — it is unfinished business, not history")
        XCTAssertEqual(waitingMessage?.deliveryState, .waiting)
        XCTAssertTrue(restoredIDs.contains(999))
    }

    // MARK: - Cap and ordering

    func testOnlyTheMostRecentNonWaitingItemsAreReseededUpToTheCap() {
        let provider = InMemoryInboxStore()
        let cap = 5
        let all = (0..<10).map { inbound(id: UInt64($0), text: "m\($0)", ageSeconds: TimeInterval(9 - $0)) }
        // ages: m0 is oldest (age 9), m9 is newest (age 0).

        HistoryRestorer.restore(all, into: provider, cap: cap)

        let texts = provider.thread(for: .crew, now: Date()).sorted { $0.timestamp < $1.timestamp }.map(\.text)
        XCTAssertEqual(texts, ["m5", "m6", "m7", "m8", "m9"], "only the CAP most recent, oldest of THOSE first")
    }

    func testReseedCapMirrorsTheCoreRingCapacity() {
        // FF_FEED_CAP, firmware/core/include/ff_feed.h — pinned here as a
        // literal so a future change to either side is a visible diff,
        // not a silent drift (`FireflyCoreTests`' own drift-guard
        // philosophy, applied to a Swift-side constant with no direct
        // C symbol to import).
        XCTAssertEqual(HistoryRestorer.coreReseedCap, 32)
    }

    // MARK: - restoredMessageIDs

    func testRestoredMessageIDsReflectExactlyWhatWasPushed() {
        let provider = InMemoryInboxStore()
        let all = [inbound(id: 1, text: "a", ageSeconds: 10), inbound(id: 2, text: "b", ageSeconds: 5)]
        let restoredIDs = HistoryRestorer.restore(all, into: provider)

        let liveThread = Set(provider.thread(for: .crew, now: Date()).map(\.id))
        XCTAssertEqual(restoredIDs, liveThread, "every id restore just pushed, and nothing else")
    }

    func testRestoringNothingReturnsAnEmptySet() {
        let provider = InMemoryInboxStore()
        XCTAssertTrue(HistoryRestorer.restore([], into: provider).isEmpty)
        XCTAssertTrue(provider.thread(for: .crew, now: Date()).isEmpty)
    }

    // MARK: - Multiple conversations

    func testRestoreAcrossConversationsPreservesEachOnesMembership() {
        let provider = InMemoryInboxStore()
        let all = [
            inbound(id: 1, text: "crew chatter", ageSeconds: 30, conversation: .crew),
            outbound(id: 2, text: "dm to taylor", deliveryState: .delivered, ageSeconds: 20, packetID: 1,
                     destination: 0x1002),
        ]
        HistoryRestorer.restore(all, into: provider)

        XCTAssertEqual(provider.thread(for: .crew, now: Date()).map(\.text), ["crew chatter"])
        XCTAssertEqual(provider.thread(for: .member(0x1002), now: Date()).map(\.text), ["dm to taylor"])
    }
}
