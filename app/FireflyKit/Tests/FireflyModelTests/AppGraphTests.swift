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

    func connect() async throws { linkHub.yield(.ready) }
    func disconnect() async { linkHub.yield(.disconnected) }

    @discardableResult
    func sendText(_ text: String, to destination: UInt32, wantAck: Bool) async throws -> UInt32 {
        lock.lock(); texts.append((text, destination, wantAck)); lock.unlock()
        return 1
    }

    @discardableResult
    func sendPosition(_ fix: ExternalPositionFix, to destination: UInt32) async throws -> UInt32 {
        lock.lock(); positions.append((fix, destination)); lock.unlock()
        return 2
    }

    @discardableResult
    func sendPrivate(_ payload: Data, to destination: UInt32, wantAck: Bool) async throws -> UInt32 {
        lock.lock(); privates.append((payload, destination, wantAck)); lock.unlock()
        return 3
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

@MainActor
final class AppGraphTests: XCTestCase {

    private func waitUntil(_ condition: @escaping () -> Bool, timeout: Int = 400) async {
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

        XCTAssertEqual(client.subscriptionCount("link"), 1)
        XCTAssertEqual(client.subscriptionCount("node"), 1)
        XCTAssertEqual(client.subscriptionCount("delivery"), 1)
        XCTAssertEqual(client.subscriptionCount("private"), 1)
        // The graph itself does NOT read incoming texts — `InboxViewModel`
        // holds that subscription (its own, independent one, S1), so a
        // graph with no Inbox on screen must not have taken one.
        XCTAssertEqual(client.subscriptionCount("text"), 0)

        await graph.stop()
    }

    func testViewModelsHoldTheirOwnIndependentSubscriptions() async {
        let client = CountingClient()
        let graph = AppGraph(dependencies: dependencies(client: client))
        await graph.start()

        let inbox = graph.makeInboxViewModel()
        inbox.observe()

        // S1's multicast rule: the Inbox's subscriptions are ITS own,
        // never stolen from or shared with the graph's.
        XCTAssertEqual(client.subscriptionCount("text"), 1)
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

    private func fix(latitude: Double, longitude: Double) -> LocationFix {
        LocationFix(latitude: latitude, longitude: longitude, altitude: 42, time: Date(),
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
}
