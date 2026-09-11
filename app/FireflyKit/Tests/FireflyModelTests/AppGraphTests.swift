//
//  AppGraphTests.swift — the composition root, exercised against mocks:
//  the live graph constructs, each client stream is subscribed exactly
//  once by the graph itself, portnum-269 traffic reaches the right
//  consumer, and the phone-GPS uplink stops pushing the moment the
//  "share phone GPS" setting goes off.
//
//  No radio, no CoreBluetooth, no CoreLocation: `CountingClient` below
//  is an honest double — it fabricates no nodes, no positions and no
//  acks, exactly like `StubMeshtasticClient`, and additionally COUNTS
//  subscriptions so "subscribed once each" is a mechanical assertion
//  rather than a code reading.
//
import FireflyMesh
import FireflyModel
import MeshtasticProto
import XCTest

/// Counts `subscribe()` calls per stream, and records everything sent.
/// Invents nothing on its own: every event a test sees came from that
/// test's own `yield*` call.
private final class CountingClient: MeshtasticClientProtocol, @unchecked Sendable {
    private let linkHub = EventHub<LinkState>()
    private let nodeHub = EventHub<MeshNodeSnapshot>()
    private let deliveryHub = EventHub<DeliveryEvent>()
    private let textHub = EventHub<IncomingText>()
    private let privateHub = EventHub<IncomingPrivate>()
    private let lock = NSLock()

    private var counts: [String: Int] = [:]
    private var positions: [(ExternalPositionFix, UInt32)] = []
    private var privates: [(Data, UInt32, Bool)] = []
    private var texts: [(String, UInt32, Bool)] = []
    private var _connectedNodeNum: UInt32?
    // M2: `AppGraph.handleScenePhaseChange`/`autoConnectToLastKnownPeripheral`
    // now call `connect()`/`disconnect()` on their own — these count
    // exactly those calls so a test can assert "off backgrounds
    // disconnect", "on backgrounds do not", and "launch auto-connects
    // exactly once" mechanically rather than by code reading.
    private var _connectCallCount = 0
    private var _disconnectCallCount = 0

    var connectCallCount: Int {
        lock.lock(); defer { lock.unlock() }; return _connectCallCount
    }
    var disconnectCallCount: Int {
        lock.lock(); defer { lock.unlock() }; return _disconnectCallCount
    }

    func subscriptionCount(_ stream: String) -> Int {
        lock.lock(); defer { lock.unlock() }
        return counts[stream] ?? 0
    }

    var sentPositions: [(ExternalPositionFix, UInt32)] {
        lock.lock(); defer { lock.unlock() }
        return positions
    }

    var sentPrivate: [(Data, UInt32, Bool)] {
        lock.lock(); defer { lock.unlock() }
        return privates
    }

    var sentTexts: [(String, UInt32, Bool)] {
        lock.lock(); defer { lock.unlock() }
        return texts
    }

    private func bump(_ stream: String) {
        lock.lock(); counts[stream, default: 0] += 1; lock.unlock()
    }

    func linkState() -> AsyncStream<LinkState> { bump("link"); return linkHub.subscribe() }
    func nodeUpdates() -> AsyncStream<MeshNodeSnapshot> { bump("node"); return nodeHub.subscribe() }
    func deliveryUpdates() -> AsyncStream<DeliveryEvent> { bump("delivery"); return deliveryHub.subscribe() }
    func incomingTexts() -> AsyncStream<IncomingText> { bump("text"); return textHub.subscribe() }
    func incomingPrivate() -> AsyncStream<IncomingPrivate> { bump("private"); return privateHub.subscribe() }

    var connectedNodeNum: UInt32? {
        get { lock.lock(); defer { lock.unlock() }; return _connectedNodeNum }
        set { lock.lock(); defer { lock.unlock() }; _connectedNodeNum = newValue }
    }

    func yieldLink(_ state: LinkState) { linkHub.yield(state) }
    func yieldNode(_ snapshot: MeshNodeSnapshot) { nodeHub.yield(snapshot) }
    func yieldPrivate(_ packet: IncomingPrivate) { privateHub.yield(packet) }
    /// `InboxViewModel.observe()`'s own `incomingTexts()` subscription —
    /// distinct from `yieldPrivate`/`yieldNode` above, and from the
    /// graph's own `observeIncomingTextsForNotifications()` subscription
    /// to the same stream (S1's multicast rule: every subscriber sees
    /// every yield here, exactly like a real client).
    func yieldText(_ text: IncomingText) { textHub.yield(text) }

    func connect() async throws {
        recordConnect()
        linkHub.yield(.ready)
    }
    func disconnect() async {
        recordDisconnect()
        linkHub.yield(.disconnected)
    }

    @discardableResult
    func sendText(_ text: String, to destination: UInt32, wantAck: Bool) async throws -> UInt32 {
        recordText(text, destination: destination, wantAck: wantAck)
        return 1
    }

    @discardableResult
    func sendPosition(_ fix: ExternalPositionFix, to destination: UInt32) async throws -> UInt32 {
        recordPosition(fix, destination: destination)
        return 2
    }

    @discardableResult
    func sendPrivate(_ payload: Data, to destination: UInt32, wantAck: Bool) async throws -> UInt32 {
        recordPrivate(payload, destination: destination, wantAck: wantAck)
        return 3
    }

    @discardableResult
    func applyChannelSet(_ request: ChannelWriteRequest) async throws -> ChannelWriteReport {
        ChannelWriteReport(channels: request.channels, loraConfig: request.loraConfig)
    }
    @discardableResult
    func setOwner(longName: String, shortName: String) async throws -> OwnerWriteReport {
        OwnerWriteReport(longName: longName, shortName: shortName)
    }
    @discardableResult
    func setRegion(_ region: Config.LoRaConfig.RegionCode) async throws -> RegionWriteReport {
        RegionWriteReport(region: region)
    }
    func currentChannelTable() async throws -> [Channel] { [] }

    // M3 / Swift 6: every locked mutation above happens in one of these
    // synchronous helpers, never lexically inside an `async` function
    // body — `NSLock.lock()`/`unlock()` are `noasync`, the same rule
    // `DemoMeshtasticClient`'s own record helpers document.
    private func recordConnect() { lock.lock(); _connectCallCount += 1; lock.unlock() }
    private func recordDisconnect() { lock.lock(); _disconnectCallCount += 1; lock.unlock() }
    private func recordText(_ text: String, destination: UInt32, wantAck: Bool) {
        lock.lock(); texts.append((text, destination, wantAck)); lock.unlock()
    }
    private func recordPosition(_ fix: ExternalPositionFix, destination: UInt32) {
        lock.lock(); positions.append((fix, destination)); lock.unlock()
    }
    private func recordPrivate(_ payload: Data, destination: UInt32, wantAck: Bool) {
        lock.lock(); privates.append((payload, destination, wantAck)); lock.unlock()
    }
}

/// A location provider a test drives by hand — it yields exactly the
/// fixes the test pushes and nothing else.
private final class ScriptedLocationProvider: LocationProviding, @unchecked Sendable {
    private let hub = EventHub<LocationFix?>()
    let authorization: LocationAuthorization = .whenInUse
    func requestWhenInUseAuthorization() async {}
    func requestAlwaysAuthorization() async {}
    func fixes() -> AsyncStream<LocationFix?> { hub.subscribe() }
    func push(_ fix: LocationFix?) { hub.yield(fix) }
}

/// An honest `NotificationSending` double: posts nothing, records every
/// call so a test can assert exactly what would have gone out — same
/// "record, never actually deliver" convention `MockFlareSender`
/// (`InboxThreadViewModelTests.swift`) already uses for the FLARE seam.
/// A plain class + `NSLock`, not an actor: `waitUntil` (this file's own
/// helper) polls a synchronous, non-`async` predicate, which an actor's
/// isolated properties cannot satisfy without their own `await` — same
/// `NSLock`-across-a-suspension-point convention `LoopbackTransport
/// .record(_:)`/`MockFlareSender.record(to:durationSeconds:)` already
/// use in this codebase for an identical reason.
private final class RecordingNotificationSending: NotificationSending, @unchecked Sendable {
    private let lock = NSLock()
    private var _flareCalls: [String] = []
    private var _messageCalls: [(senderName: String, preview: String)] = []

    var flareCalls: [String] { lock.lock(); defer { lock.unlock() }; return _flareCalls }
    var messageCalls: [(senderName: String, preview: String)] { lock.lock(); defer { lock.unlock() }; return _messageCalls }

