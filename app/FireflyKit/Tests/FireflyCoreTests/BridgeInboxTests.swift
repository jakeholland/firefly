//
//  BridgeInboxTests.swift — InboxBridge (ff_feed + ff_inbox) through the
//  Swift bridge: conversations, threads, mark-read, and the outbox
//  delivery-status progression including the broadcast-never-DELIVERED
//  rule and the explicit `.none` case (docs/specs/A01-companion-app.md's
//  "delivery states incl. NONE").
//
import FireflyCore
@testable import FireflyModel
import XCTest

final class BridgeInboxTests: XCTestCase {

    func testCrewConversationIsAlwaysPresentEvenWithNoTraffic() {
        let crew = CrewStore(now: { 0 })
        let inbox = InboxBridge()
        let convs = inbox.conversations(crew: crew, now: 0)
        XCTAssertEqual(convs.count, 1)
        XCTAssertEqual(convs.first?.kind, .crew)
        XCTAssertFalse(convs.first!.hasPreview)
    }

    func testAPairedMemberGetsAConversation() {
        let crew = CrewStore(now: { 0 })
        crew.setPaired(nodeID: 1, paired: true)
        let inbox = InboxBridge()
        let convs = inbox.conversations(crew: crew, now: 0)
        XCTAssertTrue(convs.contains { $0.kind == .member(1) })
    }

    /// A DIRECT item from a sender who is not (or no longer) paired
    /// belongs to no conversation — honestly absent, never attributed
    /// to a fabricated identity.
    func testUnpairedSenderGetsNoConversation() {
        let crew = CrewStore(now: { 0 })
        crew.upsert(nodeID: 99) // known, but not paired
        let inbox = InboxBridge()
        inbox.push(FeedItem(kind: .text, fromNode: 99, atMs: 0, text: "hi", direction: .direct))
        let convs = inbox.conversations(crew: crew, now: 0)
        XCTAssertFalse(convs.contains { $0.kind == .member(99) })
    }

    func testThreadOrdersOldestFirst() {
        let crew = CrewStore(now: { 0 })
        crew.setPaired(nodeID: 1, paired: true)
        let inbox = InboxBridge()
        inbox.push(FeedItem(kind: .text, fromNode: 1, atMs: 1_000, text: "first", direction: .direct))
        inbox.push(FeedItem(kind: .text, fromNode: 1, atMs: 2_000, text: "second", direction: .direct))
        let thread = inbox.thread(.member(1), crew: crew, now: 3_000)
        XCTAssertEqual(thread.map(\.text), ["first", "second"])
    }

    func testMarkThreadReadOnlyTouchesThatConversation() {
        let crew = CrewStore(now: { 0 })
        crew.setPaired(nodeID: 1, paired: true)
        crew.setPaired(nodeID: 2, paired: true)
        let inbox = InboxBridge()
        inbox.push(FeedItem(kind: .text, fromNode: 1, atMs: 0, text: "a", direction: .direct))
        inbox.push(FeedItem(kind: .text, fromNode: 2, atMs: 0, text: "b", direction: .direct))
        XCTAssertEqual(inbox.unreadCount, 2)

        let marked = inbox.markThreadRead(.member(1))
        XCTAssertEqual(marked, 1)
        XCTAssertEqual(inbox.unreadCount, 1, "the other member's unread item must survive untouched")
    }

    // MARK: - Outbox delivery status (S24 amendment).

    func testInboundItemsAlwaysCarryNoneStatus() {
        let crew = CrewStore(now: { 0 })
        crew.setPaired(nodeID: 1, paired: true)
        let inbox = InboxBridge()
        inbox.push(FeedItem(kind: .text, fromNode: 1, atMs: 0, text: "hi", direction: .direct))
        let msg = inbox.thread(.member(1), crew: crew, now: 0).first
        // `.none` alone is ambiguous here (Optional<FeedSendStatus>.none
        // vs. FeedSendStatus.none) — spelled out to unambiguously assert
        // the CORE enum's own zero value, not "no message decoded".
        XCTAssertEqual(msg?.sendStatus, FeedSendStatus.none)
    }

