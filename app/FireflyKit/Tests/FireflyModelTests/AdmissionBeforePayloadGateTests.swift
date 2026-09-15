//
//  AdmissionBeforePayloadGateTests.swift — the 2026-09-14 bench race,
//  end to end: real `MeshtasticClient` over `LoopbackTransport`, real
//  `AppGraph`, real `CoreStore`, real `CrewMembershipEngine`, real
//  `ff_crew`. Scripted `FromRadio` bytes in; an admitted member and a
//  rendered FLARE out.
//
//  WHAT BROKE (Mac bench app, main `d8ee569d`, 14:24). The Heltec was
//  connected on crew FIRE-8MNTT2 and the XIAO puck (`!8f48af24` =
//  2403905316), not yet crew on this phone, sent a FLARE and then a
//  text on the crew channel. The app logged
//
//      [AppGraph] dropping inbound FLARE from unpaired/unknown sender=2403905316
//
//  and then ACCEPTED the text that followed. The FLARE — the packet
//  that should have admitted the sender under A02 §4.1 (decrypted, on
//  the crew channel, portnum 269) — was the one packet dropped, and the
//  text only got through because by then the FLARE's own rx-meta had
//  finally landed on the OTHER stream and admitted them.
//
//  HOW THESE TESTS FORCE THE OLD ORDERING, DETERMINISTICALLY.
//  `StalledNodeUpdatesClient` below wraps the real client and holds
//  back `nodeUpdates()` — and ONLY `nodeUpdates()` — until a test
//  releases it. That is not a contrived scenario: it is precisely the
//  race, made total. On main, `CoreStore`'s admission rides
//  `nodeUpdates()`, so holding that stream means the payload consumer
//  ALWAYS runs first and every test here fails, every run, with no
//  timing luck involved. With the fix, admission rides
//  `inboundPackets()` — the same ordered stream the payload rides — so
//  stalling `nodeUpdates()` cannot reorder anything, and the wrapper is
//  inert. A test that passes with the node stream held is a test that
//  proved the ordering does not depend on it.
//
//  Nothing is released at the end and nothing sleeps waiting for a
//  second chance: the assertions are made while the stall is still in
//  force.
//
import FireflyCore
import FireflyMesh
import FireflyModel
import MeshtasticProto
import XCTest

/// Forwards everything to a real `MeshtasticClient`, except that
/// `nodeUpdates()` elements are parked in a buffer until
/// `releaseNodeUpdates()` is called. Every other stream — including the
/// ordered `inboundPackets()` pipeline — is passed through untouched.
private final class StalledNodeUpdatesClient: MeshtasticClientProtocol, @unchecked Sendable {
    private let wrapped: MeshtasticClient
    private let nodeHub = EventHub<MeshNodeSnapshot>()
    private let lock = NSLock()
    private var parked: [MeshNodeSnapshot] = []
    private var stalled = true
    private var pump: Task<Void, Never>?

    init(wrapping client: MeshtasticClient) {
        self.wrapped = client
        let upstream = client.nodeUpdates()
        pump = Task { [weak self] in
            for await snapshot in upstream {
                guard let self else { return }
                self.accept(snapshot)
            }
        }
    }

    deinit { pump?.cancel() }

    private func accept(_ snapshot: MeshNodeSnapshot) {
        lock.lock()
        let holdIt = stalled
        if holdIt { parked.append(snapshot) }
        lock.unlock()
        if !holdIt { nodeHub.yield(snapshot) }
    }

    /// Only ever called by a test that WANTS the node stream to flow —
    /// no test in this file calls it before its assertions.
    func releaseNodeUpdates() {
        lock.lock()
        stalled = false
        let drained = parked
        parked.removeAll()
        lock.unlock()
        for snapshot in drained { nodeHub.yield(snapshot) }
    }

    var parkedCount: Int { lock.lock(); defer { lock.unlock() }; return parked.count }

    func linkState() -> AsyncStream<LinkState> { wrapped.linkState() }
    func nodeUpdates() -> AsyncStream<MeshNodeSnapshot> { nodeHub.subscribe() }
    func deliveryUpdates() -> AsyncStream<DeliveryEvent> { wrapped.deliveryUpdates() }
    func incomingTexts() -> AsyncStream<IncomingText> { wrapped.incomingTexts() }
    func incomingPrivate() -> AsyncStream<IncomingPrivate> { wrapped.incomingPrivate() }
    func inboundPackets() -> AsyncStream<InboundPacketEvent> { wrapped.inboundPackets() }
    var connectedNodeNum: UInt32? { wrapped.connectedNodeNum }

