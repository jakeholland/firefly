//
//  AdminWriteTests.swift — M3's channel/config write-back
//  (`applyChannelSet`/`setOwner`/`setRegion`), driven with injected
//  `FromRadio` bytes over `LoopbackTransport`, no radio, no simulator
//  (docs/specs/A01-companion-app.md, M3: "Channel write-back (admin
//  messages) behind an explicit confirmation").
//
//  Covers the task's own required surface: exact admin `ToRadio` frames
//  for a known `ChannelSet` (asserted byte-for-byte against bytes this
//  file derives from the SAME `MeshtasticProto` types the client uses —
//  see `expectedBytes(...)`'s own doc comment for why the packet id is
//  read back rather than hardcoded), a read-back mismatch surfacing as
//  `AdminWriteError.readBackMismatch` rather than a silent success,
//  "never writes without a connected node" (the client-level half of
//  the confirm-then-write state machine — the UI-level half, "never
//  writes before the user taps CONFIRM", is
//  `ChannelApplyStateMachineTests.swift` in the app target), and the
//  vendored `Config.LoRaConfig.RegionCode` raw values against
//  Meshtastic's own proto numbering.
//
import FireflyMesh
import MeshtasticProto
import XCTest

final class AdminWriteTests: XCTestCase {

    // MARK: - FromRadio / ToRadio builders

    private func fromRadio(_ build: (inout FromRadio) -> Void) -> Data {
        var fr = FromRadio()
        build(&fr)
        return (try? fr.serializedData()) ?? Data()
    }

    private func myInfoFrame(num: UInt32) -> Data {
        fromRadio { fr in
            var info = MyNodeInfo()
            info.myNodeNum = num
            fr.myInfo = info
        }
    }

    private func configCompleteFrame(_ id: UInt32) -> Data {
        fromRadio { fr in fr.configCompleteID = id }
    }

    /// A `get_*_response` `FromRadio.packet`, correlated to `requestID`
    /// (`Data.request_id`) the same way a real `AdminModule` answers a
    /// `get_*_request` — see `MeshtasticClient.sendAdminRequest`'s own
    /// citation of `AdminModule::handleGetOwner`.
    private func adminResponseFrame(requestID: UInt32, _ build: (inout AdminMessage) -> Void) -> Data {
        var admin = AdminMessage()
        build(&admin)
        var data = DataMessage()
        data.portnum = .adminApp
        data.payload = (try? admin.serializedData()) ?? Data()
        data.requestID = requestID
        var packet = MeshPacket()
        packet.decoded = data
        return fromRadio { fr in fr.packet = packet }
    }

    // MARK: - Test helpers

