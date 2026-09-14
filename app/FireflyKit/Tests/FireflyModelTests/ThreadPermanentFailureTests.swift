//
//  ThreadPermanentFailureTests.swift — hardening QA pass.
//
//  `ThreadViewModel.attemptSend` requeued on ANY thrown error, on the
//  reasoning that a failed send is a transient link problem. That is
//  true of every error the client threw when the code was written — and
//  stopped being true the moment `sendText` learned to refuse a payload
//  the mesh cannot carry (`MeshtasticClientError.payloadTooLarge`).
//
//  A permanent failure in a retry queue is its own bug: the outbox is
//  bounded at 8 and drops OLDEST, so one un-sendable message would be
//  retried on every reconnect for the rest of the session while
//  evicting real messages queued behind it — and the person who wrote
//  it would never be told why it never went.
//
import FireflyMesh
import Foundation
import MeshtasticProto
import XCTest
@testable import FireflyModel

/// Throws a caller-chosen error from `sendText`, and nothing else.
private final class ThrowingMockClient: MeshtasticClientProtocol, @unchecked Sendable {
    private let linkHub = EventHub<LinkState>()
    private let error: any Error
    private let lock = NSLock()
    private var _attempts = 0
    var attempts: Int { lock.lock(); defer { lock.unlock() }; return _attempts }

    init(error: any Error) { self.error = error }

    func linkState() -> AsyncStream<LinkState> { linkHub.subscribe() }
    func nodeUpdates() -> AsyncStream<MeshNodeSnapshot> { EventHub<MeshNodeSnapshot>().subscribe() }
    func deliveryUpdates() -> AsyncStream<DeliveryEvent> { EventHub<DeliveryEvent>().subscribe() }
    func incomingTexts() -> AsyncStream<IncomingText> { EventHub<IncomingText>().subscribe() }
    func incomingPrivate() -> AsyncStream<IncomingPrivate> { EventHub<IncomingPrivate>().subscribe() }
    var connectedNodeNum: UInt32? { nil }
    func connect() async throws { linkHub.yield(.ready) }
    func disconnect() async { linkHub.yield(.disconnected) }

    @discardableResult
    func sendText(_ text: String, to destination: UInt32, wantAck: Bool) async throws -> UInt32 {
        recordAttempt()
        throw error
    }

    // Non-async on purpose — `NSLock` may not be held across a
    // suspension point, the same convention `OrderingMockClient` and
    // `LoopbackTransport.record(_:)` already follow.
    private func recordAttempt() {
        lock.lock(); _attempts += 1; lock.unlock()
    }

    @discardableResult
    func sendPosition(_ fix: ExternalPositionFix, to destination: UInt32) async throws -> UInt32 { throw error }
    @discardableResult
    func sendPrivate(_ payload: Data, to destination: UInt32, wantAck: Bool) async throws -> UInt32 { throw error }
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
}

@MainActor
final class ThreadPermanentFailureTests: XCTestCase {

    private func waitUntil(_ condition: @escaping () -> Bool, timeout: Int = 200) async {
        for _ in 0..<timeout where !condition() {
            try? await Task.sleep(nanoseconds: 5_000_000)
        }
    }

    private func makeVM(error: any Error) -> (ThreadViewModel, ThrowingMockClient) {
        let client = ThrowingMockClient(error: error)
        let vm = ThreadViewModel(conversation: .crew, provider: InMemoryInboxStore(), client: client)
        return (vm, client)
    }

    /// The classification itself, stated once: only the two LOCAL
    /// failures are permanent. A link problem is not.
    func testOnlyLocalFailuresCountAsPermanent() {
        XCTAssertTrue(ThreadViewModel.isPermanent(.payloadTooLarge(bytes: 400, max: 233)))
        XCTAssertTrue(ThreadViewModel.isPermanent(.encodingFailed))
        XCTAssertFalse(ThreadViewModel.isPermanent(.handshakeTimeout(phase: 69420)))
        XCTAssertFalse(ThreadViewModel.isPermanent(.alreadyConnecting))
        XCTAssertFalse(ThreadViewModel.isPermanent(.invalidPositionFix))
    }

