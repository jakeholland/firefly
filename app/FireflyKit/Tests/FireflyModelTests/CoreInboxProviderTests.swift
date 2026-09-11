//
//  CoreInboxProviderTests.swift — `InboxProviding` over the real
//  `ff_feed`/`ff_inbox`/`ff_crew`, the provider the live graph uses.
//
//  `InboxViewModelTests` pins `InMemoryInboxStore` (the test-only
//  stand-in) against the membership/ordering rules `ff_inbox.h`
//  DOCUMENTS. This file pins the live provider against the C code that
//  IMPLEMENTS them — which is what makes the stand-in's transcription
//  checkable rather than merely plausible.
//
import FireflyMesh
import FireflyModel
import XCTest

@MainActor
final class CoreInboxProviderTests: XCTestCase {

    private func makeProvider() -> (CoreInboxProvider, InboxBridge, CrewStore) {
        let inbox = InboxBridge()
        let crew = CrewStore()
        return (CoreInboxProvider(inbox: inbox, crew: crew), inbox, crew)
    }

    /// A paired member with an identity, so conversations for them exist
    /// ("A conversation exists only for CREW and for PAIRED members").
    private func pair(_ crew: CrewStore, nodeID: UInt32, shortName: String, longName: String) {
        crew.setIdentity(nodeID: nodeID, shortName: shortName, longName: longName)
        crew.setPaired(nodeID: nodeID, paired: true)
        crew.onHeard(nodeID: nodeID, rxTimeMs: FireflyClock.nowMillis(), direct: true)
    }

    func testEmptyProviderHasCrewOnlyAndInventsNoTraffic() {
        let (provider, _, _) = makeProvider()
        let rows = provider.conversations(now: Date())
        XCTAssertEqual(rows.count, 1)
        XCTAssertEqual(rows[0].kind, .crew)
        XCTAssertEqual(rows[0].displayName, "CREW")
        XCTAssertEqual(rows[0].itemCount, 0)
        XCTAssertFalse(rows[0].hasPreview)
        XCTAssertNil(rows[0].previewDeliveryState)
        XCTAssertTrue(provider.thread(for: .crew, now: Date()).isEmpty)
    }

    func testPairedMemberGetsARowWithTheIdentityTheMeshReported() throws {
        let (provider, _, crew) = makeProvider()
        pair(crew, nodeID: 42, shortName: "RILE", longName: "Riley")

        let rows = provider.conversations(now: Date())
        let member = try XCTUnwrap(rows.first { $0.kind == .member(42) })
        XCTAssertEqual(member.displayName, "Riley", "ff_crew_display_name: the long name when known")
        XCTAssertEqual(member.initial, "R")
        XCTAssertEqual(member.presence, .heard)
    }

    /// The keyed join `InboxBridge.records(in:)` exists for: an outbound
    /// message's `FeedMessage.id` must come back as the OUTBOX ID it was
    /// pushed with, or `markSent`/`setStatus` can never find it again.
    func testOutboundMessageKeepsItsOutboxIDThroughTheCCore() {
        let (provider, _, crew) = makeProvider()
        pair(crew, nodeID: 42, shortName: "RILE", longName: "Riley")
        let now = Date()

        provider.push(FeedMessage(id: 77, kind: .text, direction: .out, text: "omw", timestamp: now,
                                   destination: 42, deliveryState: .waiting, statusAt: now),
                       into: .member(42))

        let thread = provider.thread(for: .member(42), now: now)
        XCTAssertEqual(thread.count, 1)
        XCTAssertEqual(thread[0].id, 77)
        XCTAssertEqual(thread[0].text, "omw")
        XCTAssertEqual(thread[0].direction, .out)
        XCTAssertEqual(thread[0].deliveryState, .waiting, "WAITING is visible the instant SEND is pressed")
    }

    func testDeliveryStateProgressesThroughTheCCoreKeyedOnTheRightIDs() {
        let (provider, _, crew) = makeProvider()
        pair(crew, nodeID: 42, shortName: "RILE", longName: "Riley")
        let now = Date()

        provider.push(FeedMessage(id: 77, kind: .text, direction: .out, text: "omw", timestamp: now,
                                   destination: 42, deliveryState: .waiting, statusAt: now),
                       into: .member(42))
        // SENT is keyed on the outbox id; DELIVERED on the packet id.
        provider.markSent(outboxID: 77, packetID: 9001, at: now)
        XCTAssertEqual(provider.thread(for: .member(42), now: now).first?.deliveryState, .sent)
        XCTAssertEqual(provider.thread(for: .member(42), now: now).first?.packetID, 9001)

        provider.setStatus(packetID: 9001, state: .delivered, at: now)
        XCTAssertEqual(provider.thread(for: .member(42), now: now).first?.deliveryState, .delivered)
    }

