//
//  CrewNodeInfoRequestOnAdmissionTests.swift — bench finding
//  2026-09-14 (`docs/specs/A02-crew-join.md` §4.4): a node admitted to
//  the crew without a name gets asked directly for its NodeInfo
//  (`NODEINFO_APP`, `want_response = true`) instead of waiting out
//  Meshtastic's ~3h periodic broadcast. Mirrors firmware's own
//  `firmware/app/tests/test_shell.c` "NIR_*" group, one test each:
//
//   * NIR_nameless_admission_requests_nodeinfo_once
//   * NIR_named_member_readmission_never_requests
//   * NIR_replay_never_requests
//   * NIR_rate_limit_honoured
//   * NIR_reply_names_the_member
//
//  Same harness and determinism discipline as `CrewMembershipEngineTests`/
//  `CrewAdmissionHardeningTests` — scripted `FromRadio` bytes through
//  `LoopbackTransport` -> the real `MeshtasticClient` -> the real gate
//  -> the real `ff_crew`, duplicated here rather than shared across
//  files for the same reason those two give for their own duplication:
//  each helper is a handful of lines, not a rulebook that could drift.
//  A REQUEST is verified by decoding `transport.sentMessages` for an
//  actual `NODEINFO_APP` / `want_response = true` packet — never by
//  asking whether a function was called — the same "assert the ACTUAL
//  wire format" discipline `ClientPositionAndPrivateTests` states for
//  itself.
//
//  WHY THE WAIT IS FILTERED, NOT A RAW SENT-COUNT (found running this
//  suite for the first time): `CrewMembershipEngine.configure(crew:)`
//  starts its own background channel-table resolution
//  (`resolveCrewChannelIndex()` -> `refreshCrewChannelIndex()` ->
//  `client.currentChannelTable()`), which — against the REAL
//  `MeshtasticClient` this harness uses — sends its own `AdminMessage
//  .get_channel_request` over the wire, on a timer this suite never
//  drives to completion (no response is ever injected for it). That
//  task is harmless to admission (this harness calls `applyChannelTable`
//  directly, which is synchronous and does not depend on it) but it
//  DOES add an extra, timing-dependent message to `transport
//  .sentMessages` that a raw `waitForSentCount(_:)` cannot tell apart
//  from the NodeInfo request under test. `waitForNodeInfoRequestCount`
//  below polls the DECODED, FILTERED count instead, so this suite
//  never has to know or care how many other things happen to be in
//  flight on the wire.
//
import FireflyMesh
import FireflyModel
import MeshtasticProto
import XCTest

@MainActor
final class CrewNodeInfoRequestOnAdmissionTests: XCTestCase {

    private static let myNodeNum: UInt32 = 48_621_524
    private static let crewIndex: UInt32 = 0
    private static let sentinelID: UInt32 = 0xFEED_FACE
    private static let sentinelName = "sentinel"
    private static let crew = CrewChannelIdentity(
        code: "FIRE-4K9M7X",
        psk: Data((0..<32).map { UInt8($0 &+ 1) }))

    /// A clock the test drives explicitly (as opposed to
    /// `CrewMembershipEngineTests`'s auto-incrementing `SteppingClock`):
    /// the rate-limit boundary test needs `now` to hold still across two
    /// calls inside one `admit(_:)` (`lastAdmissionAtMs`, then the
    /// throttle) and then jump by an EXACT amount the test controls.
    private final class ManualClock: @unchecked Sendable {
        private let lock = NSLock()
        private var current: Date
        init(_ date: Date) { current = date }
        func now() -> Date { lock.lock(); defer { lock.unlock() }; return current }
        func advance(by interval: TimeInterval) {
            lock.lock(); current = current.addingTimeInterval(interval); lock.unlock()
        }
    }

