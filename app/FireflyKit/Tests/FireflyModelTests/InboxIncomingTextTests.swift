//
//  InboxIncomingTextTests.swift — PR #264 review, BLOCKING item 2: an
//  inbound TEXT_MESSAGE_APP packet, injected as raw `FromRadio` bytes at
//  the transport (no stub, no shortcut — the SAME `LoopbackTransport`
//  injection `ClientHandshakeTests` in FireflyMeshTests drives the
//  handshake with), makes it all the way through the real
//  `MeshtasticClient.handle(meshPacket:)` -> `incomingTexts()` ->
//  `InboxViewModel.ingest(_:)` path into the conversation for the
//  sender who sent it — and a self-originated echo (my own sent packet
//  id reflected back by the mesh) is dropped before it ever becomes a
//  second row, per `InMemoryInboxStore.mySentPacketIDs`'s own guard.
//
//  Duplicates a small slice of `ClientHandshakeTests`'s FromRadio/
//  handshake plumbing rather than sharing it: that file's helpers are
//  `private` to the `FireflyMeshTests` target, and this test needs
//  `FireflyModel` (`InboxViewModel`, `InMemoryInboxStore`) in the loop
//  too, which `FireflyMeshTests` does not depend on.
//
import FireflyMesh
import FireflyModel
import MeshtasticProto
import XCTest

@MainActor
final class InboxIncomingTextTests: XCTestCase {

    private struct TestTimeout: Error {}

    private func fromRadio(_ build: (inout FromRadio) -> Void) -> Data {
        var fr = FromRadio()
        build(&fr)
        return (try? fr.serializedData()) ?? Data()
    }

    private func textFrame(from: UInt32, to: UInt32, id: UInt32, text: String) -> Data {
        fromRadio { fr in
            var packet = MeshPacket()
            packet.from = from
            packet.to = to
            packet.id = id
            var data = DataMessage()
            data.portnum = .textMessageApp
            data.payload = Data(text.utf8)
            packet.decoded = data
            fr.packet = packet
        }
    }

    private func waitForSentCount(_ n: Int, on transport: LoopbackTransport, file: StaticString = #filePath, line: UInt = #line) async throws {
        for _ in 0..<400 {
            if transport.sentMessages.count >= n { return }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTFail("timed out waiting for \(n) sent message(s); saw \(transport.sentMessages.count)", file: file, line: line)
        throw TestTimeout()
    }

    /// The same two-phase want_config handshake `ClientHandshakeTests.
    /// completeHandshake` drives, reduced to what this file needs.
    private func completeHandshake(transport: LoopbackTransport, client: MeshtasticClient, myNodeNum: UInt32) async throws {
        let connectTask = Task { try await client.connect() }
        try await waitForSentCount(2, on: transport) // heartbeat, want_config(onlyConfig)

        transport.inject(fromRadio { fr in
            var info = MyNodeInfo()
            info.myNodeNum = myNodeNum
            fr.myInfo = info
        })
        transport.inject(fromRadio { fr in fr.configCompleteID = MeshtasticConfigNonce.onlyConfig })
        try await waitForSentCount(3, on: transport) // want_config(onlyNodeDB)
        transport.inject(fromRadio { fr in fr.configCompleteID = MeshtasticConfigNonce.onlyNodeDB })
        try await connectTask.value
    }

    /// Polls instead of a blind sleep: the transport's receive loop and
    /// `InboxViewModel`'s own `incomingTexts()` `Task` both hop actors
    /// before `provider` reflects the new row.
    private func waitForThread(_ conversation: ConversationKind, on provider: InMemoryInboxStore,
                                count: Int, file: StaticString = #filePath, line: UInt = #line) async throws -> [FeedMessage] {
        for _ in 0..<200 {
            let msgs = provider.thread(for: conversation, now: Date())
            if msgs.count >= count { return msgs }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTFail("timed out waiting for \(count) message(s) in \(conversation)", file: file, line: line)
        throw TestTimeout()
    }

    func testInjectedTextPacketAppearsInInboxConversationForSender() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        let myNodeNum: UInt32 = 0x1234
        try await completeHandshake(transport: transport, client: client, myNodeNum: myNodeNum)

        let provider = InMemoryInboxStore()
        let sender: UInt32 = 0x02e6_06b0
        provider.registerMember(sender, displayName: "Firefly 1", initial: "F", colorIndex: 0)

        let vm = InboxViewModel(provider: provider, client: client)
        vm.observe()

        transport.inject(textFrame(from: sender, to: myNodeNum, id: 555, text: "WHERE ARE YOU"))

        let messages = try await waitForThread(.member(sender), on: provider, count: 1)

        XCTAssertEqual(messages.count, 1)
        XCTAssertEqual(messages.first?.text, "WHERE ARE YOU")
        XCTAssertEqual(messages.first?.senderID, sender)
        XCTAssertEqual(messages.first?.direction, .direct)
        XCTAssertEqual(messages.first?.packetID, 555)
        XCTAssertTrue(messages.first?.unread ?? false)

        // Never leaked into CREW — this was a direct message, not a
        // broadcast.
        XCTAssertTrue(provider.thread(for: .crew, now: Date()).isEmpty)
    }

    func testBroadcastTextPacketRoutesToCrew() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        let myNodeNum: UInt32 = 0x1234
        try await completeHandshake(transport: transport, client: client, myNodeNum: myNodeNum)

        let provider = InMemoryInboxStore()
        let vm = InboxViewModel(provider: provider, client: client)
        vm.observe()

        let sender: UInt32 = 0x02e6_06b0
        transport.inject(textFrame(from: sender, to: meshBroadcastAddress, id: 556, text: "HERE"))

        let messages = try await waitForThread(.crew, on: provider, count: 1)
        XCTAssertEqual(messages.first?.direction, .broadcast)
        XCTAssertEqual(messages.first?.senderID, sender)
    }