    /// `outbox_id == 0` (the "not tracked" sentinel, ff_feed.h's own doc
    /// comment) never matches — a no-op, not an error. This is what
    /// `testOutboxProgressionWaitingToSentToDelivered` (PR #261 review,
    /// finding 6) was actually testing despite its name; see
    /// `testOutboxProgressionWaitingToSentToDelivered` below for the
    /// real WAITING -> SENT -> DELIVERED progression this one's name
    /// used to promise.
    func testOutboxIdZeroIsANoOpSentinel() {
        let crew = CrewStore(now: { 0 })
        crew.setPaired(nodeID: 1, paired: true)
        let inbox = InboxBridge()
        inbox.push(FeedItem(kind: .text, fromNode: 0, atMs: 0, text: "omw", direction: .out, toNode: 1),
                   unread: false)
        inbox.setSendStatus(outboxID: OutboxID(0), status: .waiting, atMs: 0)
        XCTAssertEqual(inbox.thread(.member(1), crew: crew, now: 0).first?.sendStatus, FeedSendStatus.none,
                       "outbox_id 0 never matches — 0 is the documented sentinel for 'not tracked'")
    }

    /// The genuine round-trip `markSent`/`setAck` progression (PR #261
    /// review, finding 3): push with a real `outboxID`, `markSent` with
    /// a DIFFERENT `packetID` (the way a real client actually assigns
    /// the two), then `setAck` by that `packetID` — never the
    /// `outboxID`, which `setAck` never even takes.
    func testOutboxProgressionWaitingToSentToDelivered() {
        let crew = CrewStore(now: { 0 })
        crew.setPaired(nodeID: 1, paired: true)
        let inbox = InboxBridge()
        let outboxID = OutboxID(101)
        let packetID = PacketID(9_202)
        inbox.push(FeedItem(kind: .text, fromNode: 0, atMs: 0, text: "omw", direction: .out, toNode: 1,
                             outboxID: outboxID), unread: false)
        inbox.setSendStatus(outboxID: outboxID, status: .waiting, atMs: 0)
        XCTAssertEqual(inbox.thread(.member(1), crew: crew, now: 0).first?.sendStatus, .waiting)

        inbox.markSent(outboxID: outboxID, packetID: packetID, wantAck: true, atMs: 10)
        XCTAssertEqual(inbox.thread(.member(1), crew: crew, now: 0).first?.sendStatus, .sent)

        let resolved = inbox.setAck(packetID: packetID, ok: true, atMs: 20)
        XCTAssertTrue(resolved, "setAck must report finding and resolving the item")
        XCTAssertEqual(inbox.thread(.member(1), crew: crew, now: 0).first?.sendStatus, .delivered)
    }

    /// `setAck` by `outboxID`'s VALUE reinterpreted as a `packetID` must
    /// NOT resolve anything — the two id spaces are unrelated even when
    /// their raw numbers happen to collide, and this bridge's own
    /// distinct `OutboxID`/`PacketID` types make constructing that
    /// mistake by accident a compile error, not just a logic bug.
    func testSetAckByOutboxIdValueDoesNotResolveTheItem() {
        let crew = CrewStore(now: { 0 })
        crew.setPaired(nodeID: 1, paired: true)
        let inbox = InboxBridge()
        let outboxID = OutboxID(55)
        inbox.push(FeedItem(kind: .text, fromNode: 0, atMs: 0, text: "omw", direction: .out, toNode: 1,
                             outboxID: outboxID), unread: false)
        inbox.markSent(outboxID: outboxID, packetID: PacketID(777), wantAck: true, atMs: 0)

        // The SAME raw value as outboxID (55), but as a PacketID — this
        // item's real packetID is 777, so this must not match.
        let resolved = inbox.setAck(packetID: PacketID(outboxID.rawValue), ok: true, atMs: 10)
        XCTAssertFalse(resolved)
        XCTAssertEqual(inbox.thread(.member(1), crew: crew, now: 0).first?.sendStatus, .sent)
    }

    /// A broadcast never resolves to DELIVERED — the mesh gives no
    /// receipt for one; it stays SENT forever.
    func testBroadcastNeverReachesDelivered() {
        let crew = CrewStore(now: { 0 })
        let inbox = InboxBridge()
        inbox.push(FeedItem(kind: .text, fromNode: 0, atMs: 0, text: "hey crew", direction: .out, toNode: 0),
                   unread: false)
        let msg = inbox.thread(.crew, crew: crew, now: 0).first
        XCTAssertEqual(msg?.direction, .out)
        XCTAssertEqual(msg?.sendStatus, FeedSendStatus.none, "no outbox_id was ever stamped for this push")
    }