    func testATooLongMessageIsDroppedVisiblyAndNeverEntersTheRetryQueue() async {
        let (vm, client) = makeVM(error: MeshtasticClientError.payloadTooLarge(bytes: 400, max: 233))
        vm.observe()
        try? await client.connect()
        await waitUntil { vm.isLinkReady }

        vm.composeText = String(repeating: "x", count: 400)
        await vm.sendCompose()
        await waitUntil { vm.messages.last?.deliveryState == .dropped }

        XCTAssertEqual(vm.messages.last?.deliveryState, .dropped,
                       "the person who wrote it must be able to see it did not go, and shorten it")
        XCTAssertEqual(vm.queuedCount, 0,
                       "a message that can never succeed must not occupy a slot in a bounded retry queue")
        XCTAssertEqual(client.attempts, 1, "and must not be retried")
        vm.stopObserving()
    }

    /// PR #304 review: the "Couldn't send \u{00B7} too long" row this PR
    /// added had no test at all — `dropReasonText(for:)` is new public
    /// API. `payloadTooLarge` is the one reason this app can honestly
    /// name; everything else stays `nil`, so the tag falls back to a
    /// plain "Couldn't send" rather than inventing a cause.
    func testAPayloadTooLargeDropNamesItsReasonAndNothingElseDoes() async {
        let (vm, client) = makeVM(error: MeshtasticClientError.payloadTooLarge(bytes: 400, max: 233))
        vm.observe()
        try? await client.connect()
        await waitUntil { vm.isLinkReady }
        vm.composeText = String(repeating: "x", count: 400)
        await vm.sendCompose()
        await waitUntil { vm.messages.last?.deliveryState == .dropped }
        let dropped = try? XCTUnwrap(vm.messages.last)
        XCTAssertEqual(dropped.map { vm.dropReasonText(for: $0) }, "too long")
        vm.stopObserving()

        let (other, otherClient) = makeVM(error: MeshtasticClientError.encodingFailed)
        other.observe()
        try? await otherClient.connect()
        await waitUntil { other.isLinkReady }
        other.composeText = "hi"
        await other.sendCompose()
        await waitUntil { other.messages.last?.deliveryState == .dropped }
        let encodingDrop = try? XCTUnwrap(other.messages.last)
        XCTAssertNil(encodingDrop.flatMap { other.dropReasonText(for: $0) },
                     "encodingFailed has no honest plain-language reason — never invent one")
        other.stopObserving()
    }

    /// The other half — the behaviour that must NOT have regressed. A
    /// transient failure still queues for retry, which is the whole
    /// reason the outbox exists at a festival where the link comes and
    /// goes.
    func testATransientFailureStillQueuesForRetry() async {
        let (vm, client) = makeVM(error: MeshtasticClientError.handshakeTimeout(phase: 69420))
        vm.observe()
        try? await client.connect()
        await waitUntil { vm.isLinkReady }

        vm.composeText = "omw"
        await vm.sendCompose()
        await waitUntil { vm.queuedCount == 1 }

        XCTAssertEqual(vm.queuedCount, 1, "a link problem is exactly what the outbox is for")
        XCTAssertNotEqual(vm.messages.last?.deliveryState, .dropped,
                          "a transient failure must not be reported as a permanent one")
        vm.stopObserving()
    }

    /// A non-`MeshtasticClientError` (anything a transport can throw)
    /// stays transient — the classification must not accidentally
    /// swallow errors it has never seen.
    func testAnUnknownErrorIsTreatedAsTransient() async {
        struct SomeTransportError: Error {}
        let (vm, client) = makeVM(error: SomeTransportError())
        vm.observe()
        try? await client.connect()
        await waitUntil { vm.isLinkReady }

        vm.composeText = "omw"
        await vm.sendCompose()
        await waitUntil { vm.queuedCount == 1 }

        XCTAssertEqual(vm.queuedCount, 1)
        vm.stopObserving()
    }
}