    func testBroadcastNeverReachesDeliveredThroughTheCCore() {
        let (provider, _, _) = makeProvider()
        let now = Date()
        provider.push(FeedMessage(id: 5, kind: .text, direction: .out, text: "hey crew", timestamp: now,
                                   destination: meshBroadcastAddress, deliveryState: .waiting, statusAt: now),
                       into: .crew)
        provider.markSent(outboxID: 5, packetID: 4242, at: now)
        // `ff_feed_set_ack_by_packet_id`'s own SENT+want_ack precondition
        // is what refuses this — nothing acks a broadcast, so `markSent`
        // recorded want_ack == false for it (the destination said so).
        provider.setStatus(packetID: 4242, state: .delivered, at: now)

        XCTAssertEqual(provider.thread(for: .crew, now: now).first?.deliveryState, .sent,
                        "\"delivered to the mesh\" is not delivery to a person")
    }

    func testInboundMessageGetsAStableDerivedIDDisjointFromOutboxIDs() throws {
        let (provider, _, crew) = makeProvider()
        pair(crew, nodeID: 42, shortName: "RILE", longName: "Riley")
        let now = Date()

        provider.push(FeedMessage(id: 0x8000_0000_0000_0001, kind: .text, direction: .direct,
                                   senderID: 42, text: "here", timestamp: now, packetID: 31),
                       into: .member(42))

        let first = provider.thread(for: .member(42), now: now)
        let second = provider.thread(for: .member(42), now: now)
        XCTAssertEqual(first.count, 1)
        XCTAssertEqual(first.map(\.id), second.map(\.id), "the same item must render with the same id every build")
        let id = try XCTUnwrap(first.first?.id)
        XCTAssertEqual(id & 0x8000_0000_0000_0000, 0x8000_0000_0000_0000,
                        "an inbound id must stay out of the outbox id space")
        XCTAssertEqual(first.first?.senderName, "Riley", "identity is JOINED from the roster, never fabricated")
    }

    func testUnknownSenderRendersWithNoNameRatherThanAFabricatedOne() {
        let (provider, _, _) = makeProvider()
        let now = Date()
        provider.push(FeedMessage(id: 0x8000_0000_0000_0002, kind: .text, direction: .broadcast,
                                   senderID: 777, text: "who is this", timestamp: now, packetID: 5),
                       into: .crew)

        let thread = provider.thread(for: .crew, now: now)
        XCTAssertEqual(thread.count, 1)
        XCTAssertEqual(thread[0].senderID, 777)
        XCTAssertNil(thread[0].senderName, "not a paired member: no identity to show, and none invented")
    }

    func testEchoOfMyOwnBroadcastIsDroppedButAStrangerReusingTheIDIsNot() {
        let (provider, _, _) = makeProvider()
        let now = Date()

        provider.push(FeedMessage(id: 11, kind: .text, direction: .out, text: "mine", timestamp: now,
                                   destination: meshBroadcastAddress, deliveryState: .waiting, statusAt: now),
                       into: .crew)
        provider.markSent(outboxID: 11, packetID: 500, at: now)

        // The mesh reflects my own broadcast back at me with my packet id.
        provider.push(FeedMessage(id: 0x8000_0000_0000_0003, kind: .text, direction: .broadcast,
                                   senderID: 42, text: "mine", timestamp: now, packetID: 500),
                       into: .crew)
        XCTAssertEqual(provider.thread(for: .crew, now: now).count, 1, "my own echo is dropped once")

        // A DIFFERENT packet id from a stranger is always kept — the
        // dedup memory is MY sent ids only, never "every id ever seen".
        provider.push(FeedMessage(id: 0x8000_0000_0000_0004, kind: .text, direction: .broadcast,
                                   senderID: 42, text: "theirs", timestamp: now, packetID: 501),
                       into: .crew)
        XCTAssertEqual(provider.thread(for: .crew, now: now).count, 2)
    }

    func testMarkReadClearsOnlyThisThreadsUnreadCount() {
        let (provider, bridge, crew) = makeProvider()
        pair(crew, nodeID: 42, shortName: "RILE", longName: "Riley")
        pair(crew, nodeID: 43, shortName: "SAM", longName: "Sam")
        let now = Date()

        provider.push(FeedMessage(id: 0x8000_0000_0000_0010, kind: .text, direction: .direct, senderID: 42,
                                   text: "a", timestamp: now, unread: true, packetID: 1), into: .member(42))
        provider.push(FeedMessage(id: 0x8000_0000_0000_0011, kind: .text, direction: .direct, senderID: 43,
                                   text: "b", timestamp: now, unread: true, packetID: 2), into: .member(43))
        XCTAssertEqual(bridge.unreadCount, 2)

        XCTAssertEqual(provider.markRead(.member(42)), 1)
        XCTAssertEqual(bridge.unreadCount, 1, "opening one thread must not read another's badge")
    }