    func testDuplicatePacketIdEchoDoesNotOverwriteTheSentRow() {
        let crew = CrewStore(now: { 0 })
        crew.setPaired(nodeID: 1, paired: true)
        let inbox = InboxBridge()
        inbox.push(FeedItem(kind: .text, fromNode: 0, atMs: 0, text: "sent by me", direction: .out, toNode: 1),
                   unread: false)
        // The mesh echoes our own packet back; a dedup guard upstream
        // (slice A/E) is what prevents a second push for the same
        // packet id — this bridge's own job is only to never invent a
        // duplicate on its own when asked to push exactly once.
        XCTAssertEqual(inbox.itemCount, 1)
        let thread = inbox.thread(.member(1), crew: crew, now: 0)
        XCTAssertEqual(thread.count, 1)
        XCTAssertEqual(thread.first?.text, "sent by me")
    }

    func testFeedSendStatusEnumeratesAllSixCoreValuesIncludingNone() {
        XCTAssertEqual(FeedSendStatus.allCases.count, 6)
        XCTAssertTrue(FeedSendStatus.allCases.contains(.none))
    }

    // MARK: - Enum-growth guards (PR #261 review, finding 2 — the
    // `DeliveryStateTests.testEveryStateMapsToTheCEnum` pattern: pin
    // every known C raw value to its Swift case AND `allCases.count`, so
    // a new case added to either side without the other is caught here
    // rather than silently folding into an existing case via the
    // `init(ff...:)` initializers' `default:` branch.

    func testFeedKindEnumeratesAllFourCoreValues() {
        XCTAssertEqual(FeedKind(ffKind: FEED_TEXT), .text)
        XCTAssertEqual(FeedKind(ffKind: FEED_RALLY), .rally)
        XCTAssertEqual(FeedKind(ffKind: FEED_STATUS), .status)
        XCTAssertEqual(FeedKind(ffKind: FEED_FLARE), .flare)
        XCTAssertEqual(FeedKind.text.ffValue, FEED_TEXT)
        XCTAssertEqual(FeedKind.rally.ffValue, FEED_RALLY)
        XCTAssertEqual(FeedKind.status.ffValue, FEED_STATUS)
        XCTAssertEqual(FeedKind.flare.ffValue, FEED_FLARE)
        XCTAssertEqual(FeedKind.allCases.count, 4, "a new ff_feed_kind_t value needs a matching FeedKind case")
    }

    func testFeedDirectionEnumeratesAllFourCoreValues() {
        XCTAssertEqual(FeedDirection(ffDir: FEED_DIR_UNKNOWN), .unknown)
        XCTAssertEqual(FeedDirection(ffDir: FEED_DIR_BROADCAST), .broadcast)
        XCTAssertEqual(FeedDirection(ffDir: FEED_DIR_DIRECT), .direct)
        XCTAssertEqual(FeedDirection(ffDir: FEED_DIR_OUT), .out)
        XCTAssertEqual(FeedDirection.unknown.ffValue, FEED_DIR_UNKNOWN)
        XCTAssertEqual(FeedDirection.broadcast.ffValue, FEED_DIR_BROADCAST)
        XCTAssertEqual(FeedDirection.direct.ffValue, FEED_DIR_DIRECT)
        XCTAssertEqual(FeedDirection.out.ffValue, FEED_DIR_OUT)
        XCTAssertEqual(FeedDirection.allCases.count, 4, "a new ff_feed_dir_t value needs a matching FeedDirection case")
    }

    func testSigviewPresenceEnumeratesAllThreeCoreValues() {
        XCTAssertEqual(SigviewPresence(ffPresence: FF_PRESENCE_SEEN), .seen)
        XCTAssertEqual(SigviewPresence(ffPresence: FF_PRESENCE_LOST), .lost)
        XCTAssertEqual(SigviewPresence(ffPresence: FF_PRESENCE_LINKED), .linked)
        XCTAssertEqual(SigviewPresence.allCases.count, 3,
                       "a new ff_sigview_presence_t value needs a matching SigviewPresence case")
    }
}
