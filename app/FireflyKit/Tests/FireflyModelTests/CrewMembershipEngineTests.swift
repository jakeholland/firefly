//
//  CrewMembershipEngineTests.swift — A02 slice C's acceptance criteria
//  (docs/specs/A02-crew-join.md §7, AC11–AC17), driven as scripted
//  `FromRadio` bytes through `LoopbackTransport` + `MeshtasticClient`
//  into the real `CoreStore` gate and the real `ff_crew` roster.
//
//  DETERMINISM, deliberately, with no sleeps anywhere:
//   * the handshake waits on `LoopbackTransport.waitForSentCount`,
//     which is continuation-backed rather than polling;
//   * every assertion is made after draining `nodeUpdates()` up to a
//     SENTINEL packet injected last — so a negative test ("this admits
//     nobody") never has to wait out a timeout to prove an absence, and
//     a change in how many events a packet produces makes the test hang
//     visibly rather than pass by luck.
//  The sentinel itself is injected on a non-crew channel index, so it
//  can never be the thing that admits anybody.
//
import FireflyMesh
import FireflyModel
import MeshtasticProto
import XCTest

@MainActor
final class CrewMembershipEngineTests: XCTestCase {

    // MARK: - Fixtures

    private static let myNodeNum: UInt32 = 48_621_524
    private static let crewIndex: UInt32 = 0
    private static let otherIndex: UInt32 = 5
    private static let sentinelID: UInt32 = 0xFEED_FACE
    private static let sentinelName = "sentinel"

    /// A code and a key. Slice A mints these for real (`CrewCode`); this
    /// slice only ever compares them, so a fixed pair is enough and
    /// nothing here depends on that slice landing first.
    private static let crew = CrewChannelIdentity(
        code: "FIRE-4K9M7X",
        psk: Data((0..<32).map { UInt8($0 &+ 1) }))
    private static let otherPSK = Data((0..<32).map { UInt8(0xFF &- $0) })

    private struct Harness {
        let transport: LoopbackTransport
        let client: MeshtasticClient
        let core: CoreStore
        let pairing: CrewPairingController
        let pairingStore: InMemoryCrewPairingStore
        let localState: InMemoryCrewLocalStateStore
        let engine: CrewMembershipEngine
        let stream: AsyncStream<MeshNodeSnapshot>
    }

    /// A clock that advances one second per read — so two admissions in
    /// the same test are ordered by construction rather than by how fast
    /// the machine happened to be. Sendable box, because the engine
    /// takes a `@Sendable` closure.
    private final class SteppingClock: @unchecked Sendable {
        private let lock = NSLock()
        private var step = 0
        func next() -> Date {
            lock.lock(); defer { lock.unlock() }
            step += 1
            return Date(timeIntervalSince1970: 1_780_000_000 + TimeInterval(step))
        }
    }

    private func makeHarness(pairingStore: InMemoryCrewPairingStore = InMemoryCrewPairingStore(),
                             localState: InMemoryCrewLocalStateStore = InMemoryCrewLocalStateStore(),
                             clock: SteppingClock? = nil)
        -> Harness {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        let core = CoreStore()
        let pairing = CrewPairingController(crew: core.crew, store: pairingStore)
        let engine = clock.map {
            CrewMembershipEngine(pairing: pairing, store: localState, client: client, now: $0.next)
        } ?? CrewMembershipEngine(pairing: pairing, store: localState, client: client)
        core.membership = engine
        return Harness(transport: transport, client: client, core: core, pairing: pairing,
                       pairingStore: pairingStore, localState: localState, engine: engine,
                       stream: client.nodeUpdates())
    }

    /// The crew channel, occupying `index` on this radio.
    private func crewChannel(at index: Int32, name: String = CrewMembershipEngineTests.crew.code,
                             psk: Data = CrewMembershipEngineTests.crew.psk) -> Channel {
        var settings = ChannelSettings()
        settings.name = name
        settings.psk = psk
        var channel = Channel()
        channel.index = index
        channel.settings = settings
        channel.role = index == 0 ? .primary : .secondary
        return channel
    }

    private func frame(_ build: (inout FromRadio) -> Void) throws -> Data {
        var fr = FromRadio()
        build(&fr)
        return try fr.serializedData()
    }

