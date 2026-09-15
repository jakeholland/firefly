// A02 §3.3 amendment / A03 §3.6 amendment (2026-09-14) — the one test
// in this suite that is deliberately written against the API as it
// stood BEFORE the fix.
//
// Every other test of the post-commit budget lives in `AdminWriteTests`
// and injects the new knobs (`postCommitReadyTimeout`,
// `postCommitDisconnectGrace`) or asserts the new error
// (`.committedButNotVerified`). That makes them precise, and it also
// makes them impossible to run on `main`: they would not compile there,
// and a test that cannot compile against the broken code proves nothing
// about the break.
//
// This file uses ONLY what `main` already had — `adminResponseTimeout`,
// `beginEditSettingsRetryDelay`, `LoopbackTransport.simulateDisconnect`
// / `simulateReconnect` — so it can be copied verbatim into a worktree
// at the pre-fix commit and run there. It was, against d8ee569d:
//
//     $ swift test --filter PostCommitRebootTests
//     error: XCTAssertEqual failed: ("8") is not equal to ("7") —
//            the read-back must not be sent until the puck has come back
//     Executed 1 test, with 3 failures (1 unexpected) in 4.848s
//
// The "8" is the defect: the read-back had ALREADY gone out, into a
// link the firmware was tearing down, where it sat until
// `adminResponseTimeout` gave up. That is the bench defect (2026-09-14,
// Heltec `TAY_06b0` fw 2.7.26, `-FireflyDebugJoinCrew FIRE-8MNTT2`)
// reproduced with no radio at all.
//
// The helpers below are this file's own copies, which is this suite's
// standing convention — `ClientHandshakeTests`, `ClientReconnectTests`
// and `ClientRestorationTests` each carry their own for the same
// reason: a shared helper would have to live somewhere that compiles on
// both sides of the fix, and self-containment is what makes the copy
// above a copy of one file rather than of a tree.

import FireflyMesh
import MeshtasticProto
import XCTest

final class PostCommitRebootTests: XCTestCase {

    /// A puck that reboots at `commit_edit_settings` and comes back
    /// LONG after an admin-read budget has expired has still joined —
    /// the write reached it, the read-back confirms it, and the only
    /// thing that ever went wrong was the app giving up too early.
    ///
    /// Scaled by RATIO rather than by picked numbers, so the shape is
    /// the bench's own and not a coincidence: `adminResponseTimeout` is
    /// 1 s standing in for the shipped 30 s, and the link comes back at
    /// **2×** that — the same "came back past the budget" relationship
    /// the bench measured at 45 s against 30 s. The post-commit budget
    /// is left at its shipped default on purpose: this test must not
    /// mention the knob that fixes it, or it could not run on `main`.
    ///
    /// On `main` the two waits were one number, so the wait for the
    /// puck to come back was bounded by `adminResponseTimeout` and this
    /// throws `.timeout` — with the channel written on the radio.
    ///
    /// What a scaled test cannot prove is that a REAL 45-second reboot
    /// fits the SHIPPED budget; that is pinned directly, and separately,
    /// by `AdminWriteTests.testThePostCommitBudgetIsSizedForARealReboot`.
    func testAPuckThatComesBackLongAfterAnAdminReadBudgetHasJoined() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(
            transport: transport,
            adminResponseTimeout: .seconds(1),
            beginEditSettingsRetryDelay: .milliseconds(1))
        try await completeHandshake(transport: transport, client: client, myNodeNum: 1)

        let channel = sampleChannel()
        let request = ChannelWriteRequest(channels: [channel], loraConfig: nil)
        let applyTask = Task { try await client.applyChannelSet(request) }

        // begin x2, set_channel, commit_edit_settings
        try await waitForSentCount(7, on: transport)
        try assertAdminFrame(transport, at: 6) { $0.commitEditSettings = true }

        // The radio does what a commit makes it do: Bluetooth off, save
        // to flash, restart. On the bench this arrived as
        // `didDisconnectPeripheral … CBErrorDomain Code=7`.
        transport.simulateDisconnect(reason: "CBErrorDomain Code=7")
        let sentAtLoss = transport.sentMessages.count

