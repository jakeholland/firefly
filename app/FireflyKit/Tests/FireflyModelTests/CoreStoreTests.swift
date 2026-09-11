//
//  CoreStoreTests.swift — CoreStore and a view model can observe the
//  SAME client independently, without stealing each other's events
//  (docs/specs/A01-companion-app.md, S1). This is the regression test
//  for the bug the single-consumer AsyncStream design would have had.
//
import FireflyMesh
import FireflyModel
import XCTest

@MainActor
final class CoreStoreTests: XCTestCase {

    func testCoreStoreAndAViewModelBothReachReadyFromTheSameClient() async {
        let client = StubMeshtasticClient()
        let vm = ConnectViewModel(client: client)
        let store = CoreStore()

        vm.observe()
        store.observe(client: client)
        await vm.connect()

        for _ in 0..<200 where vm.link != .ready || store.linkState != .ready {
            try? await Task.sleep(nanoseconds: 5_000_000)
        }

        XCTAssertEqual(vm.link, .ready, "the view model's own subscription must still see .ready")
        XCTAssertEqual(store.linkState, .ready,
                        "CoreStore's independent subscription must ALSO see .ready — "
                        + "a single-consumer AsyncStream would have let one of these steal the other's events")
    }

    func testObserveIsIdempotent() {
        let store = CoreStore()
        let client = StubMeshtasticClient()
        store.observe(client: client)
        store.observe(client: client) // must not crash or double-subscribe
        store.stopObserving()
    }

    // MARK: - apply(nodeUpdate:) lands in ff_crew_t (slice B).
    //
    // docs/specs/A01-companion-app.md, slice B's acceptance: "CoreStore's
    // apply() hooks are real and CoreStoreTests ... grows tests that
    // they land in ff_crew_t/ff_feed_t." These call `apply` directly
    // (the same entry point `nodeObservation`'s `for await` loop calls)
    // rather than routing bytes through a whole client, so the
    // assertion is squarely about the bridge wiring, not the transport.

    func testApplyNodeUpdateWithAPositionLandsInCrew() {
        let store = CoreStore()
        let snapshot = MeshNodeSnapshot(
            num: 1, shortName: "TAYL", longName: "Taylor",
            position: NodePosition(latitude: 47.705785, longitude: -122.2820993, time: nil,
                                    source: .externalGPS, precisionBits: 32),
            lastHeard: nil, rssiDbm: nil, snrDb: nil, hopsAway: nil)

        store.apply(nodeUpdate: snapshot)

        let member = store.crew.member(nodeID: 1, now: FireflyClock.nowMillis())
        XCTAssertNotNil(member?.position)
        XCTAssertEqual(member?.position?.latitude ?? 0, 47.705785, accuracy: 0.0001)
        XCTAssertFalse(member?.position?.asserted ?? true, "externalGPS is a measurement, not an assertion")
    }

    /// LOC_MANUAL (`.manual`) must land as `asserted`, never as an aging
    /// measurement (issue #33) — the whole point of routing `meta`
    /// through, not just the coordinate.
    func testApplyNodeUpdateWithManualSourceLandsAsAsserted() {
        let store = CoreStore()
        let snapshot = MeshNodeSnapshot(
            num: 2, shortName: nil, longName: nil,
            position: NodePosition(latitude: 47.708135, longitude: -122.2820993, time: nil,
                                    source: .manual, precisionBits: nil),
            lastHeard: nil, rssiDbm: nil, snrDb: nil, hopsAway: nil)

        store.apply(nodeUpdate: snapshot)

        let member = store.crew.member(nodeID: 2, now: FireflyClock.nowMillis())
        XCTAssertEqual(member?.freshness, .asserted)
    }

    /// RSSI is only attributable when the packet arrived directly —
    /// `hopsAway == 0`. A relayed reading must never land as a direct
    /// signal.
    func testApplyNodeUpdateOnlyRecordsRSSIWhenDirect() {
        let store = CoreStore()
        let relayed = MeshNodeSnapshot(num: 3, shortName: nil, longName: nil, position: nil,
                                        lastHeard: nil, rssiDbm: -70, snrDb: nil, hopsAway: 2)
        store.apply(nodeUpdate: relayed)
        XCTAssertNil(store.crew.member(nodeID: 3, now: FireflyClock.nowMillis())?.directSignal)

        let direct = MeshNodeSnapshot(num: 4, shortName: nil, longName: nil, position: nil,
                                       lastHeard: nil, rssiDbm: -70, snrDb: nil, hopsAway: 0)
        store.apply(nodeUpdate: direct)
        XCTAssertEqual(store.crew.member(nodeID: 4, now: FireflyClock.nowMillis())?.directSignal?.rssiDbm, -70)
    }