    private func packetFrame(from: UInt32, channel: UInt32, portnum: PortNum, payload: Data,
                             viaMqtt: Bool = false) throws -> Data {
        var pkt = MeshPacket()
        pkt.from = from
        pkt.to = meshBroadcastAddress
        pkt.channel = channel
        pkt.viaMqtt = viaMqtt
        var data = DataMessage()
        data.portnum = portnum
        data.payload = payload
        pkt.decoded = data
        return try frame { $0.packet = pkt }
    }

    /// Firefly's own portnum 269 — NOT `PRIVATE_APP` (256). Built by raw
    /// value, exactly as it arrives on the wire (A02 §4.1 clause 6).
    private func fireflyPortnum() -> PortNum {
        PortNum(rawValue: Int(fireflyPrivatePortNum)) ?? .privateApp
    }

    private func userPayload(long: String, short: String = "") throws -> Data {
        var user = User()
        user.longName = long
        user.shortName = short
        return try user.serializedData()
    }

    private func completeHandshake(_ h: Harness, replayNodes: [NodeInfo] = []) async throws {
        let connectTask = Task { try await h.client.connect() }
        try await h.transport.waitForSentCount(2)
        var info = MyNodeInfo()
        info.myNodeNum = Self.myNodeNum
        h.transport.inject(try frame { $0.myInfo = info })
        h.transport.inject(try frame { $0.configCompleteID = MeshtasticConfigNonce.onlyConfig })
        try await h.transport.waitForSentCount(3)
        for node in replayNodes {
            h.transport.inject(try frame { $0.nodeInfo = node })
        }
        h.transport.inject(try frame { $0.configCompleteID = MeshtasticConfigNonce.onlyNodeDB })
        try await connectTask.value
    }

    /// Drains every snapshot published so far into the gate, stopping at
    /// the sentinel. See this file's header comment.
    private func pump(_ h: Harness) async throws {
        h.transport.inject(try packetFrame(from: Self.sentinelID, channel: Self.otherIndex,
                                           portnum: .nodeinfoApp,
                                           payload: try userPayload(long: Self.sentinelName)))
        for await snapshot in h.stream {
            h.core.apply(nodeUpdate: snapshot)
            if snapshot.num == Self.sentinelID, snapshot.longName == Self.sentinelName { break }
        }
    }

    private func isPaired(_ h: Harness, _ nodeID: UInt32) -> Bool {
        h.core.crew.member(nodeID: nodeID, now: FireflyClock.nowMillis())?.paired == true
    }

    /// A harness that is connected, on a crew, with the crew resolved to
    /// index 0 — the ordinary case every admission test starts from.
    private func connectedCrewHarness(pairingStore: InMemoryCrewPairingStore = InMemoryCrewPairingStore(),
                                      localState: InMemoryCrewLocalStateStore = InMemoryCrewLocalStateStore())
        async throws -> Harness {
        let h = makeHarness(pairingStore: pairingStore, localState: localState)
        try await completeHandshake(h)
        h.engine.configure(crew: Self.crew)
        h.engine.applyChannelTable([crewChannel(at: 0)])
        XCTAssertEqual(h.engine.channelStatus, .resolved(index: 0))
        return h
    }

    // MARK: - A02_AC11 — the four portnums admit