        // The defect itself, stated as an assertion rather than left to
        // be inferred from a later index mismatch. Nothing may have been
        // sent after the commit: the read-back belongs AFTER the puck is
        // back, and on `main` it had already gone out by now — into a
        // link the firmware was in the middle of tearing down, where it
        // sat until `adminResponseTimeout` gave up.
        XCTAssertEqual(sentAtLoss, 7,
                       "the read-back must not be sent until the puck has come back")

        // ...and comes back at 2x the admin-read budget. On `main`, the
        // post-commit wait has already thrown by now.
        try await Task.sleep(for: .seconds(2))
        transport.simulateReconnect()

        // The handshake re-runs, exactly as it does on a real reconnect.
        try await waitForSentCount(sentAtLoss + 2, on: transport) // heartbeat, want_config(onlyConfig)
        transport.inject(myInfoFrame(num: 1))
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyConfig))
        try await waitForSentCount(sentAtLoss + 3, on: transport) // want_config(onlyNodeDB)
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyNodeDB))

        // Only NOW does the read-back go out — which is the whole point.
        // It could not have gone out before the puck came back, and on
        // `main` it did, into a link that was already going down.
        try await waitForSentCount(sentAtLoss + 4, on: transport)
        let readBackIndex = sentAtLoss + 3
        try assertAdminFrame(transport, at: readBackIndex, wantResponse: true) { $0.getChannelRequest = 1 }
        let (channelReqPacket, _) = try decodeAdminSend(transport, at: readBackIndex)
        transport.inject(adminResponseFrame(requestID: channelReqPacket.id) { $0.getChannelResponse = channel })

        let report = try await applyTask.value
        XCTAssertEqual(report.channels, [channel],
                       "a puck that rebooted, came back and read back the channel it was given has JOINED")
    }

    // MARK: - FromRadio builders (this file's own — see the header)

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

    private struct TestTimeout: Error {}

    private func waitForSentCount(_ n: Int, on transport: LoopbackTransport,
                                  file: StaticString = #filePath, line: UInt = #line) async throws {
        for _ in 0..<400 {
            if transport.sentMessages.count >= n { return }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTFail("timed out waiting for \(n) sent message(s); saw \(transport.sentMessages.count)",
                file: file, line: line)
        throw TestTimeout()
    }

    private func completeHandshake(transport: LoopbackTransport, client: MeshtasticClient,
                                   myNodeNum: UInt32 = 0x1234) async throws {
        let connectTask = Task { try await client.connect() }
        try await waitForSentCount(2, on: transport) // heartbeat, want_config(onlyConfig)
        transport.inject(myInfoFrame(num: myNodeNum))
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyConfig))
        try await waitForSentCount(3, on: transport) // want_config(onlyNodeDB)
        transport.inject(configCompleteFrame(MeshtasticConfigNonce.onlyNodeDB))
        try await connectTask.value
    }

    private func decodeAdminSend(_ transport: LoopbackTransport, at index: Int,
                                 file: StaticString = #filePath, line: UInt = #line) throws
        -> (packet: MeshPacket, admin: AdminMessage) {
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

    /// Byte-for-byte against a frame this test builds independently —
    /// the same convention (and the same reasoning about the randomly
    /// seeded packet id) as `AdminWriteTests.assertAdminFrame`.
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
        expectedPacket.id = sentPacket.id
        expectedPacket.to = sentPacket.to
        expectedPacket.from = sentPacket.to
        expectedPacket.wantAck = true
        expectedPacket.priority = .reliable
        expectedPacket.decoded = expectedData

        var expectedToRadio = ToRadio()
        expectedToRadio.packet = expectedPacket

        XCTAssertEqual(raw, try expectedToRadio.serializedData(),
                       "sent frame \(index) is not byte-identical to the proto-derived expected frame",
                       file: file, line: line)
    }

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
}