    func postFlare(senderName: String) async { record { self._flareCalls.append(senderName) } }
    func postMessage(senderName: String, preview: String) async {
        record { self._messageCalls.append((senderName, preview)) }
    }

    private func record(_ body: () -> Void) {
        lock.lock(); defer { lock.unlock() }
        body()
    }
}

/// An honest `HapticSignaling` double for the negative "unpaired sender
/// gets no haptic" tests (PR #271 review, BLOCKING finding 1) — same
/// "record, never actually vibrate" convention `RecordingNotificationSending`
/// just above already uses.
private final class RecordingHapticSignaling: HapticSignaling, @unchecked Sendable {
    private let lock = NSLock()
    private var _flareAlertCount = 0

    var flareAlertCount: Int { lock.lock(); defer { lock.unlock() }; return _flareAlertCount }

    func warmer() {}
    func colder() {}
    func flareAlert() { lock.lock(); _flareAlertCount += 1; lock.unlock() }
}

@MainActor
final class AppGraphTests: XCTestCase {

    // 200 * 5ms = 1s worst-case ceiling — matches the package-wide "no
    // test may sleep more than ~1s total" rule
    // (`ClientReconnectTests.waitForSentCount`'s own doc comment). Every
    // client this file drives is `CountingClient`, a plain test double
    // with no real backoff/timeout of its own — this loop only ever
    // outlasts its typical handful of 5ms polls when a test is
    // genuinely broken, so tightening the ceiling only speeds up that
    // failure, it does not risk a false one.
    private func waitUntil(_ condition: @escaping () -> Bool, timeout: Int = 200) async {
        for _ in 0..<timeout where !condition() {
            try? await Task.sleep(nanoseconds: 5_000_000)
        }
    }

    private func dependencies(client: CountingClient,
                              location: any LocationProviding = UnavailableLocationProvider(),
                              store: any FireflyExtraSettingsStoring = InMemorySettingsStore()) -> AppDependencies {
        AppDependencies(client: client, location: location, heading: NoHeadingProvider(), store: store)
    }

    // MARK: - The graph constructs and subscribes exactly once

