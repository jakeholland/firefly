//
//  CrewAdmissionHardeningTests.swift — A02 slice C, the adversarial half
//  of AC12/AC14 plus the per-packet attribution rule the slice's widened
//  rx-meta emission made load-bearing (PR #306 review).
//
//  Same determinism discipline as `CrewMembershipEngineTests`: scripted
//  `FromRadio` bytes through `LoopbackTransport` -> `MeshtasticClient`
//  -> the real gate -> the real `ff_crew`, every assertion made after
//  draining up to a sentinel packet injected last, no sleeps anywhere.
//
import FireflyMesh
import FireflyModel
import MeshtasticProto
import XCTest

@MainActor
final class CrewAdmissionHardeningTests: XCTestCase {

    private static let myNodeNum: UInt32 = 48_621_524
    private static let sentinelID: UInt32 = 0xFEED_FACE
    private static let sentinelName = "sentinel"
    private static let crew = CrewChannelIdentity(
        code: "FIRE-4K9M7X",
        psk: Data((0..<32).map { UInt8($0 &+ 1) }))
    private static let rotatedPSK = Data((0..<32).map { UInt8(0xFF &- $0) })

    private struct Harness {
        let transport: LoopbackTransport
        let client: MeshtasticClient
        let core: CoreStore
        let pairing: CrewPairingController
        let pairingStore: InMemoryCrewPairingStore
        let engine: CrewMembershipEngine
        let stream: AsyncStream<MeshNodeSnapshot>
    }

    private func makeHarness() -> Harness {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        let core = CoreStore()
        let pairingStore = InMemoryCrewPairingStore()
        let pairing = CrewPairingController(crew: core.crew, store: pairingStore)
        let engine = CrewMembershipEngine(pairing: pairing, store: InMemoryCrewLocalStateStore(),
                                           client: client)
        core.membership = engine
        return Harness(transport: transport, client: client, core: core, pairing: pairing,
                       pairingStore: pairingStore, engine: engine, stream: client.nodeUpdates())
    }

    private func crewChannel(at index: Int32, name: String = CrewAdmissionHardeningTests.crew.code,
                             psk: Data = CrewAdmissionHardeningTests.crew.psk) -> Channel {
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

    /// `channel: nil` writes no `MeshPacket.channel` field AT ALL —
    /// proto3 omits an explicit 0 anyway, so this is how an "unset"
    /// channel actually arrives, and the point of the test that uses it
    /// is that unset and 0 are the same bytes and must be treated alike.
    private func packetFrame(from: UInt32, channel: UInt32?, portnum: PortNum, payload: Data,
                             viaMqtt: Bool = false, hopStart: UInt32 = 0, hopLimit: UInt32 = 0,
                             rxRssi: Int32? = nil) throws -> Data {
        var pkt = MeshPacket()
        pkt.from = from
        pkt.to = meshBroadcastAddress
        if let channel { pkt.channel = channel }
        pkt.viaMqtt = viaMqtt
        pkt.hopStart = hopStart
        pkt.hopLimit = hopLimit
        if let rxRssi { pkt.rxRssi = rxRssi }
        var data = DataMessage()
        data.portnum = portnum
        data.payload = payload
        pkt.decoded = data
        return try frame { $0.packet = pkt }
    }

    private func userPayload(long: String, short: String = "", id: String = "") throws -> Data {
        var user = User()
        user.longName = long
        user.shortName = short
        if !id.isEmpty { user.id = id }
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
        for node in replayNodes { h.transport.inject(try frame { $0.nodeInfo = node }) }
        h.transport.inject(try frame { $0.configCompleteID = MeshtasticConfigNonce.onlyNodeDB })
        try await connectTask.value
    }

    private func pump(_ h: Harness) async throws {
        h.transport.inject(try packetFrame(from: Self.sentinelID, channel: 5, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: Self.sentinelName)))
        for await snapshot in h.stream {
            h.core.apply(nodeUpdate: snapshot)
            if snapshot.num == Self.sentinelID, snapshot.longName == Self.sentinelName { break }
        }
    }

    private func isPaired(_ h: Harness, _ nodeID: UInt32) -> Bool {
        h.core.crew.member(nodeID: nodeID, now: FireflyClock.nowMillis())?.paired == true
    }

    private func connectedCrewHarness(at index: Int32 = 0) async throws -> Harness {
        let h = makeHarness()
        try await completeHandshake(h)
        h.engine.configure(crew: Self.crew)
        h.engine.applyChannelTable([crewChannel(at: index)])
        return h
    }

    // MARK: - AC14 — the key is rotated on the radio after the crew was created