    // MARK: - apply(delivery:) lands in ff_feed_t (slice B).
    //
    // PR #261 review, finding 1: the old versions of these tests hard-
    // coded outboxID == packetID (`outboxID: 7` / `packetID: 7`), which
    // manufactured the exact coincidence `apply(delivery:)`'s bug
    // silently depended on. Every test below uses a DIFFERENT outboxID
    // and packetID per message, the way a real client (distinct
    // `shell_next_outbox_id` counter vs. radio-assigned packet id)
    // actually would — so a regression that aliases the two keys again
    // fails here.

    /// WAITING -> SENT is keyed on `outboxID`; the routing ack that
    /// follows is keyed on the DIFFERENT `packetID` `.sent` stamped —
    /// exactly `ff_shell.c`'s own partitioning
    /// (`ff_feed_mark_sent_by_outbox_id` then `ff_feed_set_ack_by_packet_id`).
    func testApplyDeliverySentThenAckRoutesByTheCorrectDistinctKey() {
        let store = CoreStore()
        let crew = store.crew
        let inbox = store.inbox
        crew.setPaired(nodeID: 1, paired: true)
        let outboxID = FireflyModel.OutboxID(11)
        let packetID = FireflyModel.PacketID(4_002)
        inbox.push(FeedItem(kind: .text, fromNode: 0, atMs: 0, text: "omw", direction: .out, toNode: 1,
                             outboxID: outboxID), unread: false)

        // `apply(delivery:)` — the exact entry point `deliveryObservation`
        // routes into — must land SENT via the outboxID...
        store.apply(delivery: .sent(outboxID: FireflyMesh.OutboxID(outboxID.rawValue),
                                     packetID: FireflyMesh.PacketID(packetID.rawValue), wantAck: true))
        XCTAssertEqual(inbox.thread(.member(1), crew: crew, now: 0).first?.sendStatus, .sent)

        // ...and DELIVERED via the packetID `.sent` stamped, never the
        // outboxID (which `setAck` never even sees).
        store.apply(delivery: .delivered(packetID: FireflyMesh.PacketID(packetID.rawValue)))
        XCTAssertEqual(inbox.thread(.member(1), crew: crew, now: 0).first?.sendStatus, .delivered)
    }

    /// An explicit NAK (`.noAck`) resolves the same way DELIVERED does —
    /// by `packetID` — never touching `send_status` directly.
    func testApplyDeliveryNoAckRoutesByPacketId() {
        let store = CoreStore()
        store.crew.setPaired(nodeID: 1, paired: true)
        let outboxID = FireflyModel.OutboxID(21)
        let packetID = FireflyModel.PacketID(5_003)
        store.inbox.push(FeedItem(kind: .text, fromNode: 0, atMs: 0, text: "x", direction: .out, toNode: 1,
                                   outboxID: outboxID), unread: false)
        store.apply(delivery: .sent(outboxID: FireflyMesh.OutboxID(outboxID.rawValue),
                                     packetID: FireflyMesh.PacketID(packetID.rawValue), wantAck: true))
        store.apply(delivery: .noAck(packetID: FireflyMesh.PacketID(packetID.rawValue)))
        XCTAssertEqual(store.inbox.thread(.member(1), crew: store.crew, now: 0).first?.sendStatus, .noAck)
    }

    /// WAITING and DROPPED are keyed on `outboxID` alone (no packet ever
    /// existed for either) — `setSendStatus`, not `markSent`/`setAck`.
    func testApplyDeliveryWaitingAndDroppedRouteByOutboxIdAlone() {
        let waitingStore = CoreStore()
        waitingStore.crew.setPaired(nodeID: 1, paired: true)
        let waitingOutbox = FireflyModel.OutboxID(31)
        waitingStore.inbox.push(FeedItem(kind: .text, fromNode: 0, atMs: 0, text: "w", direction: .out,
                                          toNode: 1, outboxID: waitingOutbox), unread: false)
        waitingStore.apply(delivery: .waiting(outboxID: FireflyMesh.OutboxID(waitingOutbox.rawValue)))
        XCTAssertEqual(waitingStore.inbox.thread(.member(1), crew: waitingStore.crew, now: 0).first?.sendStatus,
                       .waiting)

        let droppedStore = CoreStore()
        droppedStore.crew.setPaired(nodeID: 1, paired: true)
        let droppedOutbox = FireflyModel.OutboxID(32)
        droppedStore.inbox.push(FeedItem(kind: .text, fromNode: 0, atMs: 0, text: "d", direction: .out,
                                          toNode: 1, outboxID: droppedOutbox), unread: false)
        droppedStore.apply(delivery: .dropped(outboxID: FireflyMesh.OutboxID(droppedOutbox.rawValue)))
        XCTAssertEqual(droppedStore.inbox.thread(.member(1), crew: droppedStore.crew, now: 0).first?.sendStatus,
                       .dropped)
    }

