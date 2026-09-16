//
//  CrewPresenceTelemetryWiringTests.swift — A04: `crew.member.seen`
//  wired through a REAL `MeshtasticClient` over `LoopbackTransport`, a
//  real `AppGraph`, `CoreStore` and `CrewMembershipEngine` — the same
//  pattern `AdmissionBeforePayloadGateTests` uses (see that file's own
//  header comment), reused here rather than mocked, so a passing test
//  proves the admission path this event hooks into is the SAME
//  admission path A02 already pins, not a parallel one that could drift
//  from it.
//
//  `crew.member.lost` (a pure elapsed-TIME transition, no packet to
//  hang a deterministic test off) is pinned instead by
//  `CrewPresenceTelemetryTransitionTests`, against the exact transition
//  table `AppGraph.checkCrewPresenceTransitions(freshRssi:from:)` calls
//  — see that file for why a real 10-minute wait has no place in this
//  test suite.
//
import FireflyCore
import FireflyMesh
import FireflyModel
import FireflyTelemetry
import MeshtasticProto
import XCTest

@MainActor
final class CrewPresenceTelemetryWiringTests: XCTestCase {
    private static let sender: UInt32 = 2_403_905_316
    private static let myNodeNum: UInt32 = 48_621_524
    private static let crew = CrewChannelIdentity(
        code: "FIRE-8MNTT2",
        psk: Data((0..<32).map { UInt8($0 &+ 7) }))

    private struct Harness {
        let transport: LoopbackTransport
        let client: MeshtasticClient
        let graph: AppGraph
        let telemetry: InMemoryTelemetryRecorder
    }

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

    private func waitForSentCount(_ n: Int, on transport: LoopbackTransport) async throws {
        for _ in 0..<400 {
            if transport.sentMessages.count >= n { return }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTFail("timed out waiting for \(n) sent message(s); saw \(transport.sentMessages.count)")
    }

    private func connectedCrewHarness() async throws -> Harness {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        let telemetry = InMemoryTelemetryRecorder()
        let graph = AppGraph(
            dependencies: AppDependencies(client: client, location: UnavailableLocationProvider(),
                                          heading: NoHeadingProvider(), store: InMemorySettingsStore(),
                                          telemetry: telemetry),
            notifications: RecordingNotificationSending(),
            skipLaunchAutoConnectUnderXCTest: true)
        let h = Harness(transport: transport, client: client, graph: graph, telemetry: telemetry)

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
        return h
    }

    /// A stranger's first qualifying packet admits them (A02 §4.1) AND
    /// fires `crew.member.seen {id_hash, age_s}` — the honest `id_hash`
    /// (never the raw node number — see `TelemetryAttributeAllowlist`),
    /// never a `crew.member.seen` for a node this phone never actually
    /// admitted.
    func testFirstAdmissionFiresCrewMemberSeenWithHashedID() async throws {
        let h = try await connectedCrewHarness()
        h.transport.inject(try fireflyFrame(.flare(durationS: 300), from: Self.sender, channel: 0))

        try await waitForCrewMemberSeen(on: h.telemetry)

        let events = await h.telemetry.events
        let seen = try XCTUnwrap(events.first { $0.name == TelemetryEventName.crewMemberSeen })
        let expectedHash = TelemetryHash.nodeID(Self.sender)
        XCTAssertEqual(seen.attributes[TelemetryAttributeKey.idHash], .string(expectedHash))
        // Never the raw node number, under ANY key.
        for value in seen.attributes.values {
            if case .int(let raw) = value {
                XCTAssertNotEqual(UInt32(raw), Self.sender, "a raw node id must never reach a telemetry event")
            }
        }
        XCTAssertNotNil(seen.attributes[TelemetryAttributeKey.ageS])
    }

    /// A second qualifying packet from the SAME member, still `.heard`,
    /// must not fire a second `seen` — the double-count the spec's
    /// "Known gaps" section explicitly calls out.
    func testASecondPacketFromAnAlreadyHeardMemberDoesNotDoubleFireSeen() async throws {
        let h = try await connectedCrewHarness()
        h.transport.inject(try fireflyFrame(.flare(durationS: 300), from: Self.sender, channel: 0, packetID: 1))
        try await waitForCrewMemberSeen(on: h.telemetry)

        h.transport.inject(try fireflyFrame(.flareEnd, from: Self.sender, channel: 0, packetID: 2))
        // Give the second packet's admission pipeline a moment to run.
        try await Task.sleep(for: .milliseconds(150))

        let events = await h.telemetry.events
        let seenCount = events.filter { $0.name == TelemetryEventName.crewMemberSeen }.count
        XCTAssertEqual(seenCount, 1, "an already-`.heard` member's second packet must not re-fire `seen`")
    }

    /// Polls the recorder until `crew.member.seen` has landed, or fails
    /// the test — the async-actor equivalent of `waitUntil`, for a
    /// condition that itself needs an `await` to check.
    private func waitForCrewMemberSeen(on telemetry: InMemoryTelemetryRecorder, timeout: Int = 200) async throws {
        for _ in 0..<timeout {
            let events = await telemetry.events
            if events.contains(where: { $0.name == TelemetryEventName.crewMemberSeen }) { return }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTFail("timed out waiting for crew.member.seen")
    }
}
