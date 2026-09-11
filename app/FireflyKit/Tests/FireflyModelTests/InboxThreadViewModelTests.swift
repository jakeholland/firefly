//
//  InboxThreadViewModelTests.swift — the delivery-state state machine
//  (including the render-time no-ack window), outbox flush on
//  reconnect, echo dedup by packet.id, quick replies, and FLARE
//  send/receive rendering (docs/specs/A01-companion-app.md, slice E).
//
import FireflyMesh
import FireflyModel
import XCTest

/// A mock `FireflyPacketSending` conformance — the only place slice E
/// touches the FLARE seam (BLOCKING review item 2: this slice defines
/// the seam and a mock, never a real portnum-269 encoding).
private final class MockFlareSender: FireflyPacketSending, @unchecked Sendable {
    private let lock = NSLock()
    private(set) var calls: [(to: NodeID?, durationSeconds: UInt16)] = []
    var shouldThrow = false

    func sendFlare(to: NodeID?, durationSeconds: UInt16) async throws {
        record(to: to, durationSeconds: durationSeconds)
        if shouldThrow { throw TransportError.writeFailed("mock flare failure") }
    }

    // Non-async on purpose — same NSLock-across-a-suspension-point
    // convention as `LoopbackTransport.record(_:)`.
    private func record(to: NodeID?, durationSeconds: UInt16) {
        lock.lock(); defer { lock.unlock() }
        calls.append((to, durationSeconds))
    }
}

/// A client whose `sendText` records call order and holds briefly (long
/// enough to guarantee overlap with a concurrently-tapped send) — the
/// controllable double SHOULD-FIX 5's serialization test needs.
private final class OrderingMockClient: MeshtasticClientProtocol, @unchecked Sendable {
    private let linkHub = EventHub<LinkState>()
    private let deliveryHub = EventHub<DeliveryEvent>()
    private let lock = NSLock()
    private var nextPacketID: UInt32 = 1
    private(set) var sendOrder: [String] = []

    func linkState() -> AsyncStream<LinkState> { linkHub.subscribe() }
    func nodeUpdates() -> AsyncStream<MeshNodeSnapshot> { EventHub<MeshNodeSnapshot>().subscribe() }
    func deliveryUpdates() -> AsyncStream<DeliveryEvent> { deliveryHub.subscribe() }
    func incomingTexts() -> AsyncStream<IncomingText> { EventHub<IncomingText>().subscribe() }
    func incomingPrivate() -> AsyncStream<IncomingPrivate> { EventHub<IncomingPrivate>().subscribe() }

    /// This double exists to order `sendText` calls; it has no handshake
    /// and therefore no node num to report. nil is the honest answer.
    var connectedNodeNum: UInt32? { nil }

    func connect() async throws { linkHub.yield(.ready) }
    func disconnect() async { linkHub.yield(.disconnected) }

    @discardableResult
    func sendText(_ text: String, to destination: UInt32, wantAck: Bool) async throws -> UInt32 {
        let id = record(text)
        // Wide enough that a concurrently-tapped send has ample room to
        // (incorrectly, if unserialized) race ahead of a still-draining
        // flush.
        try? await Task.sleep(nanoseconds: 30_000_000)
        return id
    }

    /// Unused by these tests — this double's whole purpose is
    /// `sendText` ordering — but part of the protocol, so it fails
    /// loudly rather than silently pretending to send.
    @discardableResult
    func sendPosition(_ fix: ExternalPositionFix, to destination: UInt32) async throws -> UInt32 {
        record("position")
    }

    @discardableResult
    func sendPrivate(_ payload: Data, to destination: UInt32, wantAck: Bool) async throws -> UInt32 {
        record("private")
    }