    func testA02_AC11_NodeInfoOnTheCrewIndexAdmitsAndNamesTheMember() async throws {
        let h = try await connectedCrewHarness()

        h.transport.inject(try packetFrame(from: 1001, channel: Self.crewIndex, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Deshawn", short: "DSH")))
        try await pump(h)

        XCTAssertTrue(isPaired(h, 1001), "possession of the crew key IS membership")
        let member = h.core.crew.member(nodeID: 1001, now: FireflyClock.nowMillis())
        XCTAssertEqual(member?.longName, "Deshawn", "NodeInfo names the member it admitted")
        XCTAssertEqual(member?.shortName, "DSH")
        XCTAssertEqual(h.pairingStore.records().map(\.nodeID), [1001],
                        "a CrewPairingRecord is persisted, without any user action")
        XCTAssertEqual(h.pairingStore.records().first?.colorIndex, 0, "the next free colour")
        XCTAssertEqual(h.engine.joinedSinceCreated.map(\.nodeID), [1001])
    }

    func testA02_AC11_PositionOnTheCrewIndexAdmits() async throws {
        let h = try await connectedCrewHarness()
        var position = Position()
        position.latitudeI = 477_081_350
        position.longitudeI = -1_222_820_993
        position.locationSource = .locInternal

        h.transport.inject(try packetFrame(from: 1002, channel: Self.crewIndex, portnum: .positionApp,
                                           payload: try position.serializedData()))
        try await pump(h)

        XCTAssertTrue(isPaired(h, 1002))
    }

    func testA02_AC11_TextOnTheCrewIndexAdmits() async throws {
        let h = try await connectedCrewHarness()

        h.transport.inject(try packetFrame(from: 1003, channel: Self.crewIndex, portnum: .textMessageApp,
                                           payload: Data("where are you".utf8)))
        try await pump(h)

        XCTAssertTrue(isPaired(h, 1003))
    }

    /// 269 by RAW VALUE. Matching `PortNum.privateApp` (256) instead
    /// would silently admit nobody — A02 §4.1 clause 6.
    func testA02_AC11_FireflyPrivatePortnum269OnTheCrewIndexAdmits() async throws {
        let h = try await connectedCrewHarness()

        h.transport.inject(try packetFrame(from: 1004, channel: Self.crewIndex, portnum: fireflyPortnum(),
                                           payload: Data([0x01, 0x02, 0x03])))
        try await pump(h)

        XCTAssertTrue(isPaired(h, 1004))
        XCTAssertTrue(CrewMembershipEngine.admittingPortnums.contains(269))
        XCTAssertFalse(CrewMembershipEngine.admittingPortnums.contains(256),
                        "PRIVATE_APP is 256 and is not Firefly's portnum")
    }

    /// The member is admitted before their NodeInfo arrives, and is
    /// nameless — honestly, not with a fabricated name written into the
    /// model. §4.4's fallback is a DISPLAY rule.
    func testA02_AC11_AMemberAdmittedByTextHasNoNameInTheModelUntilNodeInfoArrives() async throws {
        let h = try await connectedCrewHarness()

        h.transport.inject(try packetFrame(from: 1005, channel: Self.crewIndex, portnum: .textMessageApp,
                                           payload: Data("hi".utf8)))
        try await pump(h)

        let member = h.core.crew.member(nodeID: 1005, now: FireflyClock.nowMillis())
        XCTAssertEqual(member?.longName, "", "no name is invented at admission time")
        XCTAssertEqual(CrewMembershipEngine.displayName(nickname: nil, longName: nil, shortName: nil),
                        "New crew member")

        h.transport.inject(try packetFrame(from: 1005, channel: Self.crewIndex, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Maya", short: "MAY")))
        try await pump(h)

        XCTAssertEqual(h.core.crew.member(nodeID: 1005, now: FireflyClock.nowMillis())?.longName, "Maya",
                        "the name updates in place")
        XCTAssertEqual(h.pairingStore.records().first?.colorIndex, 0,
                        "the colour does not change — assigned once at admission")
    }

    // MARK: - A02_AC12 — what does not admit anyone (one test each)

    func testA02_AC12_AnotherChannelIndexAdmitsNobody() async throws {
        let h = try await connectedCrewHarness()

        h.transport.inject(try packetFrame(from: 2001, channel: Self.otherIndex, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "LongFast Stranger")))
        try await pump(h)

        XCTAssertFalse(isPaired(h, 2001), "a friend chatting on LongFast is not crew")
        XCTAssertEqual(h.core.crew.count, 0, "and is not fed into the roster at all")
    }

    func testA02_AC12_ViaMQTTAdmitsNobody() async throws {
        let h = try await connectedCrewHarness()

        h.transport.inject(try packetFrame(from: 2002, channel: Self.crewIndex, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Bridged"), viaMqtt: true))
        try await pump(h)

        XCTAssertFalse(isPaired(h, 2002), "a crew is people who are here; an MQTT path can replay")
        XCTAssertEqual(h.core.crew.count, 0)
    }

    func testA02_AC12_TelemetryAdmitsNobody() async throws {
        let h = try await connectedCrewHarness()

        h.transport.inject(try packetFrame(from: 2003, channel: Self.crewIndex, portnum: .telemetryApp,
                                           payload: Data([0x08, 0x01])))
        try await pump(h)

        XCTAssertFalse(isPaired(h, 2003),
                        "telemetry refreshes an existing member's presence but never admits a new one")
        XCTAssertEqual(h.core.crew.count, 0)
    }

    func testA02_AC12_APacketFromOurOwnNodeNumAdmitsNobody() async throws {
        let h = try await connectedCrewHarness()

        h.transport.inject(try packetFrame(from: Self.myNodeNum, channel: Self.crewIndex,
                                           portnum: .nodeinfoApp, payload: try userPayload(long: "Me")))
        try await pump(h)

        XCTAssertFalse(isPaired(h, Self.myNodeNum))
        XCTAssertEqual(h.core.crew.count, 0, "we are not our own crew member")
    }

    func testA02_AC12_AHiddenIdAdmitsNobodyAndStaysHiddenAfterReHearing() async throws {
        let h = try await connectedCrewHarness()

        h.transport.inject(try packetFrame(from: 2005, channel: Self.crewIndex, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Straggler")))
        try await pump(h)
        XCTAssertTrue(isPaired(h, 2005))

        h.engine.hide(nodeID: 2005)
        XCTAssertFalse(isPaired(h, 2005), "hide = unpair + remember")
        XCTAssertTrue(h.engine.isHidden(2005))

        // Re-heard, repeatedly, on exactly the packet that admitted them.
        for _ in 0..<3 {
            h.transport.inject(try packetFrame(from: 2005, channel: Self.crewIndex, portnum: .nodeinfoApp,
                                               payload: try userPayload(long: "Straggler")))
            try await pump(h)
        }

        XCTAssertFalse(isPaired(h, 2005), "the hide list is what stops silent re-admission")
        XCTAssertTrue(h.engine.isHidden(2005))
    }

    /// The want_config NodeInfo REPLAY. Not a live `MeshPacket`, so it
    /// carries no `MeshRxMeta` at all and is structurally unable to
    /// admit anyone — no separate guard exists, and that is the point.
    func testA02_AC12_TheWantConfigNodeInfoReplayAdmitsNobody() async throws {
        let h = makeHarness()
        var replayed = NodeInfo()
        replayed.num = 2006
        var user = User()
        user.longName = "Replayed"
        replayed.user = user
        replayed.channel = 0 // "only populated if its not the default channel" — useless here
        try await completeHandshake(h, replayNodes: [replayed])
        h.engine.configure(crew: Self.crew)
        h.engine.applyChannelTable([crewChannel(at: 0)])

        try await pump(h)

        XCTAssertFalse(isPaired(h, 2006), "a replay is a summary, not evidence")
        XCTAssertEqual(h.core.crew.count, 0)

        // The discriminating half: the SAME id, on the SAME channel,
        // arriving as a live packet instead, IS admitted. Without this
        // the test would also pass against a gate that admitted nobody
        // at all (docs/review/code-review.md item 6).
        h.transport.inject(try packetFrame(from: 2006, channel: Self.crewIndex, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Replayed")))
        try await pump(h)
        XCTAssertTrue(isPaired(h, 2006), "live traffic from the same id does admit")
    }

    // MARK: - A02_AC13 — the membership gate in front of CoreStore

    func testA02_AC13_AReplayedNodeDBOf200StrangersLeavesTheRosterUntouched() async throws {
        let h = makeHarness()
        let strangers: [NodeInfo] = (0..<200).map { i in
            var node = NodeInfo()
            node.num = 30_000 + UInt32(i)
            var user = User()
            user.longName = "Stranger \(i)"
            user.shortName = "S\(i)"
            node.user = user
            node.lastHeard = UInt32(Date().addingTimeInterval(-60).timeIntervalSince1970)
            return node
        }
        try await completeHandshake(h, replayNodes: strangers)
        h.engine.configure(crew: Self.crew)
        h.engine.applyChannelTable([crewChannel(at: 0)])

        try await pump(h)

        XCTAssertEqual(h.core.crew.count, 0, "issue #266's remaining app-side live exposure")
        XCTAssertTrue(h.pairingStore.records().isEmpty)

        // Same discriminating half as the replay test above: one of
        // those 200, heard LIVE on the crew channel, is admitted — so
        // this cannot pass by the gate simply refusing everyone.
        h.transport.inject(try packetFrame(from: 30_007, channel: Self.crewIndex, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Stranger 7")))
        try await pump(h)
        XCTAssertEqual(h.core.crew.count, 1)
        XCTAssertTrue(isPaired(h, 30_007))
    }

    /// The four `crew.*` conditions INSIDE `apply(nodeUpdate:)` are
    /// unchanged, so the gate cannot be "fixed" by loosening them: a
    /// crew member whose position claims a future timestamp, on a
    /// snapshot with no `observedAt` (a replay), is still not fed.
    func testA02_AC13_TheFreshnessConditionsInsideApplyAreUnchanged() async throws {
        let h = try await connectedCrewHarness()
        h.pairing.pair(nodeID: 4001)
        XCTAssertTrue(isPaired(h, 4001))

        h.core.apply(nodeUpdate: MeshNodeSnapshot(
            num: 4001, shortName: "FF", longName: "Future Clock",
            position: NodePosition(latitude: 1, longitude: 2,
                                    time: Date().addingTimeInterval(3600),
                                    source: .internalGPS, precisionBits: 32),
            lastHeard: nil, rssiDbm: nil, snrDb: nil, hopsAway: nil,
            observedAt: nil))

        let member = h.core.crew.member(nodeID: 4001, now: FireflyClock.nowMillis())
        XCTAssertEqual(member?.longName, "Future Clock", "identity still lands — the member IS crew")
        XCTAssertNil(member?.position,
                      "a coordinate with no honest place on a freshness axis is not fed, gate or no gate")
    }

    // MARK: - A02_AC14 — index resolution by name AND PSK

    func testA02_AC14_TheCrewIndexIsResolvedByNameAndPSKNotAssumedToBeZero() async throws {
        let h = makeHarness()
        try await completeHandshake(h)
        h.engine.configure(crew: Self.crew)
        // A radio provisioned by CLI: the crew sits at index 2, with a
        // stock primary at 0.
        var stockPrimary = ChannelSettings()
        stockPrimary.name = ""
        var primary = Channel()
        primary.index = 0
        primary.settings = stockPrimary
        primary.role = .primary
        h.engine.applyChannelTable([primary, crewChannel(at: 2)])

        XCTAssertEqual(h.engine.channelStatus, .resolved(index: 2))

        h.transport.inject(try packetFrame(from: 5001, channel: 2, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "At Index Two")))
        h.transport.inject(try packetFrame(from: 5002, channel: 0, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "On The Public Primary")))
        try await pump(h)

        XCTAssertTrue(isPaired(h, 5001))
        XCTAssertFalse(isPaired(h, 5002), "index 0 is NOT the crew on this radio")
    }

    func testA02_AC14_AMatchingNameWithTheWrongPSKIsNotTheCrewChannel() async throws {
        let h = makeHarness()
        try await completeHandshake(h)
        h.engine.configure(crew: Self.crew)
        h.engine.applyChannelTable([crewChannel(at: 0, psk: Self.otherPSK)])

        XCTAssertEqual(h.engine.channelStatus, .notOnCrewChannel)

        h.transport.inject(try packetFrame(from: 5003, channel: 0, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Same Name, Other Key")))
        try await pump(h)

        XCTAssertFalse(isPaired(h, 5003))
        XCTAssertEqual(h.core.crew.count, 0, "never a fallback to index 0")
    }

    func testA02_AC14_AMatchingPSKWithTheWrongNameIsNotTheCrewChannel() async throws {
        let h = makeHarness()
        try await completeHandshake(h)
        h.engine.configure(crew: Self.crew)
        h.engine.applyChannelTable([crewChannel(at: 0, name: "FIRE-ZZZZZZ")])

        XCTAssertEqual(h.engine.channelStatus, .notOnCrewChannel)
    }

    func testA02_AC14_AnUnreadChannelTableIsResolvingNotNotOnCrewChannel() async throws {
        let h = makeHarness()
        try await completeHandshake(h)
        h.engine.configure(crew: Self.crew)

        XCTAssertEqual(h.engine.channelStatus, .resolving,
                        "'not read yet' and 'read, and it isn't there' are different honest answers")
        h.engine.applyChannelTable([])
        XCTAssertEqual(h.engine.channelStatus, .resolving)

        h.transport.inject(try packetFrame(from: 5004, channel: 0, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Too Early")))
        try await pump(h)
        XCTAssertFalse(isPaired(h, 5004), "an unresolved index admits nobody")
    }

    /// Driven over `StubMeshtasticClient` rather than the loopback
    /// client: this test is about the LINK edge and the channel-table
    /// read, and the stub's table is injectable, so the whole thing
    /// settles with `Task.yield()` and never a timer.
    func testA02_AC14_ReconnectingReResolvesTheIndexRatherThanReusingTheCachedOne() async throws {
        let stub = StubMeshtasticClient()
        stub.connectedNodeNum = Self.myNodeNum
        stub.channelTable = [crewChannel(at: 0)]
        let core = CoreStore()
        let engine = CrewMembershipEngine(pairing: CrewPairingController(crew: core.crew,
                                                                          store: InMemoryCrewPairingStore()),
                                           store: InMemoryCrewLocalStateStore(),
                                           client: stub)
        engine.configure(crew: Self.crew)
        engine.observe()

        try await stub.connect()
        await settle(until: { engine.channelStatus == .resolved(index: 0) },
                      "first connect should resolve the crew index")

        // A reconnect may be to a DIFFERENT radio, or to one somebody
        // reprovisioned in between. The cached index dies with the link.
        await stub.disconnect()
        stub.channelTable = [crewChannel(at: 3)]
        try await stub.connect()
        await settle(until: { engine.channelStatus == .resolved(index: 3) },
                      "the index is 'inherently a local concept' and is never carried across links")
    }

    /// Sleep-free settling: `Task.yield()` hands the main actor to the
    /// engine's own observation task, so this converges as soon as that
    /// task has run rather than after a wall-clock interval.
    private func settle(until condition: @MainActor () -> Bool, _ message: String) async {
        for _ in 0..<10_000 {
            if condition() { return }
            await Task.yield()
        }
        XCTFail(message)
    }

    func testA02_AC14_NoCrewCodeConfiguredMeansNoCrewAndNoAdmission() async throws {
        let h = makeHarness()
        try await completeHandshake(h)

        XCTAssertEqual(h.engine.channelStatus, .noCrew)
        h.transport.inject(try packetFrame(from: 5005, channel: 0, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Nobody's Crew")))
        try await pump(h)

        XCTAssertFalse(isPaired(h, 5005))
    }

    // MARK: - A02_AC15 — the cap, and what happens at nine

    func testA02_AC15_TheNinthJoinerIsListedAsUntrackedNotSilentlyDropped() async throws {
        let h = try await connectedCrewHarness()

        for i in 0..<9 {
            h.transport.inject(try packetFrame(from: 6000 + UInt32(i), channel: Self.crewIndex,
                                               portnum: .nodeinfoApp,
                                               payload: try userPayload(long: "Friend \(i)")))
        }
        try await pump(h)

        XCTAssertEqual(h.pairingStore.records().count, 8, "FF_CREW_MAX is 8 and stays 8")
        XCTAssertEqual(h.engine.untracked.map(\.nodeID), [6008],
                        "the 9th is listed honestly, not dropped")
        XCTAssertFalse(isPaired(h, 6008))
    }

    func testA02_AC15_HidingSomeoneFreesASlotAndTheUntrackedMemberIsAdmittedOnItsNextPacket() async throws {
        let h = try await connectedCrewHarness()
        for i in 0..<9 {
            h.transport.inject(try packetFrame(from: 6100 + UInt32(i), channel: Self.crewIndex,
                                               portnum: .nodeinfoApp,
                                               payload: try userPayload(long: "Friend \(i)")))
        }
        try await pump(h)
        XCTAssertEqual(h.engine.untracked.map(\.nodeID), [6108])

        h.engine.hide(nodeID: 6100)

        h.transport.inject(try packetFrame(from: 6108, channel: Self.crewIndex, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Friend 8")))
        try await pump(h)

        XCTAssertTrue(isPaired(h, 6108), "the freed slot is taken on the next qualifying packet")
        XCTAssertTrue(h.engine.untracked.isEmpty)
    }

    // MARK: - A02_AC16 — hide

    func testA02_AC16_HidePersistsPerCrewCodeAndSurvivesARelaunch() async throws {
        let pairingStore = InMemoryCrewPairingStore()
        let localState = InMemoryCrewLocalStateStore()
        let h = try await connectedCrewHarness(pairingStore: pairingStore, localState: localState)
        h.transport.inject(try packetFrame(from: 7001, channel: Self.crewIndex, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Straggler")))
        try await pump(h)
        h.engine.hide(nodeID: 7001)
        XCTAssertEqual(localState.hiddenIDs(crewCode: Self.crew.code), [7001])

        // "Relaunch": a fresh process, fresh ff_crew, same stores.
        let relaunched = makeHarness(pairingStore: pairingStore, localState: localState)
        try await completeHandshake(relaunched)
        relaunched.engine.configure(crew: Self.crew)
        relaunched.engine.applyChannelTable([crewChannel(at: 0)])
        XCTAssertTrue(relaunched.engine.isHidden(7001))

        relaunched.transport.inject(try packetFrame(from: 7001, channel: Self.crewIndex,
                                                    portnum: .nodeinfoApp,
                                                    payload: try userPayload(long: "Straggler")))
        try await pump(relaunched)
        XCTAssertFalse(isPaired(relaunched, 7001), "a hide survives the relaunch that forgot everything else")
    }

    func testA02_AC16_UnhideRestoresTheMemberOnTheirNextPacketAndNotBefore() async throws {
        let h = try await connectedCrewHarness()
        h.transport.inject(try packetFrame(from: 7002, channel: Self.crewIndex, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Straggler")))
        try await pump(h)
        h.engine.hide(nodeID: 7002)

        h.engine.unhide(nodeID: 7002)
        XCTAssertFalse(h.engine.isHidden(7002))
        XCTAssertFalse(isPaired(h, 7002),
                        "unhiding claims no presence nobody has observed since the hide")

        h.transport.inject(try packetFrame(from: 7002, channel: Self.crewIndex, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Straggler")))
        try await pump(h)
        XCTAssertTrue(isPaired(h, 7002))
    }

    func testA02_AC16_HidesAreKeptPerCrewCodeSoRejoiningRestoresThem() async throws {
        let localState = InMemoryCrewLocalStateStore()
        let h = try await connectedCrewHarness(localState: localState)
        h.transport.inject(try packetFrame(from: 7003, channel: Self.crewIndex, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Straggler")))
        try await pump(h)
        h.engine.hide(nodeID: 7003)

        let otherCrew = CrewChannelIdentity(code: "FIRE-ZZZZZZ", psk: Self.otherPSK)
        h.engine.configure(crew: otherCrew)
        XCTAssertFalse(h.engine.isHidden(7003), "a different crew has its own hides")

        h.engine.configure(crew: Self.crew)
        XCTAssertTrue(h.engine.isHidden(7003), "leaving and rejoining restores the hides you had")
    }

    // MARK: - A02_AC17 — migration of manually-added crew

    func testA02_AC17_ExistingManuallyPairedMembersKeepWorkingWithNoCrewCode() async throws {
        let pairingStore = InMemoryCrewPairingStore()
        pairingStore.upsert(CrewPairingRecord(nodeID: 8001, colorIndex: 3, nickname: "Old Friend"))
        let h = makeHarness(pairingStore: pairingStore)
        CrewPairingRestorer.restore(from: pairingStore, into: h.core.crew)
        try await completeHandshake(h)

        XCTAssertEqual(h.engine.channelStatus, .noCrew)
        h.transport.inject(try packetFrame(from: 8001, channel: Self.otherIndex, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Old Friend", short: "OF")))
        try await pump(h)

        XCTAssertTrue(isPaired(h, 8001), "nothing is removed on upgrade")
        XCTAssertEqual(h.core.crew.member(nodeID: 8001, now: FireflyClock.nowMillis())?.longName,
                        "Old Friend", "and their traffic still reaches the roster, on any channel")
        XCTAssertEqual(h.engine.members.map(\.origin), [.fromBefore])
    }

    func testA02_AC17_PreExistingMembersAreGrandfatheredAfterAJoinNotAutoRemoved() async throws {
        let pairingStore = InMemoryCrewPairingStore()
        pairingStore.upsert(CrewPairingRecord(nodeID: 8002, colorIndex: 1, nickname: nil))
        let h = makeHarness(pairingStore: pairingStore)
        CrewPairingRestorer.restore(from: pairingStore, into: h.core.crew)
        try await completeHandshake(h)

        h.engine.configure(crew: Self.crew)
        h.engine.applyChannelTable([crewChannel(at: 0)])
        h.transport.inject(try packetFrame(from: 8003, channel: Self.crewIndex, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "New Joiner")))
        try await pump(h)

        XCTAssertTrue(isPaired(h, 8002), "an unseen pre-existing member is never auto-removed")
        XCTAssertTrue(isPaired(h, 8003))
        let origins = Dictionary(uniqueKeysWithValues: h.engine.members.map { ($0.nodeID, $0.origin) })
        XCTAssertEqual(origins[8002], .fromBefore, "listed under 'From before'")
        if case .joinedCrew = origins[8003] {} else {
            XCTFail("8003 joined this crew and should read as such, got \(String(describing: origins[8003]))")
        }
        XCTAssertEqual(h.pairingStore.records().first { $0.nodeID == 8002 }?.colorIndex, 1,
                        "colours are preserved from the persisted record")
    }

    // MARK: - CrewMembershipProviding (slice B's seam)

    func testCurrentMembersReportsJoinersNewestFirstWithFromBeforeLast() async throws {
        let pairingStore = InMemoryCrewPairingStore()
        pairingStore.upsert(CrewPairingRecord(nodeID: 9001, colorIndex: 4, nickname: "Older Friend"))
        let h = makeHarness(pairingStore: pairingStore, clock: SteppingClock())
        CrewPairingRestorer.restore(from: pairingStore, into: h.core.crew)
        try await completeHandshake(h)
        h.engine.configure(crew: Self.crew)
        h.engine.applyChannelTable([crewChannel(at: 0)])

        h.transport.inject(try packetFrame(from: 9002, channel: Self.crewIndex, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "First Joiner")))
        try await pump(h)
        h.transport.inject(try packetFrame(from: 9003, channel: Self.crewIndex, portnum: .textMessageApp,
                                           payload: Data("hi".utf8)))
        try await pump(h)

        let rows = h.engine.currentMembers()
        XCTAssertEqual(rows.map(\.id), [9003, 9002, 9001],
                        "newest join first; the member with no observed join time sorts last")
        XCTAssertEqual(rows[0].displayName, nil,
                        "admitted by a text, no NodeInfo yet — nil, never a fabricated name or a hex id")
        XCTAssertEqual(rows[1].displayName, "First Joiner")
        XCTAssertEqual(rows[2].displayName, "Older Friend", "the local nickname wins")
        XCTAssertNil(rows[2].joinedAtMs, "this phone never observed them join")
        XCTAssertNotNil(rows[0].joinedAtMs)
        XCTAssertEqual(rows[2].colorIndex, 4, "the persisted colour is preserved")
    }

    func testCurrentMembersOmitsHiddenMembers() async throws {
        let h = try await connectedCrewHarness()
        h.transport.inject(try packetFrame(from: 9004, channel: Self.crewIndex, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Straggler")))
        try await pump(h)
        XCTAssertEqual(h.engine.currentMembers().map(\.id), [9004])

        h.engine.hide(nodeID: 9004)
        XCTAssertTrue(h.engine.currentMembers().isEmpty,
                       "hiding unpairs, so there is no separate filter to forget to apply")
    }

    /// PR #308 review. The presence words the Crew page and the Start
    /// screen render are age-carrying by rule
    /// (`PresenceTag.plainLabel(age:)`, PR #304: "6 min ago",
    /// "No signal \u{00B7} 40 min"); a row that arrives with no age
    /// silently falls back to the bare enum name ("STALE"). This engine
    /// is where that age enters the UI, so it is pinned here.
    func testCurrentMembersCarriesTheHeardAgeTheUIWordsRequire() async throws {
        let h = try await connectedCrewHarness()
        h.transport.inject(try packetFrame(from: 9005, channel: Self.crewIndex, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Freshly Heard")))
        try await pump(h)

        guard let row = h.engine.currentMembers().first(where: { $0.id == 9005 }) else {
            return XCTFail("9005 was not admitted")
        }
        // Just heard, so there IS an age and it must be present — the
        // label is built from it, not from the presence enum alone.
        XCTAssertEqual(row.heardPresence, .heard)
        guard let ageMs = row.heardAgeMs else {
            return XCTFail("a member heard this instant must carry an age, not nil")
        }
        XCTAssertLessThan(ageMs, PresenceTag.heardLiveMS,
                          "an age just observed cannot already be outside the HEARD window")
        // And the age actually reaches the shipped vocabulary rather
        // than being carried and ignored.
        let label = PresenceTag.heard.plainLabel(age: TimeInterval(ageMs) / 1000)
        XCTAssertNotEqual(label, PresenceTag.heard.rawValue,
                          "the label fell back to the bare enum word: \(label)")
    }

    // MARK: - §4.4's display-name order

    func testDisplayNameFallsBackInSpecOrderAndNeverToBlank() {
        XCTAssertEqual(CrewMembershipEngine.displayName(nickname: "Dee", longName: "Deshawn", shortName: "DSH"),
                        "Dee")
        XCTAssertEqual(CrewMembershipEngine.displayName(nickname: nil, longName: "Deshawn", shortName: "DSH"),
                        "Deshawn")
        XCTAssertEqual(CrewMembershipEngine.displayName(nickname: nil, longName: "", shortName: "DSH"), "DSH")
        XCTAssertEqual(CrewMembershipEngine.displayName(nickname: nil, longName: nil, shortName: nil),
                        "New crew member")
    }
}