    /// The mesh reflects a self-originated broadcast back with the SAME
    /// packet id (A01, "Routing ACK -> delivery state": "Inbound text is
    /// deduplicated on packet.id... without the guard your own sent row
    /// is overwritten and a phantom notification fires"). `markSent` is
    /// the ONLY way a packet id enters `mySentPacketIDs` — seeded here
    /// directly, standing in for a real `ThreadViewModel.send` that
    /// already ran.
    func testOwnEchoIsDroppedByPacketID() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        let myNodeNum: UInt32 = 0x1234
        try await completeHandshake(transport: transport, client: client, myNodeNum: myNodeNum)

        let provider = InMemoryInboxStore()
        provider.markSent(outboxID: 1, packetID: 777, at: Date())

        let vm = InboxViewModel(provider: provider, client: client)
        vm.observe()

        transport.inject(textFrame(from: myNodeNum, to: meshBroadcastAddress, id: 777, text: "HERE"))

        // A short, fixed wait: there is no "it arrived" signal to poll
        // for when the point of the test is that nothing should arrive.
        // Long enough for the (missing, if the guard failed) row to have
        // shown up — `testBroadcastTextPacketRoutesToCrew` above shows a
        // real one lands well inside this window.
        try await Task.sleep(for: .milliseconds(150))

        XCTAssertTrue(provider.thread(for: .crew, now: Date()).isEmpty,
                       "an echo of my own sent packet id must be dropped, not pushed as a new inbound row")
    }

    /// A genuine message from someone else that happens to reuse a
    /// packet id I have never sent must NOT be caught by the echo guard
    /// — only MY OWN sent ids are ever in `mySentPacketIDs`
    /// (`InMemoryInboxStore.mySentPacketIDs`'s own doc comment).
    func testUnrelatedSenderReusingAnUnseenPacketIDIsKept() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        let myNodeNum: UInt32 = 0x1234
        try await completeHandshake(transport: transport, client: client, myNodeNum: myNodeNum)

        let provider = InMemoryInboxStore()
        provider.markSent(outboxID: 1, packetID: 42, at: Date()) // an id I sent — unrelated to 999 below

        let vm = InboxViewModel(provider: provider, client: client)
        vm.observe()

        let sender: UInt32 = 0x02e6_06b0
        transport.inject(textFrame(from: sender, to: meshBroadcastAddress, id: 999, text: "HERE"))

        let messages = try await waitForThread(.crew, on: provider, count: 1)
        XCTAssertEqual(messages.first?.packetID, 999)
    }
}