    // Non-async on purpose — NSLock may not be held across a suspension
    // point (the same `LoopbackTransport.record(_:)` convention).
    @discardableResult
    private func record(_ text: String) -> UInt32 {
        lock.lock(); defer { lock.unlock() }
        sendOrder.append(text)
        let id = nextPacketID
        nextPacketID &+= 1
        return id
    }
}

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

    /// BLOCKING review item 1's core scenario: two DIFFERENT senders'
    /// devices independently generated the SAME 32-bit packet id (a
    /// real, non-adversarial birthday-collision risk over a multi-day
    /// festival, not just a hypothetical one) — neither is MY OWN sent
    /// id, so the echo-dedup guard, now scoped to `mySentPacketIDs`
    /// only, must not touch either. A global "every id ever seen" set
    /// would have silently dropped the second one.
    func testTwoDifferentSendersReusingTheSamePacketIDBothKept() {
        let store = InMemoryInboxStore()
        store.registerMember(1, displayName: "One", initial: "O", colorIndex: 0)
        store.registerMember(2, displayName: "Two", initial: "T", colorIndex: 1)
        store.push(FeedMessage(id: 1, kind: .text, direction: .direct, senderID: 1, text: "from one",
                                timestamp: Date(), packetID: 555), into: .member(1))
        store.push(FeedMessage(id: 2, kind: .text, direction: .direct, senderID: 2, text: "from two",
                                timestamp: Date(), packetID: 555), into: .member(2))
        XCTAssertEqual(store.thread(for: .member(1), now: Date()).count, 1,
                        "the message from sender 1 must survive")
        XCTAssertEqual(store.thread(for: .member(2), now: Date()).count, 1,
                        "sender 2's message, reusing the same id sender 1 used, must NOT be silently dropped")
    }

    /// The echo-dedup guard's memory is a BOUNDED ring of my own last 64
    /// sent ids, never an unbounded set — the review's explicit "e.g.
    /// ring of the last 64" ask. Once a 65th of my own sends pushes the
    /// very first one out of the ring, an inbound message that happens
    /// to reuse THAT id (now forgotten) is no longer mistaken for an
    /// echo of mine.
    func testEchoDedupRingIsBoundedToMyLast64Sends() {
        let store = InMemoryInboxStore()
        for i in 0..<65 {
            let outboxID = UInt64(i + 1)
            store.push(FeedMessage(id: outboxID, kind: .text, direction: .out, text: "mine \(i)", timestamp: Date()),
                       into: .crew)
            store.markSent(outboxID: outboxID, packetID: UInt32(i + 1), at: Date())
        }
        let beforeCount = store.thread(for: .crew, now: Date()).count

        // Packet id 1 (my FIRST send) has fallen out of the 64-entry
        // ring — an inbound message reusing it is a stranger's, kept.
        store.push(FeedMessage(id: 9001, kind: .text, direction: .broadcast, text: "not my echo",
                                timestamp: Date(), packetID: 1), into: .crew)
        XCTAssertEqual(store.thread(for: .crew, now: Date()).count, beforeCount + 1,
                        "id 1 fell out of the bounded ring — no longer recognized as my own echo")

        // Packet id 65 (my MOST RECENT send) is still in the ring — an
        // inbound message reusing it is correctly treated as my echo.
        let afterFirstPush = store.thread(for: .crew, now: Date()).count
        store.push(FeedMessage(id: 9002, kind: .text, direction: .broadcast, text: "echo of my newest",
                                timestamp: Date(), packetID: 65), into: .crew)
        XCTAssertEqual(store.thread(for: .crew, now: Date()).count, afterFirstPush,
                        "id 65 is still tracked — a genuine echo of my own recent send, dropped")
    }

    // MARK: - SHOULD-FIX 5: flushOutbox() serialized with user sends

    /// A send tapped while a reconnect flush is still draining must
    /// land AFTER every flushed item, never interleaved with or ahead
    /// of them. `client.sendOrder.count >= 1` is conclusive proof the
    /// flush's single chained unit has already claimed the send chain
    /// (it can only be non-empty once `attemptSend` -> `client.sendText`
    /// has actually been reached from inside that chained unit), so
    /// waiting for it rules out a race in the TEST itself, not just in
    /// the code under test.
    func testSendTappedDuringFlushLandsAfterFlushedItems() async {
        let store = InMemoryInboxStore()
        let client = OrderingMockClient()
        let vm = ThreadViewModel(conversation: .member(5), provider: store, client: client)
        vm.observe() // subscribe before connect — EventHub semantics (SHOULD-FIX 6)

        vm.composeText = "flush-1"
        await vm.sendCompose()
        vm.composeText = "flush-2"
        await vm.sendCompose()
        XCTAssertEqual(vm.queuedCount, 2)

        try? await client.connect() // triggers flushOutbox() on the .ready edge

        // Wait until the flush has genuinely started transmitting.
        await waitUntil { !client.sendOrder.isEmpty }
        XCTAssertFalse(client.sendOrder.isEmpty, "flush must have started before we tap")

        // Tap a fresh compose send WHILE the flush is still mid-flight.
        vm.composeText = "tapped-during-flush"
        await vm.sendCompose()

        XCTAssertEqual(client.sendOrder, ["flush-1", "flush-2", "tapped-during-flush"],
                        "a send tapped during a flush lands after the flushed items")
        vm.stopObserving()
    }

    // MARK: - Quick replies (BLOCKING review item 3: never enter the outbox)

    func testOmwHereWaitSendImmediately() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let vm = ThreadViewModel(conversation: .member(5), provider: store, client: client)
        vm.observe() // subscribe before connect — EventHub semantics (SHOULD-FIX 6)
        try? await client.connect()
        await waitUntil { vm.isLinkReady }

        let omw = ThreadViewModel.quickReplies.first { $0.label == "Omw" }!
        await vm.tap(omw)
        XCTAssertEqual(vm.messages.last?.text, "Omw")
        XCTAssertEqual(vm.messages.last?.direction, .out)
        XCTAssertEqual(vm.queuedCount, 0, "quick replies never enter the bounded outbox")
        XCTAssertNil(vm.immediateSendFailure)
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

    /// S24's 2026-09-07 amendment, verbatim: a link-down tap on a canned
    /// reply "still fails outright... queuing it risks a stale
    /// 'omw'/'5 min' firing minutes later once the link recovers." No
    /// phantom WAITING row, no outbox growth — just a visible failure.
    func testQuickReplyFailsVisiblyWhenDisconnectedAndIsNotQueued() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let vm = ThreadViewModel(conversation: .member(5), provider: store, client: client)
        vm.observe() // link never connects for this whole test

        let here = ThreadViewModel.quickReplies.first { $0.label == "Here" }!
        await vm.tap(here)

        XCTAssertEqual(vm.immediateSendFailure, .linkDown)
        XCTAssertEqual(vm.queuedCount, 0, "must NOT be queued into the outbox")
        XCTAssertTrue(vm.messages.isEmpty, "no phantom row waiting on a flush that will never fire")
        vm.stopObserving()
    }

    // MARK: - FLARE (BLOCKING review items 2 and 3)

    func testFlareSendCarriesItsDurationAndKind() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let flareSender = MockFlareSender()
        let vm = ThreadViewModel(conversation: .crew, provider: store, client: client, flareSender: flareSender)
        vm.observe() // subscribe before connect — EventHub semantics (SHOULD-FIX 6)
        try? await client.connect()
        await waitUntil { vm.isLinkReady }

        await vm.sendFlare(durationSeconds: 120)
        let flare = vm.messages[0]
        XCTAssertEqual(flare.kind, .flare)
        XCTAssertEqual(flare.flareDurationSeconds, 120)
        XCTAssertEqual(flare.direction, .out)
        vm.stopObserving()
    }

    /// BLOCKING review item 2's exact ask: FLARE calls the narrow
    /// `FireflyPacketSending` seam with the right arguments and NEVER
    /// touches `sendText`/the text pipeline.
    func testFlareCallsTheSeamWithRightArgsAndNeverTouchesSendText() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let flareSender = MockFlareSender()
        let vm = ThreadViewModel(conversation: .member(7), provider: store, client: client, flareSender: flareSender)
        vm.observe()
        try? await client.connect()
        await waitUntil { vm.isLinkReady }

        await vm.sendFlare(durationSeconds: 120)

        XCTAssertEqual(flareSender.calls.count, 1)
        XCTAssertEqual(flareSender.calls.first?.to, 7)
        XCTAssertEqual(flareSender.calls.first?.durationSeconds, 120)
        XCTAssertTrue(client.sentTextLog.isEmpty, "FLARE must never touch sendText/the text pipeline")
        vm.stopObserving()
    }

    /// CREW FLARE broadcasts — `to: nil` — with crew filtering left to
    /// happen receiver-side, exactly as S04's "Addressing" rule states;
    /// this seam does not filter.
    func testFlareToCrewPassesNilDestination() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let flareSender = MockFlareSender()
        let vm = ThreadViewModel(conversation: .crew, provider: store, client: client, flareSender: flareSender)
        vm.observe()
        try? await client.connect()
        await waitUntil { vm.isLinkReady }

        await vm.sendFlare(durationSeconds: 300)
        XCTAssertNil(flareSender.calls.first?.to, "CREW flare broadcasts — nil, crew-filtered receiver-side per S04")
        vm.stopObserving()
    }

    /// BLOCKING review item 2's disabled-affordance requirement: with
    /// no `FireflyPacketSending` seam injected — today's default live
    /// wiring — FLARE must be unusable, not a placeholder transmission.
    func testFlareIsDisabledWithoutASeamAndNeverFallsBackToSendText() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let vm = ThreadViewModel(conversation: .crew, provider: store, client: client) // no flareSender
        vm.observe()
        try? await client.connect()
        await waitUntil { vm.isLinkReady }

        XCTAssertFalse(vm.flareAvailable, "the control must render disabled — no seam, no placeholder transmission")
        await vm.sendFlare()
        XCTAssertTrue(vm.messages.isEmpty, "no local record and no send at all when the seam is missing")
        XCTAssertTrue(client.sentTextLog.isEmpty, "must never transmit FLARE as text")
        XCTAssertEqual(vm.immediateSendFailure, .flareUnavailable)
        vm.stopObserving()
    }

    /// BLOCKING review item 3 applied to FLARE specifically: never
    /// queued, fails visibly instead.
    func testFlareFailsVisiblyWhenDisconnectedAndIsNotQueued() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let flareSender = MockFlareSender()
        let vm = ThreadViewModel(conversation: .crew, provider: store, client: client, flareSender: flareSender)
        vm.observe() // never connects

        await vm.sendFlare()
        XCTAssertEqual(vm.immediateSendFailure, .linkDown)
        XCTAssertTrue(flareSender.calls.isEmpty, "must not even attempt the seam call while the link is down")
        XCTAssertTrue(vm.messages.isEmpty)
        vm.stopObserving()
    }

    func testFlareTransportErrorFailsVisiblyAndIsNotQueued() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let flareSender = MockFlareSender()
        flareSender.shouldThrow = true
        let vm = ThreadViewModel(conversation: .crew, provider: store, client: client, flareSender: flareSender)
        vm.observe()
        try? await client.connect()
        await waitUntil { vm.isLinkReady }

        await vm.sendFlare()
        XCTAssertEqual(vm.immediateSendFailure, .transportError)
        XCTAssertEqual(vm.queuedCount, 0, "a genuine transport error still must not queue — FLARE is fire-and-forget")
        vm.stopObserving()
    }

    /// A NO ACK/DROPPED FLARE's RESEND action must go back through the
    /// seam, never through `send(text:kind:)` — the same rule applies
    /// to a retry as to the original tap.
    func testResendOfAFlareGoesThroughTheSeamNotSendText() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let flareSender = MockFlareSender()
        let vm = ThreadViewModel(conversation: .crew, provider: store, client: client, flareSender: flareSender)
        vm.observe()
        try? await client.connect()
        await waitUntil { vm.isLinkReady }

        await vm.sendFlare(durationSeconds: 90)
        let flare = vm.messages[0]
        await vm.resend(flare)

        XCTAssertEqual(flareSender.calls.count, 2, "the original tap plus the resend, both through the seam")
        XCTAssertTrue(client.sentTextLog.isEmpty, "a resend must never fall back to sendText either")
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