    func testLiveGraphConstructsAndBuildsEveryViewModel() {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))

        // Every screen's view model comes from the ONE graph — nothing
        // reaches for `AppDependencies.current()` on its own any more.
        _ = graph.makeConnectViewModel()
        _ = graph.makeInboxViewModel()
        _ = graph.makeRadarViewModel()

        // The inbox provider is the C-core one, not the in-memory
        // stand-in: CREW always exists, with no traffic in it.
        let rows = graph.inboxProvider.conversations(now: Date())
        XCTAssertEqual(rows.map(\.kind), [.crew])
        XCTAssertEqual(rows.first?.itemCount, 0, "a fresh graph invents no traffic")
        XCTAssertFalse(rows.first?.hasPreview ?? true)
    }

    func testStartSubscribesEachClientStreamExactlyOnceAndIsIdempotent() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))

        await graph.start()
        await graph.start() // idempotent, like every observe() in this app
        await graph.start()

        // M3: the graph now holds a SECOND, independent `linkState()`
        // subscription too — `observeHistoryOutboxFlush()`, which
        // flushes any persisted-WAITING outbox items on the link's next
        // not-ready -> ready edge. Same S1 multicast rule as `"text"`
        // above: this is its OWN subscription, not a second reader
        // stealing `core.observe(client:)`'s.
        XCTAssertEqual(client.subscriptionCount("link"), 2)
        XCTAssertEqual(client.subscriptionCount("node"), 1)
        XCTAssertEqual(client.subscriptionCount("delivery"), 1)
        XCTAssertEqual(client.subscriptionCount("private"), 1)
        // M2: the graph now holds its OWN `incomingTexts()` subscription
        // too — a second, independent one (S1's multicast rule), purely
        // to notice a text arriving while the app is backgrounded and
        // post a local notification
        // (`observeIncomingTextsForNotifications()`). `InboxViewModel`'s
        // own subscription (below) is unaffected and unrelated — it is
        // ITS OWN, separately-started subscription
        // (`makeInboxViewModel()`'s own doc comment: process-lifetime
        // now, started once by the composition root, same as this one,
        // rather than tied to `InboxContainerView`'s appear/disappear)
        // — so the graph needs one of its own rather than reusing (or
        // stealing) the view model's.
        XCTAssertEqual(client.subscriptionCount("text"), 1)

        await graph.stop()
    }

    func testViewModelsHoldTheirOwnIndependentSubscriptions() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))
        await graph.start()

        // `makeInboxViewModel()` already started `observe()` itself
        // (`AppGraph`'s own doc comment on that method) — this second
        // call is deliberately redundant, exercising the idempotent
        // guard `observe()` follows everywhere in this app, same as
        // `testStartSubscribesEachClientStreamExactlyOnceAndIsIdempotent`
        // does for `graph.start()` above.
        let inbox = graph.makeInboxViewModel()
        inbox.observe()

        // S1's multicast rule: the Inbox's subscriptions are ITS own,
        // never stolen from or shared with the graph's. "2" here is the
        // graph's own M2 notification subscription (started by
        // `graph.start()` above) plus the Inbox's own (started by
        // `makeInboxViewModel()`, not by the redundant call just above).
        XCTAssertEqual(client.subscriptionCount("text"), 2)
        XCTAssertEqual(client.subscriptionCount("delivery"), 2, "graph's + the Inbox's own")

        inbox.stopObserving()
        await graph.stop()
    }

    // MARK: - Node updates reach the C core

    func testNodeUpdateReachesTheCrewRosterWithItsRealIdentity() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))
        await graph.start()

        client.yieldNode(MeshNodeSnapshot(
            num: 0x02E6_06B0, shortName: "FF1", longName: "Firefly 1",
            position: NodePosition(latitude: 47.708135, longitude: -122.2820993, time: Date(),
                                    source: .manual, precisionBits: 32),
            lastHeard: Date(), rssiDbm: -61, snrDb: 6.5, hopsAway: 0))

        await waitUntil { graph.core.crew.count == 1 }
        let member = graph.core.crew.member(nodeID: 0x02E6_06B0, now: FireflyClock.nowMillis())
        XCTAssertEqual(member?.shortName, "FF1")
        XCTAssertEqual(member?.longName, "Firefly 1")
        XCTAssertEqual(member?.initial, "F")
        XCTAssertNotNil(member?.position)
        XCTAssertEqual(member?.position?.asserted, true,
                        "LOC_MANUAL is asserted, not measured — freshness is a category error for it")
        XCTAssertEqual(member?.directSignal?.rssiDbm, -61)

        await graph.stop()
    }

    func testNodeUpdateWithNoNamesLeavesIdentityEmptyRatherThanInventingOne() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))
        await graph.start()

        client.yieldNode(MeshNodeSnapshot(num: 99, shortName: nil, longName: nil, position: nil,
                                           lastHeard: Date(), rssiDbm: nil, snrDb: nil, hopsAway: nil))

        await waitUntil { graph.core.crew.count == 1 }
        let member = graph.core.crew.member(nodeID: 99, now: FireflyClock.nowMillis())
        XCTAssertEqual(member?.shortName, "")
        XCTAssertEqual(member?.longName, "")
        XCTAssertNil(member?.initial, "'\\0' until known — never a '?' placeholder")
        XCTAssertNil(member?.position)
        XCTAssertNil(member?.directSignal, "no RSSI reported means no RSSI rendered")

        await graph.stop()
    }

    // MARK: - Portnum 269

    func testInboundPongReachesTheRadarViewModelWithTheirRSSINotOurs() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))
        await graph.start()
        let radar = graph.makeRadarViewModel()

        let payload = FireflyPacket.pong(nonce: 4242, rssiDbm: -77, snrDb: 5.5).encode()
        XCTAssertNotNil(payload)
        client.yieldPrivate(IncomingPrivate(
            from: 0x02E6_06B0, to: 48_621_524, channel: 0, packetID: 7, payload: payload!,
            // Deliberately DIFFERENT from the body's -77: this is how WE
            // heard THEM, and it must never be what FIND reports.
            rxTime: Date(), rssiDbm: -30, snrDb: 9, direct: true))

        await waitUntil { !radar.findReplies.isEmpty }
        XCTAssertEqual(radar.findReplies.first?.rssiOfUs, -77,
                        "FIND reports how THEY hear US — the PONG body's own reading, not the packet's rx RSSI")
        XCTAssertEqual(radar.findReplies.first?.hasSNR, true)

        await graph.stop()
    }

    func testMalformedPrivatePacketIsDroppedNotRendered() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))
        await graph.start()
        let radar = graph.makeRadarViewModel()

        client.yieldPrivate(IncomingPrivate(
            from: 1, to: 2, channel: 0, packetID: 9, payload: Data([0xFF, 0xFE, 0xFD]),
            rxTime: Date(), rssiDbm: -30, snrDb: nil, direct: true))

        try? await Task.sleep(nanoseconds: 100_000_000)
        XCTAssertTrue(radar.findReplies.isEmpty, "a frame ff_proto rejects renders as nothing at all")

        await graph.stop()
    }

    func testFlareGoesOutOnPortnum269AndNeverAsText() async throws {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))

        try await graph.packetSender.sendFlare(to: 0x02E6_06B0, durationSeconds: 300)

        XCTAssertTrue(client.sentTexts.isEmpty,
                       "a FLARE sent as plain text is worse than failing honestly (BLOCKING review item 2)")
        XCTAssertEqual(client.sentPrivate.count, 1)
        let (payload, destination, wantAck) = try XCTUnwrap(client.sentPrivate.first)
        XCTAssertEqual(destination, 0x02E6_06B0)
        XCTAssertTrue(wantAck, "S04: FLARE is want_ack")
        XCTAssertEqual(FireflyPacket.decode(payload), .flare(durationS: 300),
                        "the bytes on the wire must round-trip through ff_proto")
    }

    // MARK: - Phone GPS uplink

    private func fix(latitude: Double, longitude: Double, time: Date = Date()) -> LocationFix {
        LocationFix(latitude: latitude, longitude: longitude, altitude: 42, time: time,
                     horizontalAccuracyMeters: 12, groundSpeedMetersPerSecond: nil, groundTrackDegrees: nil)
    }

    func testGPSUplinkPushesOnlyWhileSharingIsOnAndStopsWhenItIsTurnedOff() async throws {
        let client = CountingClient()
        client.connectedNodeNum = 48_621_524
        let location = ScriptedLocationProvider()
        let store = InMemorySettingsStore()
        store.setBool(true, .locationSharingEnabled)
        let graph = AppGraph(dependencies: dependencies(client: client, location: location, store: store))
        await graph.start()

        location.push(fix(latitude: 47.7, longitude: -122.28))
        await waitUntil { client.sentPositions.count == 1 }
        let pushed = try XCTUnwrap(client.sentPositions.first)
        XCTAssertEqual(client.sentPositions.count, 1)
        XCTAssertEqual(pushed.1, 48_621_524, "a phone fix goes to the CONNECTED node itself")
        XCTAssertEqual(pushed.0.latitude, 47.7, accuracy: 1e-9)

        // The user turns sharing off. The uplink keeps running — it is
        // the per-fix gate that must stop, not the subscription (see
        // `AppGraph.start()`'s own comment) — and nothing further goes
        // out no matter how many fixes arrive.
        store.setBool(false, .locationSharingEnabled)
        // Far enough to clear the movement trigger as well as the timer,
        // so a second push could only be a genuine gate failure.
        location.push(fix(latitude: 47.9, longitude: -122.48))
        location.push(fix(latitude: 48.1, longitude: -122.68))
        try? await Task.sleep(nanoseconds: 300_000_000)
        XCTAssertEqual(client.sentPositions.count, 1,
                        "location sharing off must mean nothing on the wire, immediately")

        await graph.stop()
    }

    func testGPSUplinkSendsNothingBeforeTheHandshakeHasANodeToAddress() async {
        let client = CountingClient() // connectedNodeNum stays nil
        let location = ScriptedLocationProvider()
        let store = InMemorySettingsStore()
        store.setBool(true, .locationSharingEnabled)
        let graph = AppGraph(dependencies: dependencies(client: client, location: location, store: store))
        await graph.start()

        location.push(fix(latitude: 47.7, longitude: -122.28))
        try? await Task.sleep(nanoseconds: 300_000_000)
        XCTAssertTrue(client.sentPositions.isEmpty,
                       "with no my_info yet there is no node to address — never guess one")

        await graph.stop()
    }

    func testNoFixPushesNothing() async {
        let client = CountingClient()
        client.connectedNodeNum = 48_621_524
        let location = ScriptedLocationProvider()
        let store = InMemorySettingsStore()
        store.setBool(true, .locationSharingEnabled)
        let graph = AppGraph(dependencies: dependencies(client: client, location: location, store: store))
        await graph.start()

        location.push(nil) // permission denied / no signal: never fabricate
        try? await Task.sleep(nanoseconds: 200_000_000)
        XCTAssertTrue(client.sentPositions.isEmpty)

        await graph.stop()
    }

    // MARK: - The stub stack stays honest under the live graph

    func testStubGraphShowsNothingThatDidNotComeFromAnInjectedByte() async {
        // A01_AC8, at the composition-root level: the graph the iOS
        // Simulator gets must render an empty everything.
        let graph = AppGraph(dependencies: .stub())
        await graph.start()
        let radar = graph.makeRadarViewModel()
        radar.observe()

        try? await Task.sleep(nanoseconds: 100_000_000)
        XCTAssertEqual(radar.snapshot.mode, .noSel)
        XCTAssertTrue(radar.snapshot.dots.isEmpty)
        XCTAssertTrue(radar.snapshot.signalDots.isEmpty)
        XCTAssertEqual(graph.core.crew.count, 0)
        XCTAssertEqual(graph.inboxProvider.conversations(now: Date()).count, 1, "CREW only, and empty")

        radar.stopObserving()
        await graph.stop()
    }

    func testStubStackHasNoScannerSoThePickerCannotInventAPeripheral() {
        XCTAssertNil(AppDependencies.stub().scanner,
                      "no radio in the Simulator — an empty picker is the honest answer")
    }


    // MARK: - M2: PONG auto-reply (S29 PR 2)

    /// A received PING gets exactly one PONG, direct-addressed, carrying
    /// OUR OWN measured RSSI/SNR on that packet — never the RSSI it
    /// claims to be replying about.
    func testPingReceivesExactlyOnePongWithOurMeasuredRSSIAndSNR() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))
        await graph.start()

        let pingPayload = FireflyPacket.ping(nonce: 0xABCD_1234).encode()
        XCTAssertNotNil(pingPayload)
        client.yieldPrivate(IncomingPrivate(
            from: 0x02E6_06B0, to: 48_621_524, channel: 0, packetID: 55, payload: pingPayload!,
            rxTime: Date(), rssiDbm: -63, snrDb: 4.5, direct: true))

        await waitUntil { !client.sentPrivate.isEmpty }
        XCTAssertEqual(client.sentPrivate.count, 1)
        let (payload, destination, wantAck) = try! XCTUnwrap(client.sentPrivate.first)
        XCTAssertEqual(destination, 0x02E6_06B0, "the reply goes back to whoever pinged us, direct-addressed")
        XCTAssertFalse(wantAck, "S29: PONG carries no ack request")
        XCTAssertEqual(FireflyPacket.decode(payload), .pong(nonce: 0xABCD_1234, rssiDbm: -63, snrDb: 4.5),
                        "the reply must echo the nonce and carry OUR OWN measured reading of the PING")

        await graph.stop()
    }

    /// A PING packet with no honest RSSI reading of our own gets no
    /// reply at all — `ff_proto.h`'s own rule (`ff_proto_pong_t
    /// .rssi_dbm`'s doc comment): nothing honest to report, so nothing
    /// is sent.
    func testPingWithNoRSSIReadingGetsNoReply() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))
        await graph.start()

        let pingPayload = FireflyPacket.ping(nonce: 9).encode()!
        client.yieldPrivate(IncomingPrivate(
            from: 0x02E6_06B0, to: 48_621_524, channel: 0, packetID: 56, payload: pingPayload,
            rxTime: Date(), rssiDbm: nil, snrDb: nil, direct: true))

        try? await Task.sleep(nanoseconds: 150_000_000)
        XCTAssertTrue(client.sentPrivate.isEmpty)

        await graph.stop()
    }

    /// The task's own explicit ask: rate-limited to ONE reply per nonce
    /// — a redelivered/duplicated PING packet must not double-reply.
    func testDuplicatePingNonceFromTheSameSenderOnlyRepliesOnce() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))
        await graph.start()

        let pingPayload = FireflyPacket.ping(nonce: 42).encode()!
        client.yieldPrivate(IncomingPrivate(
            from: 0x02E6_06B0, to: 48_621_524, channel: 0, packetID: 60, payload: pingPayload,
            rxTime: Date(), rssiDbm: -70, snrDb: nil, direct: true))
        await waitUntil { client.sentPrivate.count == 1 }
        client.yieldPrivate(IncomingPrivate(
            from: 0x02E6_06B0, to: 48_621_524, channel: 0, packetID: 61, payload: pingPayload,
            rxTime: Date(), rssiDbm: -71, snrDb: nil, direct: true))
        try? await Task.sleep(nanoseconds: 150_000_000)

        XCTAssertEqual(client.sentPrivate.count, 1, "same (from, nonce) pair — only the first PING gets a reply")

        await graph.stop()
    }

    /// A DIFFERENT nonce from the same sender is a new probe and gets
    /// its own reply — the dedup key is the (from, nonce) PAIR, not the
    /// sender alone.
    func testDifferentNonceFromSameSenderGetsItsOwnReply() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))
        await graph.start()

        client.yieldPrivate(IncomingPrivate(
            from: 0x02E6_06B0, to: 48_621_524, channel: 0, packetID: 70,
            payload: FireflyPacket.ping(nonce: 1).encode()!, rxTime: Date(), rssiDbm: -70, snrDb: nil, direct: true))
        await waitUntil { client.sentPrivate.count == 1 }
        client.yieldPrivate(IncomingPrivate(
            from: 0x02E6_06B0, to: 48_621_524, channel: 0, packetID: 71,
            payload: FireflyPacket.ping(nonce: 2).encode()!, rxTime: Date(), rssiDbm: -70, snrDb: nil, direct: true))
        await waitUntil { client.sentPrivate.count == 2 }

        XCTAssertEqual(client.sentPrivate.count, 2)

        await graph.stop()
    }

    // MARK: - M2: inbound FLARE (S10)

    /// Every one of these senders must be registered PAIRED first (PR
    /// #271 review, BLOCKING finding 1) — before this fix, these tests
    /// themselves demonstrated the gap: they asserted a reaction for a
    /// `from` node id that was never paired, let alone identified.
    func testInboundFlareShowsTheTakeoverWhileForegrounded() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))
        graph.core.crew.setPaired(nodeID: 0x0000_1002, paired: true)
        await graph.start()
        graph.setForegrounded(true)

        client.yieldPrivate(IncomingPrivate(
            from: 0x0000_1002, to: meshBroadcastAddress, channel: 0, packetID: 80,
            payload: FireflyPacket.flare(durationS: 120).encode()!, rxTime: Date(), rssiDbm: -60, snrDb: nil,
            direct: true))

        await waitUntil { graph.flareTakeover.isActive }
        XCTAssertEqual(graph.flareTakeover.senderNodeID, 0x0000_1002)
        XCTAssertEqual(graph.flareTakeover.totalDurationSeconds, 120)

        await graph.stop()
    }

    /// S10/task point (1)+(4): backgrounded, the takeover never renders
    /// — a local notification fires instead.
    func testInboundFlareWhileBackgroundedNeverShowsTheTakeoverAndPostsANotificationInstead() async {
        let client = CountingClient()
        let notifications = RecordingNotificationSending()
        let graph = AppGraph(dependencies: dependencies(client: client), notifications: notifications)
        graph.core.crew.setPaired(nodeID: 0x0000_1002, paired: true)
        await graph.start()
        graph.setForegrounded(false)

        client.yieldPrivate(IncomingPrivate(
            from: 0x0000_1002, to: meshBroadcastAddress, channel: 0, packetID: 81,
            payload: FireflyPacket.flare(durationS: 60).encode()!, rxTime: Date(), rssiDbm: -60, snrDb: nil,
            direct: true))

        await waitUntil { !notifications.flareCalls.isEmpty }
        XCTAssertFalse(graph.flareTakeover.isActive, "never shown in the background beyond a local notification")
        XCTAssertEqual(notifications.flareCalls.count, 1)

        await graph.stop()
    }

    /// The feed keeps a record of an inbound FLARE regardless of
    /// foreground state (S10: "feed item remains").
    func testInboundFlarePushesAFeedItemEvenWhileBackgrounded() async {
        let client = CountingClient()
        let notifications = RecordingNotificationSending()
        let graph = AppGraph(dependencies: dependencies(client: client), notifications: notifications)
        graph.core.crew.setPaired(nodeID: 0x0000_1002, paired: true)
        await graph.start()
        graph.setForegrounded(false)

        client.yieldPrivate(IncomingPrivate(
            from: 0x0000_1002, to: meshBroadcastAddress, channel: 0, packetID: 82,
            payload: FireflyPacket.flare(durationS: 60).encode()!, rxTime: Date(), rssiDbm: -60, snrDb: nil,
            direct: true))

        await waitUntil { !graph.inboxProvider.thread(for: .crew, now: Date()).isEmpty }
        let message = graph.inboxProvider.thread(for: .crew, now: Date()).last
        XCTAssertEqual(message?.kind, .flare)
        XCTAssertEqual(message?.senderID, 0x0000_1002)

        await graph.stop()
    }

    /// FLARE_END only clears a takeover currently showing FOR THAT
    /// sender — a stale end naming someone else must not touch it. Both
    /// senders are paired here, so this stays a test of the SENDER-MATCH
    /// guard specifically, not a re-test of the pairing gate.
    func testFlareEndOnlyClearsTheTakeoverForTheMatchingSender() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))
        graph.core.crew.setPaired(nodeID: 0x0000_1002, paired: true)
        graph.core.crew.setPaired(nodeID: 0x0000_1003, paired: true)
        await graph.start()

        client.yieldPrivate(IncomingPrivate(
            from: 0x0000_1002, to: meshBroadcastAddress, channel: 0, packetID: 90,
            payload: FireflyPacket.flare(durationS: 300).encode()!, rxTime: Date(), rssiDbm: -60, snrDb: nil,
            direct: true))
        await waitUntil { graph.flareTakeover.isActive }

        // A FLARE_END from a DIFFERENT (but still paired) sender must
        // not clear it.
        client.yieldPrivate(IncomingPrivate(
            from: 0x0000_1003, to: meshBroadcastAddress, channel: 0, packetID: 91,
            payload: FireflyPacket.flareEnd.encode()!, rxTime: Date(), rssiDbm: -60, snrDb: nil, direct: true))
        try? await Task.sleep(nanoseconds: 150_000_000)
        XCTAssertTrue(graph.flareTakeover.isActive, "a FLARE_END naming someone else must not touch this takeover")

        // The matching sender's FLARE_END does clear it.
        client.yieldPrivate(IncomingPrivate(
            from: 0x0000_1002, to: meshBroadcastAddress, channel: 0, packetID: 92,
            payload: FireflyPacket.flareEnd.encode()!, rxTime: Date(), rssiDbm: -60, snrDb: nil, direct: true))
        await waitUntil { !graph.flareTakeover.isActive }

        await graph.stop()
    }

    // MARK: - M2: unpaired senders are dropped silently (PR #271 review, BLOCKING finding 1)

    /// Foregrounded: an unpaired sender's FLARE must never open the
    /// takeover, fire the haptic, or leave a feed item — S04's
    /// Addressing section ("only react if sender is paired"), mirroring
    /// the puck's own `wiring_push_if_paired` (`ff_wiring.c`).
    func testUnpairedFlareWhileForegroundedShowsNoTakeoverNoHapticAndNoFeedItem() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))
        let haptics = RecordingHapticSignaling()
        graph.flareTakeover.setHaptics(haptics)
        // Deliberately never paired.
        await graph.start()
        graph.setForegrounded(true)

        client.yieldPrivate(IncomingPrivate(
            from: 0x0000_9001, to: meshBroadcastAddress, channel: 0, packetID: 200,
            payload: FireflyPacket.flare(durationS: 120).encode()!, rxTime: Date(), rssiDbm: -60, snrDb: nil,
            direct: true))

        // Sentinel: a PAIRED sender's STATUS, sent after the unpaired
        // FLARE above on the same serial `incomingPrivate()` pipeline
        // (`AppGraph.observePrivatePackets()`'s single `for await` loop)
        // — once this lands, the FLARE has certainly already been
        // handled (and dropped) rather than merely not-yet-delivered.
        graph.core.crew.setPaired(nodeID: 0x0000_9002, paired: true)
        client.yieldPrivate(IncomingPrivate(
            from: 0x0000_9002, to: meshBroadcastAddress, channel: 0, packetID: 201,
            payload: FireflyPacket.status("sentinel").encode()!, rxTime: Date(), rssiDbm: -60, snrDb: nil,
            direct: false))
        await waitUntil { !graph.inboxProvider.thread(for: .crew, now: Date()).isEmpty }

        XCTAssertFalse(graph.flareTakeover.isActive, "an unpaired sender must never open the takeover")
        XCTAssertEqual(haptics.flareAlertCount, 0, "an unpaired sender must never trigger the FLARE haptic")
        let crew = graph.inboxProvider.thread(for: .crew, now: Date())
        XCTAssertFalse(crew.contains { $0.senderID == 0x0000_9001 }, "no feed item for an unpaired FLARE sender")
        XCTAssertEqual(crew.count, 1, "only the paired sentinel STATUS should have landed")

        await graph.stop()
    }

    /// Backgrounded: an unpaired sender's FLARE must never post a local
    /// notification either — the pairing gate runs before ANY reaction,
    /// notification included.
    func testUnpairedFlareWhileBackgroundedPostsNoNotificationAndNoFeedItem() async {
        let client = CountingClient()
        let notifications = RecordingNotificationSending()
        let graph = AppGraph(dependencies: dependencies(client: client), notifications: notifications)
        // Deliberately never paired.
        await graph.start()
        graph.setForegrounded(false)

        client.yieldPrivate(IncomingPrivate(
            from: 0x0000_9003, to: meshBroadcastAddress, channel: 0, packetID: 202,
            payload: FireflyPacket.flare(durationS: 60).encode()!, rxTime: Date(), rssiDbm: -60, snrDb: nil,
            direct: true))

        graph.core.crew.setPaired(nodeID: 0x0000_9004, paired: true)
        client.yieldPrivate(IncomingPrivate(
            from: 0x0000_9004, to: meshBroadcastAddress, channel: 0, packetID: 203,
            payload: FireflyPacket.status("sentinel").encode()!, rxTime: Date(), rssiDbm: -60, snrDb: nil,
            direct: false))
        await waitUntil { !graph.inboxProvider.thread(for: .crew, now: Date()).isEmpty }

        XCTAssertTrue(notifications.flareCalls.isEmpty, "an unpaired sender must never post a FLARE notification")
        let crew = graph.inboxProvider.thread(for: .crew, now: Date())
        XCTAssertFalse(crew.contains { $0.senderID == 0x0000_9003 }, "no feed item for an unpaired FLARE sender")
        XCTAssertEqual(crew.count, 1, "only the paired sentinel STATUS should have landed")

        await graph.stop()
    }

    // MARK: - M2: inbound RALLY / STATUS (S04)

    func testInboundRallyPushesAFeedItemWithDistanceAndBearingWhenWeHaveAFix() async {
        let client = CountingClient()
        let location = ScriptedLocationProvider()
        let graph = AppGraph(dependencies: dependencies(client: client, location: location))
        graph.core.crew.setPaired(nodeID: 0x0000_1004, paired: true)
        await graph.start()

        location.push(fix(latitude: 43.700000, longitude: -121.500000))
        // Same convention `testGPSUplinkPushesOnlyWhileSharingIsOnAndStopsWhenItIsTurnedOff`
        // uses to let a pushed fix actually reach its subscriber before
        // the next step depends on it.
        try? await Task.sleep(nanoseconds: 150_000_000)

        let rallyPayload = FireflyPacket.rally(latitude: 43.701000, longitude: -121.500000, name: "MY SPOT").encode()
        XCTAssertNotNil(rallyPayload)
        client.yieldPrivate(IncomingPrivate(
            from: 0x0000_1004, to: meshBroadcastAddress, channel: 0, packetID: 95, payload: rallyPayload!,
            rxTime: Date(), rssiDbm: -60, snrDb: nil, direct: false))

        await waitUntil { !graph.inboxProvider.thread(for: .crew, now: Date()).isEmpty }
        let message = try! XCTUnwrap(graph.inboxProvider.thread(for: .crew, now: Date()).last)
        XCTAssertEqual(message.kind, .rally)
        XCTAssertTrue(message.text.contains("MY SPOT"), "the place label must survive: \(message.text)")
        XCTAssertTrue(message.text.contains("of you"), "a real fix on both ends must render distance/bearing: \(message.text)")

        await graph.stop()
    }

    func testInboundRallyWithNoFixOfOurOwnShowsOnlyTheName() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client)) // UnavailableLocationProvider — no fix, ever
        graph.core.crew.setPaired(nodeID: 0x0000_1004, paired: true)
        await graph.start()

        let rallyPayload = FireflyPacket.rally(latitude: 43.701000, longitude: -121.500000, name: "THE TOWER").encode()
        client.yieldPrivate(IncomingPrivate(
            from: 0x0000_1004, to: meshBroadcastAddress, channel: 0, packetID: 96, payload: rallyPayload!,
            rxTime: Date(), rssiDbm: -60, snrDb: nil, direct: false))

        await waitUntil { !graph.inboxProvider.thread(for: .crew, now: Date()).isEmpty }
        let message = try! XCTUnwrap(graph.inboxProvider.thread(for: .crew, now: Date()).last)
        XCTAssertEqual(message.text, "THE TOWER", "no fix of our own — the honest answer is the name alone, never a fabricated distance")

        await graph.stop()
    }

    /// SHOULD-FIX 3: a stale fix of our own is not honest grounds for a
    /// confident bearing either, even with a real RALLY position on the
    /// other end.
    func testInboundRallyWithAStaleFixOfOurOwnRendersTheStalenessReasonNotABearing() async {
        let client = CountingClient()
        let location = ScriptedLocationProvider()
        let graph = AppGraph(dependencies: dependencies(client: client, location: location))
        graph.core.crew.setPaired(nodeID: 0x0000_1004, paired: true)
        await graph.start()

        // FF_CREW_LIVE_MS is 45s (ff_crew.h) — 6 minutes old is
        // squarely past it.
        location.push(fix(latitude: 43.700000, longitude: -121.500000,
                           time: Date().addingTimeInterval(-6 * 60)))
        try? await Task.sleep(nanoseconds: 150_000_000)

        let rallyPayload = FireflyPacket.rally(latitude: 43.701000, longitude: -121.500000, name: "MY SPOT").encode()
        client.yieldPrivate(IncomingPrivate(
            from: 0x0000_1004, to: meshBroadcastAddress, channel: 0, packetID: 950, payload: rallyPayload!,
            rxTime: Date(), rssiDbm: -60, snrDb: nil, direct: false))

        await waitUntil { !graph.inboxProvider.thread(for: .crew, now: Date()).isEmpty }
        let message = try! XCTUnwrap(graph.inboxProvider.thread(for: .crew, now: Date()).last)
        XCTAssertEqual(message.text, "MY SPOT — no bearing (your fix is 6 min old)")

        await graph.stop()
    }

    func testInboundStatusPushesAFeedItemWithItsText() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))
        graph.core.crew.setPaired(nodeID: 0x0000_1003, paired: true)
        await graph.start()

        client.yieldPrivate(IncomingPrivate(
            from: 0x0000_1003, to: meshBroadcastAddress, channel: 0, packetID: 97,
            payload: FireflyPacket.status("RAGING").encode()!, rxTime: Date(), rssiDbm: -60, snrDb: nil,
            direct: false))

        await waitUntil { !graph.inboxProvider.thread(for: .crew, now: Date()).isEmpty }
        let message = try! XCTUnwrap(graph.inboxProvider.thread(for: .crew, now: Date()).last)
        XCTAssertEqual(message.kind, .status)
        XCTAssertEqual(message.text, "RAGING")

        await graph.stop()
    }

    /// A direct (non-broadcast) RALLY/STATUS lands in the SENDER's own
    /// 1:1 thread, not CREW — same membership rule ordinary inbound
    /// text uses.
    func testDirectInboundStatusLandsInTheSendersOwnThreadNotCrew() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))
        graph.core.crew.setPaired(nodeID: 0x0000_1003, paired: true)
        await graph.start()

        client.yieldPrivate(IncomingPrivate(
            from: 0x0000_1003, to: 48_621_524, channel: 0, packetID: 98,
            payload: FireflyPacket.status("solo mission").encode()!, rxTime: Date(), rssiDbm: -60, snrDb: nil,
            direct: true))

        await waitUntil { !graph.inboxProvider.thread(for: .member(0x0000_1003), now: Date()).isEmpty }
        XCTAssertTrue(graph.inboxProvider.thread(for: .crew, now: Date()).isEmpty,
                       "a direct STATUS must not also appear in CREW")

        await graph.stop()
    }

    /// SHOULD-FIX 2: `to == 0` is protobuf's zero-default for an unset
    /// field, not a real broadcast — `pushInboundFeedItem`'s routing
    /// must agree with `InboxViewModel.ingest(_:)`'s (both now call the
    /// same `isBroadcastDestination` helper), so it must land in the
    /// SENDER's own thread, exactly like a direct message would.
    func testStatusAddressedToZeroRoutesAsDirectNotBroadcast() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))
        graph.core.crew.setPaired(nodeID: 0x0000_1005, paired: true)
        await graph.start()

        client.yieldPrivate(IncomingPrivate(
            from: 0x0000_1005, to: 0, channel: 0, packetID: 99,
            payload: FireflyPacket.status("to-zero").encode()!, rxTime: Date(), rssiDbm: -60, snrDb: nil,
            direct: true))

        await waitUntil { !graph.inboxProvider.thread(for: .member(0x0000_1005), now: Date()).isEmpty }
        XCTAssertTrue(graph.inboxProvider.thread(for: .crew, now: Date()).isEmpty,
                       "to == 0 is not a real broadcast — must not land in CREW")

        await graph.stop()
    }

    /// Unpaired RALLY: no feed item, same trust boundary as FLARE.
    func testUnpairedRallyPushesNoFeedItem() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))
        // Deliberately never paired.
        await graph.start()

        let rallyPayload = FireflyPacket.rally(latitude: 43.701000, longitude: -121.500000, name: "UNKNOWN SPOT").encode()
        client.yieldPrivate(IncomingPrivate(
            from: 0x0000_9005, to: meshBroadcastAddress, channel: 0, packetID: 204, payload: rallyPayload!,
            rxTime: Date(), rssiDbm: -60, snrDb: nil, direct: false))

        graph.core.crew.setPaired(nodeID: 0x0000_9006, paired: true)
        client.yieldPrivate(IncomingPrivate(
            from: 0x0000_9006, to: meshBroadcastAddress, channel: 0, packetID: 205,
            payload: FireflyPacket.status("sentinel").encode()!, rxTime: Date(), rssiDbm: -60, snrDb: nil,
            direct: false))
        await waitUntil { !graph.inboxProvider.thread(for: .crew, now: Date()).isEmpty }

        let crew = graph.inboxProvider.thread(for: .crew, now: Date())
        XCTAssertFalse(crew.contains { $0.senderID == 0x0000_9005 }, "no feed item for an unpaired RALLY sender")
        XCTAssertEqual(crew.count, 1, "only the paired sentinel STATUS should have landed")

        await graph.stop()
    }

    /// Unpaired STATUS: no feed item, same trust boundary as FLARE.
    func testUnpairedStatusPushesNoFeedItem() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))
        // Deliberately never paired.
        await graph.start()

        client.yieldPrivate(IncomingPrivate(
            from: 0x0000_9007, to: meshBroadcastAddress, channel: 0, packetID: 206,
            payload: FireflyPacket.status("unpaired").encode()!, rxTime: Date(), rssiDbm: -60, snrDb: nil,
            direct: false))

        graph.core.crew.setPaired(nodeID: 0x0000_9008, paired: true)
        client.yieldPrivate(IncomingPrivate(
            from: 0x0000_9008, to: meshBroadcastAddress, channel: 0, packetID: 207,
            payload: FireflyPacket.status("sentinel").encode()!, rxTime: Date(), rssiDbm: -60, snrDb: nil,
            direct: false))
        await waitUntil { !graph.inboxProvider.thread(for: .crew, now: Date()).isEmpty }

        let crew = graph.inboxProvider.thread(for: .crew, now: Date())
        XCTAssertFalse(crew.contains { $0.senderID == 0x0000_9007 }, "no feed item for an unpaired STATUS sender")
        XCTAssertEqual(crew.count, 1, "only the paired sentinel STATUS should have landed")

        await graph.stop()
    }

    // MARK: - M2: background lifecycle (docs/specs/A01-companion-app.md,
    // "the 'stay connected in background' setting actually gating this")

    func testBackgroundWithSettingOffStopsAndDisconnects() async {
        let client = CountingClient()
        let store = InMemorySettingsStore()
        store.backgroundConnectEnabled = false
        let graph = AppGraph(dependencies: dependencies(client: client, store: store))
        await graph.start()

        await graph.handleScenePhaseChange(.background)

        XCTAssertEqual(client.disconnectCallCount, 1,
                        "off = disconnect when backgrounded, per the M2 task's own words")
        // The graph's own subscriptions stood down too — re-subscribing
        // would show up as more `link` subscriptions once restarted. "2"
        // is both of the graph's OWN `linkState()` readers from this one
        // `start()` — `core.observe(client:)` and M3's
        // `observeHistoryOutboxFlush()` (each its own independent S1
        // subscription) — not a sign either one resubscribed.
        XCTAssertEqual(client.subscriptionCount("link"), 2)
    }

    func testBackgroundWithSettingOnDoesNothing() async {
        let client = CountingClient()
        let store = InMemorySettingsStore()
        store.backgroundConnectEnabled = true
        let graph = AppGraph(dependencies: dependencies(client: client, store: store))
        await graph.start()

        await graph.handleScenePhaseChange(.background)

        XCTAssertEqual(client.disconnectCallCount, 0,
                        "on = the link (and BLETransport's own reconnect-on-loss loop underneath it) stays up")

        await graph.stop()
    }

    func testForegroundAfterAnOffBackgroundRestartsTheGraphButDoesNotAutoReconnect() async {
        let client = CountingClient()
        let store = InMemorySettingsStore()
        store.backgroundConnectEnabled = false
        let graph = AppGraph(dependencies: dependencies(client: client, store: store))
        await graph.start()
        let connectCallsAtLaunch = client.connectCallCount

        await graph.handleScenePhaseChange(.background)
        XCTAssertEqual(client.disconnectCallCount, 1)

        await graph.handleScenePhaseChange(.foreground)

        // The graph's own subscriptions are back — "4" is two
        // independent `link` readers (`core.observe(client:)` and M3's
        // `observeHistoryOutboxFlush()`) per `start()`, times two
        // `start()` calls (launch + this restart)...
        XCTAssertEqual(client.subscriptionCount("link"), 4)
        // ...but nothing auto-reconnected the CLIENT on its own — "off
        // means off, the user taps CONNECT again", same as M1.
        XCTAssertEqual(client.connectCallCount, connectCallsAtLaunch,
                        "coming back to the foreground after an off-background disconnect must not silently reconnect")

        await graph.stop()
    }

    func testForegroundIsANoOpWhenTheGraphNeverStopped() async {
        let client = CountingClient()
        let store = InMemorySettingsStore()
        store.backgroundConnectEnabled = true
        let graph = AppGraph(dependencies: dependencies(client: client, store: store))
        await graph.start()

        await graph.handleScenePhaseChange(.foreground) // never backgrounded — start()'s own idempotency

        // "2" is the graph's own two independent `link` readers from the
        // ONE `start()` call above (`core.observe(client:)` +
        // `observeHistoryOutboxFlush()`), not a resubscription.
        XCTAssertEqual(client.subscriptionCount("link"), 2, "start() is idempotent; foreground must not resubscribe")

        await graph.stop()
    }

    // MARK: - M2: auto-connect at launch to the remembered peripheral

    func testStartAutoConnectsWhenALastPeripheralIsRemembered() async {
        let client = CountingClient()
        let store = InMemorySettingsStore()
        store.setString(UUID().uuidString, .lastPeripheralID)
        let graph = AppGraph(dependencies: dependencies(client: client, store: store))

        await graph.start()
        await waitUntil { client.connectCallCount == 1 }

        XCTAssertEqual(client.connectCallCount, 1)

        await graph.stop()
    }

    func testStartDoesNotAutoConnectWithoutARememberedPeripheral() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client)) // fresh InMemorySettingsStore, nothing persisted

        await graph.start()
        try? await Task.sleep(nanoseconds: 100_000_000)

        XCTAssertEqual(client.connectCallCount, 0,
                        "no remembered peripheral (the stub/demo stacks always start empty here) means no auto-connect noise")

        await graph.stop()
    }

    func testAutoConnectAtLaunchFiresOnlyOnceEvenAcrossABackgroundRestart() async {
        let client = CountingClient()
        let store = InMemorySettingsStore()
        store.setString(UUID().uuidString, .lastPeripheralID)
        store.backgroundConnectEnabled = true
        let graph = AppGraph(dependencies: dependencies(client: client, store: store))

        await graph.start()
        await waitUntil { client.connectCallCount == 1 }

        // A background/foreground cycle later (setting stays ON here, so
        // `stop()` is never actually called) must not fire a SECOND
        // launch auto-connect.
        await graph.handleScenePhaseChange(.background)
        await graph.handleScenePhaseChange(.foreground)
        try? await Task.sleep(nanoseconds: 100_000_000)

        XCTAssertEqual(client.connectCallCount, 1, "the launch auto-connect is a one-shot, not per-foreground")

        await graph.stop()
    }

    // MARK: - "app: fix live connect path never reaching CONNECTED on
    // macOS" — the live-graph reproduction the investigation itself
    // asked for.
    //
    // Every test above this line drives `AppGraph` against
    // `CountingClient`, an honest double that publishes exactly what the
    // test tells it to and nothing else — it cannot reproduce a bug that
    // lives in how a REAL `MeshtasticClient` (its reentrancy guard, its
    // own `CurrentValueEventHub`, its actual want_config handshake) and
    // the graph built on top of it interact. `BLEHardwareTests`
    // (`FireflyHardwareTests`, gated, real board only) constructs
    // `MeshtasticClient` directly and passes — it never goes through
    // `AppGraph`/`ConnectViewModel` at all. This is the missing middle:
    // the exact same live wiring the Connect screen uses — `AppGraph`
    // composition root, a REAL `MeshtasticClient`, `ConnectViewModel`
    // observing it — with `LoopbackTransport` standing in for
    // `BLETransport` so the test needs no radio, matching the mocked-
    // transport discipline `ConnectViewModelTests`/`ClientReconnectTests`
    // already use elsewhere in this suite.
    func testConnectViewModelReachesReadyThroughTheLiveGraphOverLoopback() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        // A fresh `InMemorySettingsStore()` — nothing remembered — so
        // `AppGraph.start()`'s own launch auto-connect never fires here;
        // that race is `ConnectViewModelTests
        // .testAlreadyConnectingRaceNeverShowsFailed`'s own, already
        // pinned scenario. This test isolates the OTHER question: does a
        // single, ordinary CONNECT tap ever reach `.ready` when routed
        // through the whole live graph, not just a bare client?
        let graph = AppGraph(dependencies: AppDependencies(
            client: client, location: UnavailableLocationProvider(), heading: NoHeadingProvider(),
            store: InMemorySettingsStore()))

        await graph.start()
        let connect = graph.makeConnectViewModel()
        // Same ordering `ConnectScreen.onAppear` uses — subscribe BEFORE
        // the CONNECT button's own `connect()` call, never after (S1).
        connect.observe()

        // Fired as its own `Task`, NOT awaited inline — exactly what
        // `ConnectScreen`'s own `Button(...) { Task { await connect
        // .connect() } }` does. `connect.connect()` does not return
        // until the whole handshake resolves (success or the 30s/120s
        // phase timeout), so awaiting it inline here — before this test
        // has injected anything for it to resolve WITH — would just
        // reproduce this test's own mistake, not the product's: a real
        // board answers want_config on its own over the wire while
        // `connect()` is in flight, which is what firing this off lets
        // the two injects below stand in for.
        let connectTask = Task { await connect.connect() }

        // Drive the real want_config handshake exactly the way
        // `ConnectViewModelTests.testAlreadyConnectingRaceNeverShowsFailed`
        // does against the same `MeshtasticClient` type — this transport
        // has no scripted board behind it, so nothing answers unless
        // this test injects it.
        try await waitForSentCount(2, on: transport) // heartbeat, want_config(onlyConfig)
        // Firefly 1's own node number (!02e606b0) — not load-bearing for
        // the assertion, just an honest stand-in rather than an
        // arbitrary magic number.
        transport.inject(fromRadio { $0.myInfo.myNodeNum = 48_629_424 })
        transport.inject(fromRadio { $0.configCompleteID = MeshtasticConfigNonce.onlyConfig })
        try await waitForSentCount(3, on: transport) // want_config(onlyNodeDB)
        transport.inject(fromRadio { $0.configCompleteID = MeshtasticConfigNonce.onlyNodeDB })
        _ = await connectTask.value

        for _ in 0..<200 where connect.link != .ready {
            try? await Task.sleep(nanoseconds: 5_000_000)
        }
        XCTAssertEqual(connect.link, .ready,
                        "CONNECT through the live graph (AppGraph + real MeshtasticClient + LoopbackTransport) " +
                        "must reach .ready — if this fails, the defect is in the graph, not the transport")
        XCTAssertEqual(connect.statusLabel, "CONNECTED")
        XCTAssertNil(connect.lastError)

        connect.stopObserving()
        await graph.stop()
    }

    /// The actual bug, pinned: `ConnectViewModel.observe()` must start on
    /// its own, from `AppGraph.makeConnectViewModel()`, the moment the
    /// view model is built — NEVER left for a caller (`ConnectScreen
    /// .onAppear`, in production) to start later. Before the fix, a
    /// `NavigationSplitView` detail-column remount at launch (bench-
    /// reproduced against a real board, 2026-09-11, no user action at
    /// all) fired `ConnectScreen`'s `.onDisappear` once with no matching
    /// `.onAppear` ever following it — `connect.stopObserving()`
    /// cancelled the ONE subscription before anything had called
    /// `connect()`, and `MeshtasticClient` going on to reach `.ready`
    /// perfectly (`BLETransport`'s own log showed a clean connect
    /// sequence) changed nothing: nobody was listening any more. This
    /// test never calls `connect.observe()` at all — reproducing exactly
    /// that "screen never got the chance to subscribe" shape — and
    /// still expects `.ready` to arrive, because `makeConnectViewModel()`
    /// itself is supposed to have already started it.
    func testConnectViewModelObservesAutomaticallyWithNoCallerEverCallingObserve() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))
        await graph.start()

        let connect = graph.makeConnectViewModel()
        // Deliberately NOT calling `connect.observe()` here — the whole
        // point of this test.

        client.yieldLink(.connecting)
        client.yieldLink(.handshaking)
        client.yieldLink(.ready)

        await waitUntil { connect.link == .ready }
        XCTAssertEqual(connect.link, .ready,
                        "the Connect screen must reflect .ready even if no screen ever called observe() itself — " +
                        "makeConnectViewModel() owns starting this subscription, not ConnectScreen.onAppear")
        XCTAssertEqual(connect.statusLabel, "CONNECTED")

        await graph.stop()
    }

    /// PR #276 follow-up (app: singleton view models own their
    /// subscriptions in the composition root) — the same fix and the
    /// same test shape as `testConnectViewModelObservesAutomaticallyWith
    /// NoCallerEverCallingObserve` just above, now for `RadarViewModel`.
    /// Fails pre-fix: before `makeRadarViewModel(haptics:)` started
    /// `observe()` itself, nothing did until `RadarView.onAppear` ran,
    /// so a fix pushed with no screen ever having appeared (or one that
    /// appeared once and was remounted away, exactly `ConnectScreen`'s
    /// own NavigationSplitView bug) was silently dropped.
    func testRadarViewModelObservesAutomaticallyWithNoCallerEverCallingObserve() async {
        let client = CountingClient()
        let location = ScriptedLocationProvider()
        let graph = AppGraph(dependencies: dependencies(client: client, location: location))
        await graph.start()

        let radar = graph.makeRadarViewModel()
        // Deliberately NOT calling `radar.observe()` here — the whole
        // point of this test.

        let pushed = fix(latitude: 47.7, longitude: -122.28)
        location.push(pushed)

        await waitUntil { radar.lastFix != nil }
        XCTAssertEqual(radar.lastFix, pushed,
                        "the Radar screen must mirror a fix even if no screen ever called observe() itself — " +
                        "makeRadarViewModel(haptics:) owns starting this subscription, not RadarView.onAppear")

        await graph.stop()
    }

    /// Same fix, same shape, for `InboxViewModel` — fails pre-fix for
    /// the identical reason `testRadarViewModelObservesAutomaticallyWith
    /// NoCallerEverCallingObserve` just above does: before
    /// `makeInboxViewModel()` started `observe()` itself, an inbound
    /// text arriving with no Inbox screen ever having appeared was
    /// silently lost — `ingest(_:)` is only ever reached from THIS
    /// subscription.
    func testInboxViewModelObservesAutomaticallyWithNoCallerEverCallingObserve() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))
        await graph.start()

        let inbox = graph.makeInboxViewModel()
        // Deliberately NOT calling `inbox.observe()` here — the whole
        // point of this test.

        client.yieldText(IncomingText(from: 0x02E6_06B0, to: meshBroadcastAddress, channel: 0, packetID: 11,
                                       text: "hello crew", rxTime: Date(), rssiDbm: -60, snrDb: 6, direct: nil))

        await waitUntil { inbox.conversations.first(where: { $0.kind == .crew })?.itemCount == 1 }
        let crewRow = inbox.conversations.first(where: { $0.kind == .crew })
        XCTAssertEqual(crewRow?.itemCount, 1,
                        "the Inbox screen must reflect an inbound text even if no screen ever called observe() " +
                        "itself — makeInboxViewModel() owns starting this subscription, not InboxContainerView.onAppear")
        XCTAssertTrue(crewRow?.hasPreview ?? false)

        await graph.stop()
    }

    // MARK: - M3: persistence (docs/specs/A01-companion-app.md, M3)

    /// The persisted-outbox flush: a WAITING item that never left the
    /// device before a relaunch has no live `ThreadViewModel` watching
    /// it (that type's own in-memory outbox only ever holds what IT
    /// personally queued this session) — `AppGraph.flushPersistedOutbox()`
    /// is what re-attempts it, on the link's next not-ready -> ready
    /// edge, entirely independent of whether any screen is open.
    func testPersistedWaitingOutboxFlushesOnConnect() async {
        let client = CountingClient()
        let history = HistoryStore.inMemory()
        history.record(
            FeedMessage(id: 1, kind: .text, direction: .out, text: "still waiting when the app died",
                        timestamp: Date().addingTimeInterval(-600), destination: 0x0000_2001,
                        deliveryState: .waiting),
            in: .member(0x0000_2001))

        let graph = AppGraph(dependencies: dependencies(client: client), historyStore: history)
        await graph.start()

        // Not sent yet — nothing has connected.
        XCTAssertTrue(client.sentTexts.isEmpty)

        // The link's own not-ready -> ready edge is what triggers the
        // flush — never `start()` itself.
        client.yieldLink(.ready)
        await waitUntil { !client.sentTexts.isEmpty }

        XCTAssertEqual(client.sentTexts.first?.0, "still waiting when the app died")
        XCTAssertEqual(client.sentTexts.first?.1, 0x0000_2001)

        await waitUntil {
            self.graphInboxThread(graph, .member(0x0000_2001)).first?.deliveryState == .sent
        }
        let restored = graphInboxThread(graph, .member(0x0000_2001)).first
        XCTAssertEqual(restored?.deliveryState, .sent, "flushed WAITING -> SENT, the same transition a live send makes")

        // The persisted store agrees — nothing is WAITING any more.
        XCTAssertTrue(history.pendingOutbox(cap: 8).isEmpty)

        await graph.stop()
    }

    /// A WAITING item beyond `ThreadViewModel.outboxCap` is never
    /// flushed at all (the same bounded-FIFO discipline a live thread's
    /// own outbox enforces) — proved here with exactly `outboxCap + 1`
    /// persisted items, oldest dropped from the flush.
    func testPersistedOutboxFlushIsBoundedOldestFirst() async {
        let client = CountingClient()
        let history = HistoryStore.inMemory()
        let cap = ThreadViewModel.outboxCap
        for i in 0..<(cap + 1) {
            history.record(
                FeedMessage(id: UInt64(i + 1), kind: .text, direction: .out, text: "msg \(i)",
                            timestamp: Date().addingTimeInterval(TimeInterval(i) - 1000), destination: 0x0000_2002,
                            deliveryState: .waiting),
                in: .member(0x0000_2002))
        }

        let graph = AppGraph(dependencies: dependencies(client: client), historyStore: history)
        await graph.start()
        client.yieldLink(.ready)
        await waitUntil { client.sentTexts.count >= cap }
        // Give any (incorrect) extra flush a moment to show up before
        // asserting the ceiling held.
        try? await Task.sleep(nanoseconds: 20_000_000)

        XCTAssertEqual(client.sentTexts.count, cap, "the flush must never exceed ThreadViewModel.outboxCap")
        XCTAssertEqual(client.sentTexts.first?.0, "msg 0", "oldest first")
        XCTAssertFalse(client.sentTexts.contains { $0.0 == "msg \(cap)" }, "the newest overflow item stays queued")

        await graph.stop()
    }

    /// Cold-launch restore, then a REAL live want_config-shaped replay
    /// for the same member: the restored messages keep their own
    /// "FROM STORAGE" tag and original ages, the live one does not, and
    /// nothing is duplicated — the exact rule `HistoryRestorer`'s own
    /// header comment states ("a restored member later heard live must
    /// flip to live without duplicates").
    func testRestoredHistorySurvivesALiveWantConfigReplayWithoutDuplicating() async {
        let taylor: UInt32 = 0x0000_3001
        let history = HistoryStore.inMemory()
        history.record(
            FeedMessage(id: 0x8000_0000_0000_9001, kind: .text, direction: .direct, senderID: taylor,
                        senderName: "Taylor", text: "yesterday's message", timestamp: Date().addingTimeInterval(-3600)),
            in: .member(taylor))

        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client), historyStore: history)

        // Restore already ran, inside `AppGraph.init`, before `start()`
        // (and therefore before ANY client stream is even subscribed) —
        // the ordering `HistoryRestorer.restore`'s own doc comment
        // requires.
        let beforeReplay = graphInboxThread(graph, .member(taylor))
        XCTAssertEqual(beforeReplay.count, 1)
        XCTAssertTrue(beforeReplay[0].isRestored, "loaded from storage before any client stream existed")

        await graph.start()
        let inbox = graph.makeInboxViewModel()
        _ = inbox.openThread(.member(taylor)) // the same subscription a real Thread screen would start

        // The want_config-shaped replay: identity/position for Taylor,
        // THEN a live inbound text — the exact sequence a real
        // reconnect's nodeDB dump followed by fresh mesh traffic
        // produces.
        client.yieldNode(MeshNodeSnapshot(num: taylor, shortName: "TAY", longName: "Taylor", position: nil,
                                           lastHeard: Date(), rssiDbm: -50, snrDb: 6, hopsAway: 0))
        client.yieldText(IncomingText(from: taylor, to: 0x0000_0001, channel: 0, packetID: 555,
                                       text: "morning! live now", rxTime: Date(), rssiDbm: -50, snrDb: 6, direct: true))

        await waitUntil { self.graphInboxThread(graph, .member(taylor)).count == 2 }
        let afterReplay = graphInboxThread(graph, .member(taylor)).sorted { $0.timestamp < $1.timestamp }

        XCTAssertEqual(afterReplay.count, 2, "no duplicate of the restored message")
        XCTAssertTrue(afterReplay[0].isRestored, "the OLD message keeps its own restored tag permanently")
        XCTAssertEqual(afterReplay[0].text, "yesterday's message")
        XCTAssertFalse(afterReplay[1].isRestored, "the NEW live message was never in storage — it renders live")
        XCTAssertEqual(afterReplay[1].text, "morning! live now")

        await graph.stop()
    }

    private func graphInboxThread(_ graph: AppGraph, _ conversation: ConversationKind) -> [FeedMessage] {
        graph.inboxProvider.thread(for: conversation, now: Date())
    }

    private struct GraphTestTimeout: Error {}

    private func waitForSentCount(_ n: Int, on transport: LoopbackTransport,
                                   file: StaticString = #filePath, line: UInt = #line) async throws {
        for _ in 0..<400 {
            if transport.sentMessages.count >= n { return }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTFail("timed out waiting for \(n) sent message(s); saw \(transport.sentMessages.count)", file: file, line: line)
        throw GraphTestTimeout()
    }

    private func fromRadio(_ build: (inout FromRadio) -> Void) -> Data {
        var fr = FromRadio()
        build(&fr)
        return (try? fr.serializedData()) ?? Data()
    }
}