    func connect() async throws { try await wrapped.connect() }
    func beginListening() async { await wrapped.beginListening() }
    func disconnect() async { await wrapped.disconnect() }
    @discardableResult
    func sendText(_ text: String, to destination: UInt32, wantAck: Bool) async throws -> UInt32 {
        try await wrapped.sendText(text, to: destination, wantAck: wantAck)
    }
    @discardableResult
    func sendPosition(_ fix: ExternalPositionFix, to destination: UInt32) async throws -> UInt32 {
        try await wrapped.sendPosition(fix, to: destination)
    }
    @discardableResult
    func sendPrivate(_ payload: Data, to destination: UInt32, wantAck: Bool) async throws -> UInt32 {
        try await wrapped.sendPrivate(payload, to: destination, wantAck: wantAck)
    }
    @discardableResult
    func requestNodeInfo(from nodeID: UInt32) async throws -> UInt32 {
        try await wrapped.requestNodeInfo(from: nodeID)
    }
    @discardableResult
    func applyChannelSet(_ request: ChannelWriteRequest) async throws -> ChannelWriteReport {
        try await wrapped.applyChannelSet(request)
    }
    @discardableResult
    func setOwner(longName: String, shortName: String) async throws -> OwnerWriteReport {
        try await wrapped.setOwner(longName: longName, shortName: shortName)
    }
    @discardableResult
    func setRegion(_ region: Config.LoRaConfig.RegionCode) async throws -> RegionWriteReport {
        try await wrapped.setRegion(region)
    }
    func currentChannelTable() async throws -> [Channel] { try await wrapped.currentChannelTable() }
}

@MainActor
final class AdmissionBeforePayloadGateTests: XCTestCase {

    /// The bench puck, `!8f48af24`.
    private static let sender: UInt32 = 2_403_905_316
    private static let myNodeNum: UInt32 = 48_621_524
    private static let crewIndex: UInt32 = 0
    private static let crew = CrewChannelIdentity(
        code: "FIRE-8MNTT2",
        psk: Data((0..<32).map { UInt8($0 &+ 7) }))

    private struct Harness {
        let transport: LoopbackTransport
        let client: StalledNodeUpdatesClient
        let notifications: RecordingNotificationSending
        let graph: AppGraph
        /// Held, not merely built: an inbound TEXT is persisted by
        /// `InboxViewModel.ingest(_:)` off ITS OWN `incomingTexts()`
        /// subscription, which `makeInboxViewModel()` starts. In the
        /// shipped app the composition root builds this at launch and
        /// keeps it for the process; a test that never built one would
        /// find the crew thread empty for a reason that has nothing to
        /// do with admission ordering.
        let inbox: InboxViewModel
    }

    // MARK: - Harness

    private func frame(_ build: (inout FromRadio) -> Void) throws -> Data {
        var fr = FromRadio()
        build(&fr)
        return try fr.serializedData()
    }

    private func packetFrame(from: UInt32, channel: UInt32, portnum: PortNum,
                             payload: Data, packetID: UInt32 = 4242) throws -> Data {
        var pkt = MeshPacket()
        pkt.from = from
        pkt.to = meshBroadcastAddress
        pkt.channel = channel
        pkt.id = packetID
        pkt.hopStart = 3
        pkt.hopLimit = 3
        pkt.rxRssi = -71
        var data = DataMessage()
        data.portnum = portnum
        data.payload = payload
        pkt.decoded = data
        return try frame { $0.packet = pkt }
    }

    /// Portnum 269 — Firefly's own, matched by RAW VALUE everywhere
    /// (A02 §4.1 clause 6); it is not a named `PortNum` enumerator.
    private func fireflyFrame(_ packet: FireflyPacket, from: UInt32, channel: UInt32,
                              packetID: UInt32 = 4242) throws -> Data {
        let payload = try XCTUnwrap(packet.encode(), "FireflyPacket.encode() produced no bytes")
        return try packetFrame(from: from, channel: channel,
                               portnum: PortNum(rawValue: 269) ?? .privateApp,
                               payload: payload, packetID: packetID)
    }

    private func crewChannel(at index: Int32) -> Channel {
        var settings = ChannelSettings()
        settings.name = Self.crew.code
        settings.psk = Self.crew.psk
        var channel = Channel()
        channel.index = index
        channel.settings = settings
        channel.role = index == 0 ? .primary : .secondary
        return channel
    }

    private func makeHarness() -> Harness {
        let transport = LoopbackTransport()
        let client = StalledNodeUpdatesClient(wrapping: MeshtasticClient(transport: transport))
        let notifications = RecordingNotificationSending()
        let graph = AppGraph(
            dependencies: AppDependencies(client: client, location: UnavailableLocationProvider(),
                                          heading: NoHeadingProvider(), store: InMemorySettingsStore()),
            notifications: notifications,
            skipLaunchAutoConnectUnderXCTest: true)
        return Harness(transport: transport, client: client, notifications: notifications, graph: graph,
                       inbox: graph.makeInboxViewModel())
    }