    func testAnUnpairedSendersDirectMessageBelongsToNoConversation() {
        let (provider, bridge, _) = makeProvider()
        let now = Date()

        provider.push(FeedMessage(id: 0x8000_0000_0000_0020, kind: .text, direction: .direct, senderID: 900,
                                   text: "hello", timestamp: now, unread: true, packetID: 3), into: .member(900))

        // `ff_inbox.h`: "a DIRECT item from a sender who is not (or no
        // longer) a paired roster member belongs to NO conversation: it
        // is honestly ABSENT from the inbox model... rather than
        // attributed to a fabricated identity". The item is really in
        // the feed — its unread still counts globally — it simply has no
        // row to appear in.
        XCTAssertEqual(bridge.itemCount, 1)
        XCTAssertEqual(bridge.unreadCount, 1)
        XCTAssertEqual(provider.conversations(now: now).map(\.kind), [.crew])
    }

    func testConversationOrderingComesFromTheCCoreNotFromSwift() {
        let (provider, _, crew) = makeProvider()
        pair(crew, nodeID: 42, shortName: "RILE", longName: "Riley")
        pair(crew, nodeID: 43, shortName: "SAM", longName: "Sam")
        let now = Date()

        // One unread member thread; CREW quiet. S24 AC2 puts the unread
        // conversation first, ahead of even CREW.
        provider.push(FeedMessage(id: 0x8000_0000_0000_0030, kind: .text, direction: .direct, senderID: 43,
                                   text: "unread", timestamp: now, unread: true, packetID: 4), into: .member(43))

        let rows = provider.conversations(now: now)
        XCTAssertEqual(rows.first?.kind, .member(43))
        XCTAssertEqual(rows.first?.unreadCount, 1)
        XCTAssertTrue(rows.contains { $0.kind == .crew }, "CREW is always present")
    }

    /// M3 regression: `thread(for:now:)`'s own protocol doc comment
    /// ("oldest first") — found while building the "FROM STORAGE"
    /// preview tag, which reads `thread(for:).last` to mean "newest".
    /// `InboxBridge.records(in:)` walks the ring NEWEST FIRST
    /// internally; this pins that `CoreInboxProvider` re-sorts before
    /// handing anything back, matching `InMemoryInboxStore`'s own
    /// already-sorted contract.
    func testThreadOrdersOldestFirstMatchingInboxProvidingsContract() {
        let (provider, _, _) = makeProvider()
        let now = Date()
        provider.push(FeedMessage(id: 1, kind: .text, direction: .broadcast, senderID: 1, text: "first",
                                   timestamp: now.addingTimeInterval(-100)), into: .crew)
        provider.push(FeedMessage(id: 2, kind: .text, direction: .broadcast, senderID: 1, text: "second",
                                   timestamp: now.addingTimeInterval(-50)), into: .crew)
        provider.push(FeedMessage(id: 3, kind: .text, direction: .broadcast, senderID: 1, text: "third",
                                   timestamp: now), into: .crew)

        XCTAssertEqual(provider.thread(for: .crew, now: now).map(\.text), ["first", "second", "third"])
    }

    /// M3 regression: the Inbox row's `previewDeliveryState` must come
    /// from the NEWEST item, not the oldest — the exact bug this file's
    /// header would have masked forever, since no earlier test pushed a
    /// second item into the same conversation before asserting on it.
    func testPreviewDeliveryStateReflectsTheNewestItemNotTheOldest() {
        let (provider, _, crew) = makeProvider()
        pair(crew, nodeID: 42, shortName: "RILE", longName: "Riley")
        let now = Date()
        provider.push(FeedMessage(id: 1, kind: .text, direction: .out, text: "older", timestamp: now.addingTimeInterval(-100),
                                   destination: 42, deliveryState: .waiting, statusAt: now.addingTimeInterval(-100)),
                       into: .member(42))
        provider.markSent(outboxID: 1, packetID: 900, at: now.addingTimeInterval(-100))
        provider.setStatus(packetID: 900, state: .delivered, at: now.addingTimeInterval(-90))

        provider.push(FeedMessage(id: 2, kind: .text, direction: .out, text: "newer", timestamp: now,
                                   destination: 42, deliveryState: .waiting, statusAt: now), into: .member(42))

        let row = provider.conversations(now: now).first { $0.kind == .member(42) }
        XCTAssertEqual(row?.previewText, "newer")
        XCTAssertEqual(row?.previewDeliveryState, .waiting, "the NEWEST item's own state, not the older DELIVERED one")
    }
}