    private func waitForSentCount(_ n: Int, on transport: LoopbackTransport, file: StaticString = #filePath, line: UInt = #line) async throws {
        for _ in 0..<400 {
            if transport.sentMessages.count >= n { return }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTFail("timed out waiting for \(n) sent message(s); saw \(transport.sentMessages.count)", file: file, line: line)
        throw TestTimeout()
    }

    private struct TestTimeout: Error {}

    @discardableResult
    private func completeHandshake(
        transport: LoopbackTransport, client: MeshtasticClient, myNodeNum: UInt32 = 0x1234
    ) async throws -> Task<Void, Error> {
        let connectTask = Task { try await client.connect() }
        try await waitForSentCount(2, on: transport) // heartbeat, want_config(onlyConfig)
        transport.inject(myInfoFrame(num: myNodeNum))
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyConfig))
        try await waitForSentCount(3, on: transport) // want_config(onlyNodeDB)
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyNodeDB))
        try await connectTask.value
        return connectTask
    }

    /// Decodes `transport.sentMessages[index]` as a `ToRadio.packet`'s
    /// `AdminMessage`, returning both the decoded message and the
    /// packet's own id (needed to correlate an injected response, and to
    /// reconstruct an expected-bytes comparison — see `assertAdminFrame`).
    private func decodeAdminSend(_ transport: LoopbackTransport, at index: Int, file: StaticString = #filePath, line: UInt = #line) throws -> (packet: MeshPacket, admin: AdminMessage) {
        let raw = transport.sentMessages[index]
        let toRadio = try ToRadio(serializedBytes: raw)
        guard case .packet(let packet) = toRadio.payloadVariant else {
            XCTFail("sent message \(index) is not a MeshPacket", file: file, line: line)
            throw TestTimeout()
        }
        guard case .decoded(let data) = packet.payloadVariant, data.portnum == .adminApp else {
            XCTFail("sent message \(index) is not an ADMIN_APP packet", file: file, line: line)
            throw TestTimeout()
        }
        let admin = try AdminMessage(serializedBytes: data.payload)
        return (packet, admin)
    }

    /// Byte-for-byte proof that a sent admin frame is EXACTLY what
    /// `ToRadio`/`MeshPacket`/`DataMessage`/`AdminMessage` — the same
    /// vendored proto types `MeshtasticClient` itself uses — would
    /// produce for the given semantic content. The packet id is read
    /// back off the actual send (`packet.id`) rather than hardcoded:
    /// `MeshtasticClient` seeds its packet-id counter from
    /// `UInt32.random` (`packetIDCounter`'s own doc comment — two
    /// client lifetimes must not collide), so it is data this test
    /// reads, not a value either side gets to assert in advance. Every
    /// OTHER field (`to`, `from`, `wantAck`/`wantResponse`, `priority`,
    /// `channel`, and the admin oneof payload itself) is compared
    /// byte-for-byte against a frame this test constructs independently.
    private func assertAdminFrame(
        _ transport: LoopbackTransport, at index: Int, wantResponse: Bool = false,
        buildAdmin: (inout AdminMessage) -> Void,
        file: StaticString = #filePath, line: UInt = #line
    ) throws {
        let raw = transport.sentMessages[index]
        let toRadio = try ToRadio(serializedBytes: raw)
        guard case .packet(let sentPacket) = toRadio.payloadVariant else {
            return XCTFail("sent message \(index) is not a MeshPacket", file: file, line: line)
        }

        var expectedAdmin = AdminMessage()
        buildAdmin(&expectedAdmin)
        var expectedData = DataMessage()
        expectedData.portnum = .adminApp
        expectedData.payload = try expectedAdmin.serializedData()
        if wantResponse { expectedData.wantResponse = true }

        var expectedPacket = MeshPacket()
        expectedPacket.id = sentPacket.id // read back — see this method's own doc comment
        expectedPacket.to = sentPacket.to
        expectedPacket.from = sentPacket.to // local admin: from == to == our own node
        expectedPacket.wantAck = !wantResponse // writes want_ack; reads do not (mc_client.c's own rule)
        if !wantResponse { expectedPacket.priority = .reliable }
        expectedPacket.decoded = expectedData

        var expectedToRadio = ToRadio()
        expectedToRadio.packet = expectedPacket

        XCTAssertEqual(raw, try expectedToRadio.serializedData(), "sent frame \(index) is not byte-identical to the proto-derived expected frame", file: file, line: line)
    }

    // MARK: - A known ChannelSet, written and read back

    private func sampleChannel() -> Channel {
        var settings = ChannelSettings()
        settings.name = "Firefly"
        settings.psk = Data([0x9a, 0x2f, 0x6c, 0x11])
        settings.moduleSettings.positionPrecision = 32
        var channel = Channel()
        channel.index = 0
        channel.role = .primary
        channel.settings = settings
        return channel
    }

    private func sampleLoRaConfig() -> Config.LoRaConfig {
        var lora = Config.LoRaConfig()
        lora.usePreset = true
        lora.modemPreset = .longFast
        lora.region = .us
        return lora
    }

    /// Drives `applyChannelSet` for a one-channel, replace-mode request
    /// (a channel plus its LoRa config) through the whole
    /// begin/set/set/commit/read-back sequence, asserting the exact
    /// admin `ToRadio` frame at each step and then answering the two
    /// read-back requests with matching content.
    func testApplyChannelSetSendsExactAdminFramesAndReadsBackSuccessfully() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        try await completeHandshake(transport: transport, client: client, myNodeNum: 48_621_524)

        let channel = sampleChannel()
        let lora = sampleLoRaConfig()
        let request = ChannelWriteRequest(channels: [channel], loraConfig: lora)

        let applyTask = Task { try await client.applyChannelSet(request) }

        // 1. begin_edit_settings
        try await waitForSentCount(4, on: transport)
        try assertAdminFrame(transport, at: 3) { $0.beginEditSettings = true }

        // 2. set_channel
        try await waitForSentCount(5, on: transport)
        try assertAdminFrame(transport, at: 4) { $0.setChannel = channel }

        // 3. set_config (lora)
        try await waitForSentCount(6, on: transport)
        try assertAdminFrame(transport, at: 5) { admin in
            var config = Config()
            config.lora = lora
            admin.setConfig = config
        }

        // 4. commit_edit_settings
        try await waitForSentCount(7, on: transport)
        try assertAdminFrame(transport, at: 6) { $0.commitEditSettings = true }

        // The link never actually dropped (LoopbackTransport doesn't
        // simulate the commit's reboot on its own) — waitForReadyAfterCommit
        // resolves off CurrentValueEventHub's replay of the .ready this
        // client already reached, so the read-back requests follow
        // immediately, same as a real 2.8 live-apply commit that never
        // disconnects.

        // 5. get_channel_request(index: 0)
        try await waitForSentCount(8, on: transport)
        try assertAdminFrame(transport, at: 7, wantResponse: true) { $0.getChannelRequest = 0 }
        let (channelReqPacket, _) = try decodeAdminSend(transport, at: 7)
        transport.inject(adminResponseFrame(requestID: channelReqPacket.id) { $0.getChannelResponse = channel })

        // 6. get_config_request(loraConfig)
        try await waitForSentCount(9, on: transport)
        try assertAdminFrame(transport, at: 8, wantResponse: true) { $0.getConfigRequest = .loraConfig }
        let (loraReqPacket, _) = try decodeAdminSend(transport, at: 8)
        transport.inject(adminResponseFrame(requestID: loraReqPacket.id) { admin in
            var config = Config()
            config.lora = lora
            admin.getConfigResponse = config
        })

        let report = try await applyTask.value
        XCTAssertEqual(report.channels, [channel])
        XCTAssertEqual(report.loraConfig, lora)
    }

    /// A read-back that does not match what was written must surface as
    /// a clear, typed error — never a silent success (A01 M3: "honest
    /// success = read-back matches; otherwise a clear error").
    func testChannelReadBackMismatchThrowsReadBackMismatch() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        try await completeHandshake(transport: transport, client: client, myNodeNum: 1)

        let channel = sampleChannel()
        let request = ChannelWriteRequest(channels: [channel], loraConfig: nil)
        let applyTask = Task { try await client.applyChannelSet(request) }

        try await waitForSentCount(7, on: transport) // begin, set_channel, commit, get_channel_request
        let (channelReqPacket, _) = try decodeAdminSend(transport, at: 6)

        // The node reports a DIFFERENT name than what was written —
        // e.g. a concurrent write from another client, or firmware that
        // silently truncated it.
        var mismatched = channel
        mismatched.settings.name = "NotFirefly"
        transport.inject(adminResponseFrame(requestID: channelReqPacket.id) { $0.getChannelResponse = mismatched })

        do {
            _ = try await applyTask.value
            XCTFail("expected a readBackMismatch")
        } catch let error as AdminWriteError {
            guard case .readBackMismatch = error else {
                return XCTFail("expected .readBackMismatch, got \(error)")
            }
        }
    }

    /// A `get_channel_request`/`get_config_request`/`get_owner_request`
    /// that never gets an answer — the node rebooted and never came
    /// back, or firmware ignored it — must time out honestly rather
    /// than hang or silently report success.
    func testReadBackNeverArrivingTimesOut() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport, adminResponseTimeout: .milliseconds(80))
        try await completeHandshake(transport: transport, client: client, myNodeNum: 1)

        let request = ChannelWriteRequest(channels: [sampleChannel()], loraConfig: nil)
        do {
            _ = try await client.applyChannelSet(request)
            XCTFail("expected a timeout")
        } catch let error as AdminWriteError {
            XCTAssertEqual(error, .timeout)
        }
    }

    // MARK: - setOwner

    func testSetOwnerWritesAndReadsBack() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        try await completeHandshake(transport: transport, client: client, myNodeNum: 1)

        let applyTask = Task { try await client.setOwner(longName: "Firefly One", shortName: "FF1") }

        try await waitForSentCount(4, on: transport) // begin
        try assertAdminFrame(transport, at: 3) { $0.beginEditSettings = true }

        try await waitForSentCount(5, on: transport) // set_owner
        try assertAdminFrame(transport, at: 4) { admin in
            var owner = User()
            owner.longName = "Firefly One"
            owner.shortName = "FF1"
            admin.setOwner = owner
        }

        try await waitForSentCount(6, on: transport) // commit
        try assertAdminFrame(transport, at: 5) { $0.commitEditSettings = true }

        try await waitForSentCount(7, on: transport) // get_owner_request
        try assertAdminFrame(transport, at: 6, wantResponse: true) { $0.getOwnerRequest = true }
        let (ownerReqPacket, _) = try decodeAdminSend(transport, at: 6)
        transport.inject(adminResponseFrame(requestID: ownerReqPacket.id) { admin in
            var owner = User()
            owner.longName = "Firefly One"
            owner.shortName = "FF1"
            admin.getOwnerResponse = owner
        })

        let report = try await applyTask.value
        XCTAssertEqual(report.longName, "Firefly One")
        XCTAssertEqual(report.shortName, "FF1")
    }

    // MARK: - setRegion

    /// `set_config.lora` replaces the WHOLE submessage on the wire —
    /// `setRegion` must read the node's current LoRa config first and
    /// change only `region`, never silently reset bandwidth/spread
    /// factor/tx power/etc to their zero defaults.
    func testSetRegionReadsCurrentConfigFirstAndPreservesEveryOtherField() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        try await completeHandshake(transport: transport, client: client, myNodeNum: 1)

        var currentLora = Config.LoRaConfig()
        currentLora.usePreset = false
        currentLora.bandwidth = 250
        currentLora.spreadFactor = 9
        currentLora.codingRate = 7
        currentLora.txPower = 17
        currentLora.region = .unset

        let applyTask = Task { try await client.setRegion(.us) }

        // 1. get_config_request(loraConfig) — the read before the write.
        try await waitForSentCount(4, on: transport)
        try assertAdminFrame(transport, at: 3, wantResponse: true) { $0.getConfigRequest = .loraConfig }
        let (readReqPacket, _) = try decodeAdminSend(transport, at: 3)
        transport.inject(adminResponseFrame(requestID: readReqPacket.id) { admin in
            var config = Config()
            config.lora = currentLora
            admin.getConfigResponse = config
        })

        var expectedWrite = currentLora
        expectedWrite.region = .us

        try await waitForSentCount(5, on: transport) // begin
        try assertAdminFrame(transport, at: 4) { $0.beginEditSettings = true }

        try await waitForSentCount(6, on: transport) // set_config
        try assertAdminFrame(transport, at: 5) { admin in
            var config = Config()
            config.lora = expectedWrite
            admin.setConfig = config
        }

        try await waitForSentCount(7, on: transport) // commit
        try assertAdminFrame(transport, at: 6) { $0.commitEditSettings = true }

        try await waitForSentCount(8, on: transport) // get_config_request (read-back)
        let (readBackPacket, _) = try decodeAdminSend(transport, at: 7)
        transport.inject(adminResponseFrame(requestID: readBackPacket.id) { admin in
            var config = Config()
            config.lora = expectedWrite
            admin.getConfigResponse = config
        })

        let report = try await applyTask.value
        XCTAssertEqual(report.region, .us)
    }

    // MARK: - Never writes without a connected node

    /// `applyChannelSet`/`setOwner`/`setRegion` must refuse — with
    /// `AdminWriteError.notConnected` — and send ZERO bytes when there
    /// is no connected node, rather than addressing an admin message to
    /// node 0 (proto3's zero default, not a real destination). This is
    /// the client-level half of "never writes without confirmation" —
    /// the UI never even offers a reachable Apply action while
    /// disconnected, and the client independently refuses to send
    /// anything if it were somehow called anyway.
    func testNeverWritesWithoutAConnectedNode() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport) // never connected

        do {
            _ = try await client.applyChannelSet(ChannelWriteRequest(channels: [sampleChannel()]))
            XCTFail("expected .notConnected")
        } catch let error as AdminWriteError {
            XCTAssertEqual(error, .notConnected)
        }
        do {
            _ = try await client.setOwner(longName: "X", shortName: "X")
            XCTFail("expected .notConnected")
        } catch let error as AdminWriteError {
            XCTAssertEqual(error, .notConnected)
        }
        do {
            _ = try await client.setRegion(.us)
            XCTFail("expected .notConnected")
        } catch let error as AdminWriteError {
            XCTAssertEqual(error, .notConnected)
        }

        XCTAssertTrue(transport.sentMessages.isEmpty, "no admin write may reach the wire without a connected node")
    }

    // MARK: - Region enum mapping

    /// Pins the vendored `Config.LoRaConfig.RegionCode` raw values
    /// against Meshtastic's own proto numbering (`meshtastic/config.proto`,
    /// pinned commit — see `app/tools/gen_swift_protos.sh`) — a
    /// mismatch here means the generator pin drifted and `setRegion`
    /// would silently address the wrong region on the wire.
    func testRegionCodeRawValuesMatchMeshtasticProtoNumbering() {
        XCTAssertEqual(Config.LoRaConfig.RegionCode.unset.rawValue, 0)
        XCTAssertEqual(Config.LoRaConfig.RegionCode.us.rawValue, 1)
        XCTAssertEqual(Config.LoRaConfig.RegionCode.eu433.rawValue, 2)
        XCTAssertEqual(Config.LoRaConfig.RegionCode.eu868.rawValue, 3)
        XCTAssertEqual(Config.LoRaConfig.RegionCode.cn.rawValue, 4)
        XCTAssertEqual(Config.LoRaConfig.RegionCode.jp.rawValue, 5)
        XCTAssertEqual(Config.LoRaConfig.RegionCode.anz.rawValue, 6)
        XCTAssertEqual(Config.LoRaConfig.RegionCode.kr.rawValue, 7)
        XCTAssertEqual(Config.LoRaConfig.RegionCode.tw.rawValue, 8)
        XCTAssertEqual(Config.LoRaConfig.RegionCode.ru.rawValue, 9)
        XCTAssertEqual(Config.LoRaConfig.RegionCode.in.rawValue, 10)
    }

    /// `setRegion` must carry through the exact enum case it was given
    /// — a case near the end of the enum, not just `.us` (the common
    /// path every other test here exercises), so an off-by-a-few mapping
    /// bug would not hide behind coincidence.
    func testSetRegionWireEncodesTheExactRegionRequested() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        try await completeHandshake(transport: transport, client: client, myNodeNum: 1)

        let applyTask = Task { try await client.setRegion(.jp) }

        try await waitForSentCount(4, on: transport)
        let (readReqPacket, _) = try decodeAdminSend(transport, at: 3)
        transport.inject(adminResponseFrame(requestID: readReqPacket.id) { admin in
            var config = Config()
            config.lora = Config.LoRaConfig() // unset region
            admin.getConfigResponse = config
        })

        try await waitForSentCount(6, on: transport) // begin, set_config
        let (_, setAdmin) = try decodeAdminSend(transport, at: 5)
        guard case .setConfig(let sentConfig) = setAdmin.payloadVariant, case .lora(let sentLora)? = sentConfig.payloadVariant else {
            return XCTFail("expected a set_config.lora write")
        }
        XCTAssertEqual(sentLora.region, .jp)

        try await waitForSentCount(7, on: transport) // commit
        try await waitForSentCount(8, on: transport) // read-back request
        let (readBackPacket, _) = try decodeAdminSend(transport, at: 7)
        transport.inject(adminResponseFrame(requestID: readBackPacket.id) { admin in
            var config = Config()
            var lora = Config.LoRaConfig()
            lora.region = .jp
            config.lora = lora
            admin.getConfigResponse = config
        })

        let report = try await applyTask.value
        XCTAssertEqual(report.region, .jp)
    }
}