    private struct Harness {
        let transport: LoopbackTransport
        let client: MeshtasticClient
        let core: CoreStore
        let pairing: CrewPairingController
        let engine: CrewMembershipEngine
        let clock: ManualClock
        let stream: AsyncStream<MeshNodeSnapshot>
    }

    private func makeHarness() -> Harness {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        let core = CoreStore()
        let pairing = CrewPairingController(crew: core.crew, store: InMemoryCrewPairingStore())
        let clock = ManualClock(Date(timeIntervalSince1970: 1_780_000_000))
        let engine = CrewMembershipEngine(pairing: pairing, store: InMemoryCrewLocalStateStore(),
                                          client: client, now: clock.now)
        core.membership = engine
        return Harness(transport: transport, client: client, core: core, pairing: pairing,
                       engine: engine, clock: clock, stream: client.nodeUpdates())
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

    private func frame(_ build: (inout FromRadio) -> Void) throws -> Data {
        var fr = FromRadio()
        build(&fr)
        return try fr.serializedData()
    }

    private func packetFrame(from: UInt32, channel: UInt32, portnum: PortNum, payload: Data) throws -> Data {
        var pkt = MeshPacket()
        pkt.from = from
        pkt.to = meshBroadcastAddress
        pkt.channel = channel
        var data = DataMessage()
        data.portnum = portnum
        data.payload = payload
        pkt.decoded = data
        return try frame { $0.packet = pkt }
    }

    private func userPayload(long: String, short: String = "") throws -> Data {
        var user = User()
        user.longName = long
        user.shortName = short
        return try user.serializedData()
    }

    private func completeHandshake(_ h: Harness) async throws {
        let connectTask = Task { try await h.client.connect() }
        try await h.transport.waitForSentCount(2)
        var info = MyNodeInfo()
        info.myNodeNum = Self.myNodeNum
        h.transport.inject(try frame { $0.myInfo = info })
        h.transport.inject(try frame { $0.configCompleteID = MeshtasticConfigNonce.onlyConfig })
        try await h.transport.waitForSentCount(3)
        h.transport.inject(try frame { $0.configCompleteID = MeshtasticConfigNonce.onlyNodeDB })
        try await connectTask.value
    }

    /// Drains every snapshot published so far into the gate, stopping at
    /// a sentinel — see the file header on `CrewMembershipEngineTests`'s
    /// identical discipline, with ONE addition this file's own tests
    /// needed and that one did not: the marker is unique PER CALL
    /// (`pumpCounter`), not the fixed `sentinelID`/`sentinelName` pair
    /// reused every time.
    ///
    /// Found running this suite for the first time: a sentinel's own
    /// `NODEINFO_APP` packet yields TWICE (`applyRxMeta`'s generic
    /// rx-meta snapshot, then the `.nodeinfoApp`-specific decode that
    /// actually names it) — see `MeshtasticClient.handle(meshPacket:)`.
    /// On the FIRST call the generic yield carries no name yet, so the
    /// loop correctly runs past it to the second, NAMED yield before
    /// breaking. On every call AFTER that, the sentinel id already has a
    /// cached name in `NodeDB` from the previous round, so the GENERIC
    /// yield now ALSO already carries that (stale) name — with a fixed
    /// marker, that stale match broke the loop one yield too early,
    /// leaving the round's second (truly current) yield undrained and
    /// sitting in the stream for the NEXT `pump` call to dequeue FIRST —
    /// which then broke on that leftover before ever seeing what THIS
    /// round actually injected. A monotonically unique marker closes
    /// this: a stale yield can never satisfy the CURRENT round's break
    /// condition, so it is drained (harmlessly — it is still off the
    /// crew channel) and the loop keeps going to the real match.
    private var pumpCounter: UInt32 = 0

    private func pump(_ h: Harness) async throws {
        pumpCounter += 1
        let marker = "\(Self.sentinelName)-\(pumpCounter)"
        h.transport.inject(try packetFrame(from: Self.sentinelID, channel: Self.crewIndex + 1,
                                           portnum: .nodeinfoApp,
                                           payload: try userPayload(long: marker)))
        for await snapshot in h.stream {
            h.core.apply(nodeUpdate: snapshot)
            if snapshot.num == Self.sentinelID, snapshot.longName == marker { break }
        }
    }

    private func isPaired(_ h: Harness, _ nodeID: UInt32) -> Bool {
        h.core.crew.member(nodeID: nodeID, now: FireflyClock.nowMillis())?.paired == true
    }

    /// Every destination this harness has asked for a NodeInfo, in wire
    /// order — decoded straight off `transport.sentMessages`, never off
    /// a call-was-made flag. `want_response` is asserted here too, once,
    /// so every call site downstream only has to check destinations.
    private func nodeInfoRequestDestinations(_ h: Harness) -> [UInt32] {
        h.transport.sentMessages.compactMap { raw -> UInt32? in
            guard let toRadio = try? ToRadio(serializedBytes: raw),
                  case .packet(let pkt)? = toRadio.payloadVariant,
                  case .decoded(let body)? = pkt.payloadVariant,
                  body.portnum == .nodeinfoApp,
                  body.wantResponse else { return nil }
            return pkt.to
        }
    }

    /// The request is fire-and-forget (`Task { ... }` inside
    /// `requestNodeInfoIfNameless`), so a POSITIVE expectation ("a
    /// request WAS sent") has to be waited out rather than checked the
    /// instant `pump` returns. Polls the FILTERED count — see this
    /// file's header comment on why a raw `transport.waitForSentCount`
    /// is the wrong tool here.
    private func waitForNodeInfoRequestCount(_ n: Int, on h: Harness,
                                             file: StaticString = #filePath, line: UInt = #line) async throws {
        for _ in 0..<400 {
            if nodeInfoRequestDestinations(h).count >= n { return }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTFail("timed out waiting for \(n) NodeInfo request(s); saw \(nodeInfoRequestDestinations(h).count)",
                file: file, line: line)
    }

    private func connectedCrewHarness() async throws -> Harness {
        let h = makeHarness()
        try await completeHandshake(h)
        h.engine.configure(crew: Self.crew)
        h.engine.applyChannelTable([crewChannel(at: 0)])
        XCTAssertEqual(h.engine.channelStatus, .resolved(index: 0))
        return h
    }

    // MARK: - NIR_nameless_admission_requests_nodeinfo_once

    func testNamelessAdmissionRequestsNodeInfoOnce() async throws {
        let h = try await connectedCrewHarness()

        var position = Position()
        position.latitudeI = 477_081_350
        position.longitudeI = -1_222_820_993
        position.locationSource = .locInternal
        h.transport.inject(try packetFrame(from: 3001, channel: Self.crewIndex, portnum: .positionApp,
                                           payload: try position.serializedData()))
        try await pump(h)
        try await waitForNodeInfoRequestCount(1, on: h)

        XCTAssertTrue(isPaired(h, 3001))
        XCTAssertEqual(h.core.crew.member(nodeID: 3001, now: FireflyClock.nowMillis())?.longName, "",
                       "a Position packet carries no name to admit with")
        XCTAssertEqual(nodeInfoRequestDestinations(h), [3001],
                       "exactly one NodeInfo request, to the node that was just admitted")

        // A member is only ADMITTED once by construction — ordinary
        // traffic from an already-paired member never reaches
        // `admit(_:)` again (the `isCrew` fast path in `admits(_:)`), so
        // re-hearing them must not add a second request. No wait needed
        // for this negative expectation — see
        // `testRateLimitHonouredIncludingTheTenMinuteBoundary`'s own
        // comment on why a refused/no-op path has nothing async to race.
        h.transport.inject(try packetFrame(from: 3001, channel: Self.crewIndex, portnum: .positionApp,
                                           payload: try position.serializedData()))
        try await pump(h)
        XCTAssertEqual(nodeInfoRequestDestinations(h), [3001], "ordinary traffic asks nothing twice")
    }

    // MARK: - NIR_named_member_readmission_never_requests

    func testAlreadyNamedMemberNeverRequestsOnReadmission() async throws {
        let h = try await connectedCrewHarness()

        // Admitted AND named by the same live NodeInfo packet. This
        // DOES fire one request — `CoreStore.apply(nodeUpdate:)` writes
        // `crew.setIdentity` from this packet's own name only AFTER
        // `admits(_:)` (and this admission) returns, so the roster read
        // inside `requestNodeInfoIfNameless` still sees "nameless" for
        // THIS packet (`CrewMembershipEngine.requestNodeInfoIfNameless`'s
        // own doc comment — mirrors firmware's `shell_try_admit`
        // exactly). Waited out explicitly before the baseline is taken,
        // so that unavoidable first request cannot be mistaken for the
        // READMISSION this test actually checks — same reasoning
        // firmware's own `NIR_named_member_readmission_never_requests`
        // states for binding its spy only after this first admission.
        h.transport.inject(try packetFrame(from: 3002, channel: Self.crewIndex, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Dana", short: "DNA")))
        try await pump(h)
        try await waitForNodeInfoRequestCount(1, on: h)
        XCTAssertEqual(h.core.crew.member(nodeID: 3002, now: FireflyClock.nowMillis())?.longName, "Dana")
        let baseline = nodeInfoRequestDestinations(h)
        XCTAssertEqual(baseline, [3002], "the first admission's own, now-landed request")

        // Hide = unpair + remember (§4.5) — the name stays on the
        // `ff_crew` slot, only `paired` flips. Unhide, then a fresh
        // qualifying packet re-admits through `tryAdmit`/`admit(_:)` a
        // second time — the readmission this test is actually about.
        h.engine.hide(nodeID: 3002)
        XCTAssertFalse(isPaired(h, 3002))
        h.engine.unhide(nodeID: 3002)

        h.transport.inject(try packetFrame(from: 3002, channel: Self.crewIndex, portnum: .textMessageApp,
                                           payload: Data("hi again".utf8)))
        try await pump(h)

        XCTAssertTrue(isPaired(h, 3002))
        XCTAssertEqual(h.core.crew.member(nodeID: 3002, now: FireflyClock.nowMillis())?.longName, "Dana",
                       "still named — hide never erases identity")
        XCTAssertEqual(nodeInfoRequestDestinations(h), baseline,
                       "an already-named member is never asked again on readmission")
    }

    // MARK: - NIR_replay_never_requests

    func testTheWantConfigNodeInfoReplayNeverRequests() async throws {
        let h = makeHarness()
        var replayed = NodeInfo()
        replayed.num = 3003
        var user = User()
        user.longName = "Ghost"
        replayed.user = user
        try await withReplay(h, nodes: [replayed])
        h.engine.configure(crew: Self.crew)
        h.engine.applyChannelTable([crewChannel(at: 0)])

        try await pump(h)

        XCTAssertFalse(isPaired(h, 3003), "a replay is a summary, not evidence — §4.2")
        XCTAssertTrue(nodeInfoRequestDestinations(h).isEmpty,
                      "nothing was ever admitted, so nothing was ever asked")
    }

    private func withReplay(_ h: Harness, nodes: [NodeInfo]) async throws {
        let connectTask = Task { try await h.client.connect() }
        try await h.transport.waitForSentCount(2)
        var info = MyNodeInfo()
        info.myNodeNum = Self.myNodeNum
        h.transport.inject(try frame { $0.myInfo = info })
        h.transport.inject(try frame { $0.configCompleteID = MeshtasticConfigNonce.onlyConfig })
        try await h.transport.waitForSentCount(3)
        for node in nodes {
            h.transport.inject(try frame { $0.nodeInfo = node })
        }
        h.transport.inject(try frame { $0.configCompleteID = MeshtasticConfigNonce.onlyNodeDB })
        try await connectTask.value
    }

    // MARK: - NIR_rate_limit_honoured

    func testRateLimitHonouredIncludingTheTenMinuteBoundary() async throws {
        let h = try await connectedCrewHarness()

        h.transport.inject(try packetFrame(from: 3004, channel: Self.crewIndex, portnum: .textMessageApp,
                                           payload: Data("hi".utf8)))
        try await pump(h)
        try await waitForNodeInfoRequestCount(1, on: h)
        XCTAssertEqual(nodeInfoRequestDestinations(h), [3004])

        // Hide + unhide WITHOUT ever naming 3004 — still nameless, so
        // only the rate limit stands between re-admission and a second
        // request.
        h.engine.hide(nodeID: 3004)
        h.engine.unhide(nodeID: 3004)

        // Well inside the 10-minute window: re-admission must NOT ask
        // again. No wait needed for this negative expectation:
        // `requestNodeInfoIfNameless`'s rate-limit guard runs
        // SYNCHRONOUSLY inside `admit(_:)`, which `pump`'s drain loop
        // has already executed to completion by the time it returns —
        // if the guard refuses, no `Task` is ever created, so there is
        // nothing pending to race against.
        h.clock.advance(by: 60)
        h.transport.inject(try packetFrame(from: 3004, channel: Self.crewIndex, portnum: .textMessageApp,
                                           payload: Data("hi again".utf8)))
        try await pump(h)
        XCTAssertTrue(isPaired(h, 3004))
        XCTAssertEqual(nodeInfoRequestDestinations(h), [3004], "still inside the 10-minute window")

        // Past the window (measured from the FIRST request): hide/unhide
        // once more, then a fresh qualifying packet is due to ask again.
        h.engine.hide(nodeID: 3004)
        h.engine.unhide(nodeID: 3004)
        h.clock.advance(by: CrewNodeInfoRequestThrottle.rateLimit - 60) // exactly 10 min since the 1st
        h.transport.inject(try packetFrame(from: 3004, channel: Self.crewIndex, portnum: .textMessageApp,
                                           payload: Data("hi a third time".utf8)))
        try await pump(h)
        try await waitForNodeInfoRequestCount(2, on: h)
        XCTAssertEqual(nodeInfoRequestDestinations(h), [3004, 3004],
                       "exactly at the boundary, a new request is due")
    }

    // MARK: - NIR_reply_names_the_member

    func testANodeInfoReplyNamesTheMember() async throws {
        let h = try await connectedCrewHarness()

        h.transport.inject(try packetFrame(from: 3005, channel: Self.crewIndex, portnum: .textMessageApp,
                                           payload: Data("hi".utf8)))
        try await pump(h)
        try await waitForNodeInfoRequestCount(1, on: h)
        XCTAssertEqual(nodeInfoRequestDestinations(h), [3005])
        XCTAssertEqual(h.core.crew.member(nodeID: 3005, now: FireflyClock.nowMillis())?.longName, "")

        // The answer: an ordinary live NODEINFO_APP packet from 3005,
        // through the SAME decode path (`.nodeinfoApp` in
        // `MeshtasticClient.handle(meshPacket:)`) any other NodeInfo
        // broadcast takes — no separate seam or event exists for it, by
        // design (this file's header comment / `requestNodeInfo`'s own
        // doc comment on `MeshtasticClientProtocol`).
        h.transport.inject(try packetFrame(from: 3005, channel: Self.crewIndex, portnum: .nodeinfoApp,
                                           payload: try userPayload(long: "Marcus", short: "MRC")))
        try await pump(h)

        let member = h.core.crew.member(nodeID: 3005, now: FireflyClock.nowMillis())
        XCTAssertEqual(member?.longName, "Marcus", "the reply names the member in place")
        XCTAssertEqual(member?.shortName, "MRC")
    }
}
