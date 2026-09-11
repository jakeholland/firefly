//
//  InboxThreadViewModelTests.swift — the delivery-state state machine
//  (including the render-time no-ack window), outbox flush on
//  reconnect, echo dedup by packet.id, quick replies, and FLARE
//  send/receive rendering (docs/specs/A01-companion-app.md, slice E).
//
import FireflyMesh
import FireflyModel
import XCTest

@MainActor
final class InboxThreadViewModelTests: XCTestCase {

    private func waitUntil(_ condition: @escaping () -> Bool, timeout: Int = 200) async {
        for _ in 0..<timeout where !condition() {
            try? await Task.sleep(nanoseconds: 5_000_000)
        }
    }

    // MARK: - Delivery-state progression

    func testDirectSendProgressesWaitingSentDelivered() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let vm = ThreadViewModel(conversation: .member(5), provider: store, client: client)
        vm.observe()
        await waitUntil { vm.isLinkReady == false } // starts disconnected
        try? await client.connect()
        await waitUntil { vm.isLinkReady }

        vm.composeText = "on my way"
        await vm.sendCompose()
        await waitUntil { vm.messages.last?.deliveryState == .sent }

        XCTAssertEqual(vm.messages.count, 1)
        let sent = vm.messages[0]
        XCTAssertEqual(sent.direction, .out)
        XCTAssertNotNil(sent.packetID, "SENT must carry the packet id the radio assigned")

        // Simulate the routing ack the way a real client's
        // deliveryUpdates() would deliver it.
        store.setStatus(packetID: sent.packetID!, state: .delivered, at: Date())
        vm.refresh()
        XCTAssertEqual(vm.messages[0].deliveryState, .delivered)