    /// Somebody re-provisions the radio (CLI, or the stock app) and the
    /// channel keeps its NAME but gets a new key. That is not our crew
    /// channel any more, and the honest consequence is to stop admitting
    /// — NOT to evict the members we already have, whose membership was
    /// proved by a packet that really did arrive on the old key.
    func testA02_AC14_ARotatedPSKStopsAdmissionWithoutEvictingExistingMembers() async throws {
        let h = try await connectedCrewHarness()
        h.transport.inject(try packetFrame(from: 1101, channel: 0, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Early Joiner")))
        try await pump(h)
        XCTAssertTrue(isPaired(h, 1101))

        h.engine.applyChannelTable([crewChannel(at: 0, psk: Self.rotatedPSK)])
        XCTAssertEqual(h.engine.channelStatus, .notOnCrewChannel,
                        "a name match with a rotated key is not this crew's channel")

        h.transport.inject(try packetFrame(from: 1102, channel: 0, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Stranger On The New Key")))
        h.transport.inject(try packetFrame(from: 1101, channel: 0, portnum: .textMessageApp,
                                           payload: Data("still here".utf8)))
        try await pump(h)

        XCTAssertFalse(isPaired(h, 1102), "nobody is admitted while the crew channel does not resolve")
        XCTAssertTrue(isPaired(h, 1101), "and an existing member is not evicted by a failed resolve")
    }

    /// proto3 drops zero values, so an omitted `MeshPacket.channel` is
    /// byte-identical to an explicit 0 — which is why "never assume 0"
    /// has to mean "never admit on 0 unless 0 is where the crew
    /// resolved", not "treat 0 as absent".
    func testA02_AC14_AnOmittedChannelFieldIsIndexZeroAndAdmitsNobodyWhenTheCrewIsElsewhere() async throws {
        let h = try await connectedCrewHarness(at: 2)
        XCTAssertEqual(h.engine.channelStatus, .resolved(index: 2))

        h.transport.inject(try packetFrame(from: 1201, channel: nil, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Implicit Zero")))
        try await pump(h)
        XCTAssertFalse(isPaired(h, 1201), "an omitted channel field is index 0, and 0 is not the crew here")

        h.transport.inject(try packetFrame(from: 1202, channel: 2, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Actually On Two")))
        try await pump(h)
        XCTAssertTrue(isPaired(h, 1202), "the discriminating half: index 2 does admit")
    }

    // MARK: - AC12 — `User.id` is a claim, `MeshPacket.from` is the identity

    func testA02_AC12_TheUserPayloadsSelfDeclaredIDCannotImpersonateAnybody() async throws {
        let h = try await connectedCrewHarness()

        // A stranger whose NodeInfo payload claims to be OUR radio.
        h.transport.inject(try packetFrame(from: 1301, channel: 0, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Not You",
                                                                     id: String(format: "!%08x", Self.myNodeNum))))
        try await pump(h)
        XCTAssertTrue(isPaired(h, 1301), "the packet's `from` is the identity, never the payload's claim")
        XCTAssertFalse(isPaired(h, Self.myNodeNum), "and the claimed id gets no roster slot of its own")

        // Our own radio's NodeInfo, claiming in the payload to be 1337.
        h.transport.inject(try packetFrame(from: Self.myNodeNum, channel: 0, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Me", id: "!00000539")))
        try await pump(h)
        XCTAssertFalse(isPaired(h, Self.myNodeNum), "we are never admitted as our own crew member")
        XCTAssertFalse(isPaired(h, 1337), "and a `from`-less claim admits nobody either")
    }

    // MARK: - Per-packet attribution (PR #306 review)

    /// The slice widened `applyRxMeta` to publish a snapshot for EVERY
    /// packet naming a sender, which is what lets a text or a 269 admit
    /// a joiner. The snapshot carries the node's EXISTING record, so
    /// `hopsAway`/`rssiDbm` on it are the nodeDB's latched summary of an
    /// EARLIER hearing — and attributing those to a bridged or relayed
    /// packet renders somebody on the far side of an MQTT gateway as
    /// "standing next to you", with the reading's age re-stamped to
    /// zero. Attribution rides on THIS packet's own path and reading,
    /// the rule `ff_shell.c`'s `shell_ev_rx_meta` has always applied.
    func testAMemberSRelayedAndMQTTPacketsDoNotRefreshTheirDirectSignal() async throws {
        var replayed = NodeInfo()
        replayed.num = 1501
        var user = User()
        user.longName = "Member"
        replayed.user = user
        replayed.hopsAway = 0   // the nodeDB remembers an earlier DIRECT hearing
        let h = makeHarness()
        try await completeHandshake(h, replayNodes: [replayed])
        h.engine.configure(crew: Self.crew)
        h.engine.applyChannelTable([crewChannel(at: 0)])
        h.pairing.pair(nodeID: 1501)

        // One genuinely direct packet, with a strong reading.
        h.transport.inject(try packetFrame(from: 1501, channel: 0, portnum: .textMessageApp,
                                           payload: Data("close".utf8),
                                           hopStart: 3, hopLimit: 3, rxRssi: -42))
        try await pump(h)
        let direct = h.core.crew.member(nodeID: 1501, now: FireflyClock.nowMillis())
        XCTAssertEqual(direct?.directSignal?.rssiDbm, -42)
        XCTAssertEqual(direct?.heardDirect, true)

        // The SAME member, now over MQTT — our radio measured nothing.
        h.transport.inject(try packetFrame(from: 1501, channel: 0, portnum: .textMessageApp,
                                           payload: Data("bridged".utf8), viaMqtt: true))
        try await pump(h)
        XCTAssertEqual(h.core.crew.member(nodeID: 1501, now: FireflyClock.nowMillis())?.heardDirect, false,
                        "an MQTT path is not 'standing next to you'")

        // And over two hops, carrying a reading of its own — which
        // measures the RELAY, not this member, and must never enter
        // their signal history.
        h.transport.inject(try packetFrame(from: 1501, channel: 0, portnum: .textMessageApp,
                                           payload: Data("relayed".utf8),
                                           hopStart: 3, hopLimit: 1, rxRssi: -99))
        try await pump(h)
        let relayed = h.core.crew.member(nodeID: 1501, now: FireflyClock.nowMillis())
        XCTAssertEqual(relayed?.heardDirect, false, "RSSI on a relayed packet measures the relay")
        XCTAssertEqual(relayed?.directSignal?.rssiDbm, -42,
                        "the relay's own reading is not folded into this member's signal history")
    }

    /// The live NodeInfo decode this slice adds rebuilds a `NodeInfo`
    /// wrapper that carries no `hops_away`, which clears the nodeDB's
    /// latched one. That must not cost a member their direct-signal
    /// attribution for the rest of the session — with the path read off
    /// each packet, it cannot.
    func testALiveNodeInfoDoesNotCostAMemberTheirDirectSignal() async throws {
        var replayed = NodeInfo()
        replayed.num = 1601
        var user = User()
        user.longName = "Member"
        replayed.user = user
        replayed.hopsAway = 0
        let h = makeHarness()
        try await completeHandshake(h, replayNodes: [replayed])
        h.engine.configure(crew: Self.crew)
        h.engine.applyChannelTable([crewChannel(at: 0)])
        h.pairing.pair(nodeID: 1601)

        h.transport.inject(try packetFrame(from: 1601, channel: 0, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Member", short: "MB"),
                                           hopStart: 3, hopLimit: 3, rxRssi: -55))
        try await pump(h)
        h.transport.inject(try packetFrame(from: 1601, channel: 0, portnum: .textMessageApp,
                                           payload: Data("hi again".utf8),
                                           hopStart: 3, hopLimit: 3, rxRssi: -57))
        try await pump(h)

        let member = h.core.crew.member(nodeID: 1601, now: FireflyClock.nowMillis())
        XCTAssertEqual(member?.heardDirect, true,
                        "a direct packet after a live NodeInfo is still direct")
        XCTAssertNotNil(member?.directSignal, "and its reading is still attributable")
    }

    // MARK: - §4.3 — the untracked list stays bounded and ordered

    func testA02_AC15_TheUntrackedListIsBoundedAndDeterministicallyOrdered() async throws {
        let h = try await connectedCrewHarness()
        for i in 0..<100 {
            h.transport.inject(try packetFrame(from: 7000 + UInt32(i), channel: 0, portnum: .nodeinfoApp,
                                               payload: try userPayload(long: "F\(i)")))
        }
        try await pump(h)

        XCTAssertEqual(h.pairingStore.records().count, 8, "FF_CREW_MAX is 8 and stays 8")
        XCTAssertEqual(h.engine.untracked.count, CrewMembershipEngine.maxUntrackedTracked,
                        "bounded exactly like NearbyNodesViewModel's own dictionary")
        XCTAssertEqual(h.engine.untracked.map(\.nodeID), h.engine.untracked.map(\.nodeID).sorted(),
                        "the LRU keeps a total order — never an arbitrary one that reshuffles run to run")
        XCTAssertEqual(h.engine.untracked.last?.nodeID, 7099, "oldest-heard evicted first")
    }
}