    private func waitForSentCount(_ n: Int, on transport: LoopbackTransport) async throws {
        for _ in 0..<400 {
            if transport.sentMessages.count >= n { return }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTFail("timed out waiting for \(n) sent message(s); saw \(transport.sentMessages.count)")
    }

    /// Brings up a graph whose crew channel is resolved at index 0 and
    /// whose roster is EMPTY — `Self.sender` has never been seen.
    private func connectedCrewHarness() async throws -> Harness {
        let h = makeHarness()
        await h.graph.start()
        let connectTask = Task { try await h.client.connect() }
        try await waitForSentCount(2, on: h.transport)
        var info = MyNodeInfo()
        info.myNodeNum = Self.myNodeNum
        h.transport.inject(try frame { $0.myInfo = info })
        h.transport.inject(try frame { $0.configCompleteID = MeshtasticConfigNonce.onlyConfig })
        try await waitForSentCount(3, on: h.transport)
        h.transport.inject(try frame { $0.configCompleteID = MeshtasticConfigNonce.onlyNodeDB })
        try await connectTask.value

        h.graph.crewMembership.configure(crew: Self.crew)
        h.graph.crewMembership.applyChannelTable([crewChannel(at: 0)])
        XCTAssertEqual(h.graph.crewMembership.channelStatus, .resolved(index: 0))
        XCTAssertFalse(isPaired(h, Self.sender), "the sender must start as a total stranger")
        return h
    }

    private func isPaired(_ h: Harness, _ nodeID: UInt32) -> Bool {
        h.graph.core.crew.member(nodeID: nodeID, now: FireflyClock.nowMillis())?.paired == true
    }

    private func crewThread(_ h: Harness) -> [FeedMessage] {
        h.graph.inboxProvider.thread(for: .crew, now: Date())
    }

    /// Polls a synchronous predicate. 200 × 5 ms = 1 s ceiling, the
    /// package-wide rule. A test here only ever spends the full second
    /// when the ordering is genuinely broken — which is exactly the
    /// case this file exists to fail on.
    private func waitUntil(_ condition: @escaping () -> Bool, timeout: Int = 200) async {
        for _ in 0..<timeout where !condition() {
            try? await Task.sleep(nanoseconds: 5_000_000)
        }
    }

    /// Gives the graph's inbound loop a real chance to run and finish,
    /// for the NEGATIVE tests — where the correct outcome is that
    /// nothing happens, and "nothing happened yet" would pass for the
    /// wrong reason. Waits out a full ceiling rather than a single hop.
    private func settle() async {
        for _ in 0..<40 { try? await Task.sleep(nanoseconds: 5_000_000) }
    }

    private func teardown(_ h: Harness) async {
        await h.graph.stop()
        await h.client.disconnect()
    }

    // MARK: - (1) FLARE — the bench packet

    func testFirstEverFlareOnTheCrewChannelAdmitsItsSenderAndIsShown() async throws {
        let h = try await connectedCrewHarness()

        h.transport.inject(try fireflyFrame(.flare(durationS: 300), from: Self.sender,
                                            channel: Self.crewIndex))

        // The feed item is the observable proof the gate PASSED: it is
        // pushed by `handleInboundFlare` only after `isPairedSender`.
        await waitUntil { self.crewThread(h).contains { $0.kind == .flare } }

        XCTAssertTrue(isPaired(h, Self.sender),
                      "A02 §4.1: a decrypted portnum-269 packet on the crew channel admits its sender")
        XCTAssertEqual(h.graph.crewMembership.admissionCounters.admitted, 1)
        let flares = crewThread(h).filter { $0.kind == .flare }
        XCTAssertEqual(flares.count, 1, "the FLARE was not dropped")
        XCTAssertEqual(flares.first?.senderID, Self.sender)
        // Backgrounded (`isForegrounded` is false until a scene says
        // otherwise), so A03's notification is the render — the same
        // either/or `handleInboundFlare` itself branches on. Waited for
        // separately because `post(_:)` is fired off as its own `Task`
        // (`AppGraph+M2Protocol.post(_:)`), unlike the feed row above
        // which is pushed synchronously: this second wait cannot mask
        // the bug, because the row already proved the gate passed.
        await waitUntil { h.notifications.flareCalls.count == 1 }
        XCTAssertEqual(h.notifications.flareCalls.count, 1)
        // And the whole point of the wrapper: none of this needed the
        // node stream to have been delivered at all.
        XCTAssertGreaterThan(h.client.parkedCount, 0,
                             "the node stream was still stalled when the FLARE was rendered")

        await teardown(h)
    }

    // MARK: - (2) TEXT — admitted and persisted

    func testFirstEverCrewTextAdmitsItsSenderAndIsPersisted() async throws {
        let h = try await connectedCrewHarness()

        h.transport.inject(try packetFrame(from: Self.sender, channel: Self.crewIndex,
                                           portnum: .textMessageApp,
                                           payload: Data("at the ferris wheel".utf8)))

        await waitUntil { self.isPaired(h, Self.sender) && !self.crewThread(h).isEmpty }

        XCTAssertTrue(isPaired(h, Self.sender))
        let texts = crewThread(h).filter { $0.kind == .text }
        XCTAssertEqual(texts.count, 1)
        XCTAssertEqual(texts.first?.text, "at the ferris wheel")
        XCTAssertEqual(texts.first?.senderID, Self.sender)
        XCTAssertGreaterThan(h.client.parkedCount, 0)

        await teardown(h)
    }

    // MARK: - (3) RALLY

    func testFirstEverRallyOnTheCrewChannelAdmitsItsSenderAndIsShown() async throws {
        let h = try await connectedCrewHarness()

        h.transport.inject(try fireflyFrame(.rally(latitude: 39.0997, longitude: -84.5124, name: "MY SPOT"),
                                            from: Self.sender, channel: Self.crewIndex))

        await waitUntil { self.crewThread(h).contains { $0.kind == .rally } }

        XCTAssertTrue(isPaired(h, Self.sender))
        let rallies = crewThread(h).filter { $0.kind == .rally }
        XCTAssertEqual(rallies.count, 1)
        // No fix of our own (`UnavailableLocationProvider`), so the row
        // is the bare name — never a fabricated distance.
        XCTAssertEqual(rallies.first?.text, "MY SPOT")
        XCTAssertGreaterThan(h.client.parkedCount, 0)

        await teardown(h)
    }

    // MARK: - The gate is not weakened

    /// §4.1 clause 4. A hidden node's FLARE admits nobody and renders
    /// nothing — the ordering fix must not have turned "apply the
    /// packet's facts first" into "admit from the packet".
    func testHiddenSenderIsStillDroppedOnTheCrewChannel() async throws {
        let h = try await connectedCrewHarness()
        h.graph.crewMembership.hide(nodeID: Self.sender)

        h.transport.inject(try fireflyFrame(.flare(durationS: 300), from: Self.sender,
                                            channel: Self.crewIndex))
        await settle()

        XCTAssertFalse(isPaired(h, Self.sender))
        XCTAssertTrue(crewThread(h).isEmpty, "a hidden sender's FLARE renders nothing")
        XCTAssertTrue(h.notifications.flareCalls.isEmpty)
        XCTAssertEqual(h.graph.crewMembership.admissionCounters.admitted, 0)
        XCTAssertGreaterThan(h.graph.crewMembership.admissionCounters.refusedHidden, 0)

        await teardown(h)
    }

    /// §4.2. A FLARE decrypted on some OTHER channel index is not crew
    /// traffic, admits nobody, and renders nothing.
    func testFlareOnANonCrewChannelIsStillDropped() async throws {
        let h = try await connectedCrewHarness()

        h.transport.inject(try fireflyFrame(.flare(durationS: 300), from: Self.sender,
                                            channel: Self.crewIndex + 1))
        await settle()

        XCTAssertFalse(isPaired(h, Self.sender))
        XCTAssertTrue(crewThread(h).isEmpty)
        XCTAssertTrue(h.notifications.flareCalls.isEmpty)
        XCTAssertEqual(h.graph.crewMembership.admissionCounters.admitted, 0)
        XCTAssertGreaterThan(h.graph.crewMembership.admissionCounters.refusedWrongChannel, 0)

        await teardown(h)
    }

    /// §4.1 clause 5. Same packet, same channel, `via_mqtt` — a crew is
    /// people who are HERE.
    func testViaMQTTFlareOnTheCrewChannelIsStillDropped() async throws {
        let h = try await connectedCrewHarness()

        let payload = try XCTUnwrap(FireflyPacket.flare(durationS: 300).encode())
        var pkt = MeshPacket()
        pkt.from = Self.sender
        pkt.to = meshBroadcastAddress
        pkt.channel = Self.crewIndex
        pkt.id = 4243
        pkt.viaMqtt = true
        var data = DataMessage()
        data.portnum = PortNum(rawValue: 269) ?? .privateApp
        data.payload = payload
        pkt.decoded = data
        h.transport.inject(try frame { $0.packet = pkt })
        await settle()

        XCTAssertFalse(isPaired(h, Self.sender))
        XCTAssertTrue(crewThread(h).isEmpty)
        XCTAssertEqual(h.graph.crewMembership.admissionCounters.admitted, 0)
        XCTAssertGreaterThan(h.graph.crewMembership.admissionCounters.refusedViaMQTT, 0)

        await teardown(h)
    }
}