        vm.stopObserving()
    }

    func testBroadcastNeverReachesDelivered() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let vm = ThreadViewModel(conversation: .crew, provider: store, client: client)
        vm.observe()
        try? await client.connect()
        await waitUntil { vm.isLinkReady }

        vm.composeText = "omw crew"
        await vm.sendCompose()
        await waitUntil { vm.messages.last?.deliveryState == .sent }

        // Terminal for a broadcast: SENT is the whole story.
        XCTAssertEqual(vm.renderedDeliveryState(for: vm.messages[0]), .sent)
        // Even a long time later, a broadcast must not render NO ACK —
        // the render-time window only applies to a DIRECT want_ack send.
        let farFuture = Date().addingTimeInterval(ThreadViewModel.noAckRenderWindow + 3600)
        XCTAssertEqual(vm.renderedDeliveryState(for: vm.messages[0], now: farFuture), .sent)

        vm.stopObserving()
    }

    func testWaitingRendersAsNodeNotConnectedWhileLinkIsDown() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let vm = ThreadViewModel(conversation: .member(5), provider: store, client: client)
        // Never connected: isLinkReady stays false.
        vm.composeText = "hello"
        await vm.sendCompose()
        XCTAssertEqual(vm.messages.last?.deliveryState, .waiting)
        XCTAssertFalse(vm.isLinkReady)
        XCTAssertEqual(vm.queuedCount, 1, "queued in the outbox because the transport is down")
    }

    /// The render-time no-ack window (A01: "5 minutes elapsed... derived
    /// at render time... not driven by a timer"): a SENT direct message
    /// renders NO ACK once enough time has passed, WITHOUT anything ever
    /// mutating the stored state.
    func testNoAckWindowIsAppliedAtRenderTimeNotByMutatingStoredState() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let vm = ThreadViewModel(conversation: .member(5), provider: store, client: client)
        vm.observe()
        try? await client.connect()
        await waitUntil { vm.isLinkReady }

        vm.composeText = "ping"
        await vm.sendCompose()
        await waitUntil { vm.messages.last?.deliveryState == .sent }
        let message = vm.messages[0]

        // Just under the window: still SENT.
        let almost = message.statusAt.addingTimeInterval(ThreadViewModel.noAckRenderWindow - 1)
        XCTAssertEqual(vm.renderedDeliveryState(for: message, now: almost), .sent)

        // At/after the window: renders NO ACK...
        let after = message.statusAt.addingTimeInterval(ThreadViewModel.noAckRenderWindow)
        XCTAssertEqual(vm.renderedDeliveryState(for: message, now: after), .noAck)

        // ...but the STORED state never changed — a later real ack must
        // still be able to land honestly.
        XCTAssertEqual(vm.messages[0].deliveryState, .sent,
                        "the render-time window must not mutate the stored delivery state")

        vm.stopObserving()
    }

    func testNoAckHasAResendActionThatRetriesTheSameContent() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let vm = ThreadViewModel(conversation: .member(5), provider: store, client: client)
        vm.observe()
        try? await client.connect()
        await waitUntil { vm.isLinkReady }

        vm.composeText = "still there?"
        await vm.sendCompose()
        await waitUntil { vm.messages.last?.deliveryState == .sent }
        let original = vm.messages[0]

        await vm.resend(original)
        await waitUntil { vm.messages.count == 2 }
        XCTAssertEqual(vm.messages.last?.text, original.text)
        XCTAssertEqual(vm.messages.last?.direction, .out)

        vm.stopObserving()
    }

    // MARK: - Outbox: queue while down, flush on reconnect, bounded cap

    func testOutboxFlushesOnReconnect() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let vm = ThreadViewModel(conversation: .member(5), provider: store, client: client)
        vm.observe() // link starts disconnected

        vm.composeText = "queued while down"
        await vm.sendCompose()
        XCTAssertEqual(vm.queuedCount, 1)
        XCTAssertEqual(vm.messages.last?.deliveryState, .waiting)

        try? await client.connect()
        await waitUntil { vm.queuedCount == 0 }
        await waitUntil { vm.messages.last?.deliveryState == .sent }

        XCTAssertEqual(vm.queuedCount, 0, "flushed automatically on the not-ready -> ready edge")
        XCTAssertNotNil(vm.messages.last?.packetID)

        vm.stopObserving()
    }

    func testOutboxCapDropsOldestAndMarksItDropped() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let vm = ThreadViewModel(conversation: .member(5), provider: store, client: client)
        // Stays disconnected for this whole test — everything queues.
        for i in 0..<(ThreadViewModel.outboxCap + 1) {
            vm.composeText = "msg \(i)"
            await vm.sendCompose()
        }
        XCTAssertEqual(vm.queuedCount, ThreadViewModel.outboxCap, "bounded — never grows past the cap")
        XCTAssertEqual(vm.messages.count, ThreadViewModel.outboxCap + 1, "every attempt still shows up in the thread")
        XCTAssertEqual(vm.messages.first?.text, "msg 0")
        XCTAssertEqual(vm.messages.first?.deliveryState, .dropped,
                        "the evicted item's OWN row flips to DROPPED — visible, never silent")
        XCTAssertEqual(vm.messages.last?.deliveryState, .waiting)
    }

    // MARK: - Echo dedup by packet.id

    func testInboundEchoOfOwnSentPacketIsDroppedNotDuplicated() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let vm = ThreadViewModel(conversation: .crew, provider: store, client: client)
        vm.observe()
        try? await client.connect()
        await waitUntil { vm.isLinkReady }

        vm.composeText = "omw"
        await vm.sendCompose()
        await waitUntil { vm.messages.last?.deliveryState == .sent }
        XCTAssertEqual(vm.messages.count, 1)
        let sentPacketID = vm.messages[0].packetID!

        // The mesh echoes the same packet back as if it were inbound.
        store.push(FeedMessage(id: 999, kind: .text, direction: .broadcast, senderID: nil, text: "omw",
                                timestamp: Date(), packetID: sentPacketID), into: .crew)
        vm.refresh()

        XCTAssertEqual(vm.messages.count, 1, "the echo must not create a second row")
        XCTAssertEqual(vm.messages[0].direction, .out, "and must not overwrite the original OUT row's direction")
        XCTAssertEqual(vm.messages[0].deliveryState, .sent)

        vm.stopObserving()
    }

    func testTwoDistinctInboundMessagesWithDifferentPacketIDsBothAppear() {
        let store = InMemoryInboxStore()
        store.push(FeedMessage(id: 1, kind: .text, direction: .broadcast, text: "a", timestamp: Date(),
                                packetID: 100), into: .crew)
        store.push(FeedMessage(id: 2, kind: .text, direction: .broadcast, text: "b", timestamp: Date(),
                                packetID: 101), into: .crew)
        XCTAssertEqual(store.thread(for: .crew, now: Date()).count, 2)
    }

    // MARK: - Quick replies

    func testOmwHereWaitSendImmediately() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let vm = ThreadViewModel(conversation: .member(5), provider: store, client: client)
        try? await client.connect()
        await waitUntil { vm.isLinkReady }
        vm.observe()

        let omw = ThreadViewModel.quickReplies.first { $0.label == "Omw" }!
        await vm.tap(omw)
        await waitUntil { !vm.messages.isEmpty }
        XCTAssertEqual(vm.messages.last?.text, "Omw")
        XCTAssertEqual(vm.messages.last?.direction, .out)
        vm.stopObserving()
    }

    func testMeetAtSeedsComposeTextInsteadOfSending() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let vm = ThreadViewModel(conversation: .member(5), provider: store, client: client)
        let meetAt = ThreadViewModel.quickReplies.first { $0.label == "Meet at…" }!
        await vm.tap(meetAt)
        XCTAssertEqual(vm.composeText, "Meet at ")
        XCTAssertTrue(vm.messages.isEmpty, "must not send anything by itself")
    }

    // MARK: - FLARE

    func testFlareSendCarriesItsDurationAndKind() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let vm = ThreadViewModel(conversation: .crew, provider: store, client: client)
        try? await client.connect()
        await waitUntil { vm.isLinkReady }
        vm.observe()

        await vm.sendFlare(durationSeconds: 120)
        await waitUntil { !vm.messages.isEmpty }
        let flare = vm.messages[0]
        XCTAssertEqual(flare.kind, .flare)
        XCTAssertEqual(flare.flareDurationSeconds, 120)
        XCTAssertEqual(flare.direction, .out)
        vm.stopObserving()
    }

    func testInboundFlareRendersDistinctlyFromText() {
        let store = InMemoryInboxStore()
        store.push(FeedMessage(id: 1, kind: .flare, direction: .broadcast, senderID: 4, senderName: "Sam",
                                text: "FLARE", timestamp: Date(), flareDurationSeconds: 300), into: .crew)
        let thread = store.thread(for: .crew, now: Date())
        XCTAssertEqual(thread.count, 1)
        XCTAssertEqual(thread[0].kind, .flare)
        XCTAssertEqual(thread[0].flareDurationSeconds, 300)
        XCTAssertEqual(thread[0].senderName, "Sam")
    }
}