    /// An ack for a `packetID` this app never stamped onto anything
    /// (unknown to the feed entirely) is a safe, silent no-op — the item
    /// stays exactly where `.sent` left it. Matches `ff_feed_set_ack_by_
    /// packet_id`'s own documented "matches nothing -> no-op" contract.
    func testApplyDeliveryAckForUnknownPacketIdIsIgnored() {
        let store = CoreStore()
        store.crew.setPaired(nodeID: 1, paired: true)
        let outboxID = FireflyModel.OutboxID(41)
        let packetID = FireflyModel.PacketID(6_004)
        store.inbox.push(FeedItem(kind: .text, fromNode: 0, atMs: 0, text: "x", direction: .out, toNode: 1,
                                   outboxID: outboxID), unread: false)
        store.apply(delivery: .sent(outboxID: FireflyMesh.OutboxID(outboxID.rawValue),
                                     packetID: FireflyMesh.PacketID(packetID.rawValue), wantAck: true))

        // A totally different, never-seen packet id — as if the mesh
        // acked somebody else's send, or this device's own long-expired
        // request.
        store.apply(delivery: .delivered(packetID: FireflyMesh.PacketID(packetID.rawValue &+ 999)))

        XCTAssertEqual(store.inbox.thread(.member(1), crew: store.crew, now: 0).first?.sendStatus, .sent,
                       "an ack for an unrelated packetID must not touch this item")
    }

    /// An ack that arrives BEFORE `markSent` ever ran (the item is still
    /// WAITING, `want_ack` still false, no `packet_id` stamped) must be
    /// ignored — `ff_feed_set_ack_by_packet_id`'s own SENT+want_ack gate,
    /// which this bridge must honour rather than bypass (PR #261 review,
    /// finding 1's whole point: the old code wrote `send_status` directly
    /// and skipped this guard entirely).
    func testApplyDeliveryAckBeforeMarkSentIsIgnored() {
        let store = CoreStore()
        store.crew.setPaired(nodeID: 1, paired: true)
        let outboxID = FireflyModel.OutboxID(51)
        let packetID = FireflyModel.PacketID(7_005)
        store.inbox.push(FeedItem(kind: .text, fromNode: 0, atMs: 0, text: "x", direction: .out, toNode: 1,
                                   outboxID: outboxID), unread: false)
        store.apply(delivery: .waiting(outboxID: FireflyMesh.OutboxID(outboxID.rawValue)))

        // No `.sent` ever happened — `packet_id`/`want_ack` were never
        // stamped, so this packetID (even if it's the "right" one a
        // real ack would eventually carry) cannot legitimately resolve
        // anything yet.
        store.apply(delivery: .delivered(packetID: FireflyMesh.PacketID(packetID.rawValue)))

        XCTAssertEqual(store.inbox.thread(.member(1), crew: store.crew, now: 0).first?.sendStatus, .waiting,
                       "an ack before markSent must not jump the item straight to DELIVERED")
    }

    /// The ack-TIMEOUT half of NO_ACK is a tick sweep
    /// (`ff_feed_expire_pending_acks`), not a `DeliveryEvent` case —
    /// `CoreStore.tick(nowMs:)` is the seam that drives it, mirroring
    /// `ff_shell_tick`'s own unconditional per-tick call.
    func testTickExpiresAPendingAckAfterTheTimeout() {
        let store = CoreStore()
        store.crew.setPaired(nodeID: 1, paired: true)
        let outboxID = FireflyModel.OutboxID(61)
        let packetID = FireflyModel.PacketID(8_006)
        store.inbox.push(FeedItem(kind: .text, fromNode: 0, atMs: 0, text: "x", direction: .out, toNode: 1,
                                   outboxID: outboxID), unread: false)

        // `apply(delivery:)` stamps `status_at_ms` off the REAL wall
        // clock (`FireflyClock.nowMillis()`), not a value this test
        // controls — so `tick(nowMs:)` must be driven relative to that
        // same clock, with a margin wide enough to absorb the few ms
        // between reading `base` and `.sent` actually landing.
        let base = FireflyClock.nowMillis()
        store.apply(delivery: .sent(outboxID: FireflyMesh.OutboxID(outboxID.rawValue),
                                     packetID: FireflyMesh.PacketID(packetID.rawValue), wantAck: true))
        XCTAssertEqual(store.inbox.thread(.member(1), crew: store.crew, now: 0).first?.sendStatus, .sent)

        store.tick(nowMs: base + CoreStore.outboxAckTimeoutMs - 5_000)
        XCTAssertEqual(store.inbox.thread(.member(1), crew: store.crew, now: 0).first?.sendStatus, .sent,
                       "must not expire before the timeout has actually elapsed")

        store.tick(nowMs: base + CoreStore.outboxAckTimeoutMs + 5_000)
        XCTAssertEqual(store.inbox.thread(.member(1), crew: store.crew, now: 0).first?.sendStatus, .noAck)
    }
}
