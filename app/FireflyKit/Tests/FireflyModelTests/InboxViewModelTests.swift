//
//  InboxViewModelTests.swift — conversation ordering, unread counts,
//  previews, presence tagging, and the preview-truncation UTF-8 rule
//  (docs/specs/A01-companion-app.md, slice E; docs/specs/
//  S24-signals-inbox.md).
//
import FireflyCore
import FireflyMesh
import FireflyModel
import Foundation
import XCTest

/// `StubMeshtasticClient` deliberately never yields an `IncomingText`
/// (its own header comment: "NEVER invents... incoming messages" —
/// tests that need one inject exact bytes). `InboxViewModel.ingest(_:)`'s
/// `to`-routing (PR #271 review, SHOULD-FIX 2) needs exactly that, so
/// this tiny double exists purely to be able to yield one — every other
/// stream is empty and every send is a no-op, same "record nothing,
/// fabricate nothing beyond what the test injects" shape `CountingClient`
/// (`AppGraphTests.swift`) uses for the same reason.
private final class TextInjectingClient: MeshtasticClientProtocol, @unchecked Sendable {
    private let linkHub = EventHub<LinkState>()
    private let nodeHub = EventHub<MeshNodeSnapshot>()
    private let deliveryHub = EventHub<DeliveryEvent>()
    private let textHub = EventHub<IncomingText>()
    private let privateHub = EventHub<IncomingPrivate>()

    func linkState() -> AsyncStream<LinkState> { linkHub.subscribe() }
    func nodeUpdates() -> AsyncStream<MeshNodeSnapshot> { nodeHub.subscribe() }
    func deliveryUpdates() -> AsyncStream<DeliveryEvent> { deliveryHub.subscribe() }
    func incomingTexts() -> AsyncStream<IncomingText> { textHub.subscribe() }
    func incomingPrivate() -> AsyncStream<IncomingPrivate> { privateHub.subscribe() }
    var connectedNodeNum: UInt32? { nil }

    func connect() async throws {}
    func disconnect() async {}
    @discardableResult
    func sendText(_ text: String, to destination: UInt32, wantAck: Bool) async throws -> UInt32 { 0 }
    @discardableResult
    func sendPosition(_ fix: ExternalPositionFix, to destination: UInt32) async throws -> UInt32 { 0 }
    @discardableResult
    func sendPrivate(_ payload: Data, to destination: UInt32, wantAck: Bool) async throws -> UInt32 { 0 }

    func yieldText(_ text: IncomingText) { textHub.yield(text) }
}

@MainActor
final class InboxViewModelTests: XCTestCase {

    // MARK: - Conversation list: membership, unread, previews, direction

    func testCrewConversationAlwaysPresentEvenWithNoTraffic() {
        let store = InMemoryInboxStore()
        let now = Date()
        let convs = store.conversations(now: now)
        XCTAssertEqual(convs.count, 1)
        XCTAssertEqual(convs[0].kind, .crew)
        XCTAssertFalse(convs[0].hasPreview)
        XCTAssertEqual(convs[0].unreadCount, 0)
    }

    func testOneRowPerPairedMemberPlusCrew() {
        let store = InMemoryInboxStore()
        store.registerMember(11, displayName: "Riley", initial: "R", colorIndex: 0)
        store.registerMember(22, displayName: "Dana", initial: "D", colorIndex: 1)
        let convs = store.conversations(now: Date())
        XCTAssertEqual(Set(convs.map(\.kind)), [.crew, .member(11), .member(22)])
    }

    func testUnreadCountAndPreviewFromInjectedFeedItems() {
        let store = InMemoryInboxStore()
        store.registerMember(11, displayName: "Riley", initial: "R", colorIndex: 0)
        let now = Date()
        store.push(FeedMessage(id: 1, kind: .text, direction: .direct, senderID: 11, senderName: "Riley",
                                text: "on my way", timestamp: now.addingTimeInterval(-60), unread: true),
                    into: .member(11))
        store.push(FeedMessage(id: 2, kind: .text, direction: .direct, senderID: 11, senderName: "Riley",
                                text: "at the gate", timestamp: now.addingTimeInterval(-10), unread: true),
                    into: .member(11))

        let convs = store.conversations(now: now)
        let riley = convs.first { $0.kind == .member(11) }!
        XCTAssertEqual(riley.unreadCount, 2)
        XCTAssertEqual(riley.itemCount, 2)
        XCTAssertTrue(riley.hasPreview)
        XCTAssertEqual(riley.previewText, "at the gate") // newest, not oldest
        XCTAssertEqual(riley.previewDirection, .direct)
        XCTAssertEqual(riley.previewFromName, "Riley")
    }

    func testOutgoingPreviewCarriesNoSenderName() {
        let store = InMemoryInboxStore()
        store.push(FeedMessage(id: 1, kind: .text, direction: .out, text: "omw", timestamp: Date(),
                                destination: meshBroadcastAddress, deliveryState: .sent),
                    into: .crew)
        let crew = store.conversations(now: Date()).first { $0.kind == .crew }!
        XCTAssertEqual(crew.previewDirection, .out)
        XCTAssertNil(crew.previewFromName, "an outgoing preview's sender is this device — no name to show")
        XCTAssertEqual(crew.previewDeliveryState, .sent)
    }

    // MARK: - Ordering (S24 AC2)

    func testUnreadConversationsSortBeforeReadOnes() {
        let store = InMemoryInboxStore()
        store.registerMember(1, displayName: "A", initial: "A", colorIndex: 0)
        store.registerMember(2, displayName: "B", initial: "B", colorIndex: 1)
        let now = Date()
        // Member 1: read traffic, very recent.
        store.push(FeedMessage(id: 1, kind: .text, direction: .direct, senderID: 1, text: "hi",
                                timestamp: now.addingTimeInterval(-5), unread: false), into: .member(1))
        // Member 2: unread traffic, older.
        store.push(FeedMessage(id: 2, kind: .text, direction: .direct, senderID: 2, text: "hey",
                                timestamp: now.addingTimeInterval(-500), unread: true), into: .member(2))

        let convs = store.conversations(now: now)
        let order = convs.map(\.kind)
        XCTAssertEqual(order.firstIndex(of: .member(2)).map { order.firstIndex(of: .member(1))! > $0 }, true,
                        "the unread conversation must sort ahead of the read one despite being older")
    }

    func testQuietConversationsOrderByPresenceFreshnessCrewFirst() {
        let store = InMemoryInboxStore()
        store.registerMember(1, displayName: "Heard", initial: "H", colorIndex: 0)
        store.registerMember(2, displayName: "Stale", initial: "S", colorIndex: 1)
        store.registerMember(3, displayName: "Lost", initial: "L", colorIndex: 2)
        store.registerMember(4, displayName: "Linked", initial: "K", colorIndex: 3)
        store.setPresence(.heard, ageMS: 5_000, for: 1)
        store.setPresence(.stale, ageMS: 200_000, for: 2)
        store.setPresence(.lost, ageMS: 900_000, for: 3)
        store.setPresence(.linked, ageMS: nil, for: 4)

        let convs = store.conversations(now: Date())
        XCTAssertEqual(convs.map(\.kind), [.crew, .member(1), .member(2), .member(3), .member(4)],
                        "quiet rows: CREW first, then freshest-heard first, LINKED last")
    }

    func testQuietMembersTieBreakByAscendingNodeID() {
        let store = InMemoryInboxStore()
        store.registerMember(50, displayName: "Fifty", initial: "F", colorIndex: 0)
        store.registerMember(10, displayName: "Ten", initial: "T", colorIndex: 1)
        store.setPresence(.linked, ageMS: nil, for: 50)
        store.setPresence(.linked, ageMS: nil, for: 10)
        let convs = store.conversations(now: Date())
        XCTAssertEqual(convs.map(\.kind), [.crew, .member(10), .member(50)])
    }

    // MARK: - Presence tagging (the heard axis)

    func testPresenceTagBoundariesMatchFfCrewThresholds() {
        XCTAssertEqual(PresenceTag.classify(everHeard: true, heardAgeMS: 0), .heard)
        XCTAssertEqual(PresenceTag.classify(everHeard: true, heardAgeMS: PresenceTag.heardLiveMS - 1), .heard)
        XCTAssertEqual(PresenceTag.classify(everHeard: true, heardAgeMS: PresenceTag.heardLiveMS), .stale,
                        "inclusive-toward-STALE at the live boundary, per ff_crew.h")
        XCTAssertEqual(PresenceTag.classify(everHeard: true, heardAgeMS: PresenceTag.heardLostMS), .stale,
                        "inclusive-toward-STALE at the lost boundary too")
        XCTAssertEqual(PresenceTag.classify(everHeard: true, heardAgeMS: PresenceTag.heardLostMS + 1), .lost)
        XCTAssertEqual(PresenceTag.classify(everHeard: false, heardAgeMS: 999_999), .linked,
                        "never heard -> LINKED, never a fabricated recent age")
    }

    /// Cross-checks this app's HEARD/STALE/LOST/LINKED tag against the
    /// real `ff_sigview_presence` C function (`firmware/core/include/
    /// ff_sigview.h`) so a drift between the two vocabularies fails here,
    /// not in the field.
    func testPresenceTagAgreesWithFfSigviewPresence() {
        func sigview(_ heard: ff_crew_presence_t, _ ageMS: UInt32) -> ff_sigview_presence_t {
            var out: UInt32 = 0
            return ff_sigview_presence(heard, ageMS, &out)
        }
        let heard = PresenceTag.classify(everHeard: true, heardAgeMS: 1_000)
        XCTAssertEqual(heard.ffSigviewPresence, sigview(FF_CREW_PRESENCE_HEARD, 1_000))
        XCTAssertEqual(heard.ffSigviewPresence, FF_PRESENCE_SEEN)

        let stale = PresenceTag.classify(everHeard: true, heardAgeMS: 300_000)
        XCTAssertEqual(stale.ffSigviewPresence, sigview(FF_CREW_PRESENCE_STALE, 300_000))
        XCTAssertEqual(stale.ffSigviewPresence, FF_PRESENCE_SEEN)

        let lost = PresenceTag.classify(everHeard: true, heardAgeMS: 900_000)
        XCTAssertEqual(lost.ffSigviewPresence, sigview(FF_CREW_PRESENCE_LOST, 900_000))
        XCTAssertEqual(lost.ffSigviewPresence, FF_PRESENCE_LOST)

        let linked = PresenceTag.classify(everHeard: false, heardAgeMS: 0)
        XCTAssertEqual(linked.ffSigviewPresence, sigview(FF_CREW_PRESENCE_NEVER, 0))
        XCTAssertEqual(linked.ffSigviewPresence, FF_PRESENCE_LINKED)
    }

    // MARK: - Preview truncation, UTF-8 / grapheme-cluster safety

    func testPreviewTruncationLeavesShortTextUntouched() {
        XCTAssertEqual(InboxText.preview("omw", maxLength: 42), "omw")
    }

    func testPreviewTruncationAddsEllipsisPastTheLimit() {
        let long = String(repeating: "a", count: 50)
        let preview = InboxText.preview(long, maxLength: 42)
        XCTAssertEqual(preview.count, 43) // 42 characters + the ellipsis
        XCTAssertTrue(preview.hasSuffix("…"))
    }

    func testPreviewTruncationNeverSplitsAMultiScalarGraphemeCluster() {
        // A family emoji is FOUR scalars joined by ZWJ — one Character.
        // Padding it to land exactly on the truncation boundary must not
        // produce a dangling half-cluster or an invalid String.
        let family = "\u{1F468}\u{200D}\u{1F469}\u{200D}\u{1F467}\u{200D}\u{1F466}" // 👨‍👩‍👧‍👦
        let text = String(repeating: "x", count: 41) + family + "y"
        let preview = InboxText.preview(text, maxLength: 42)
        // Either the cluster is wholly present or wholly absent — never
        // torn, and the result must still be a valid, decodable String.
        XCTAssertTrue(preview.hasSuffix("…"))
        XCTAssertFalse(preview.unicodeScalars.contains(where: { $0.value == 0 }))
        for character in preview where character != "…" {
            XCTAssertFalse(character.unicodeScalars.isEmpty)
        }
    }

    func testPreviewTruncationHandlesFlagEmojiAtTheBoundary() {
        let flag = "🇺🇸" // two regional-indicator scalars, one Character
        let text = String(repeating: "x", count: 41) + flag
        let preview = InboxText.preview(text, maxLength: 42)
        XCTAssertEqual(preview.count, 42, "the flag fits exactly at the limit and needs no ellipsis")
        XCTAssertFalse(preview.hasSuffix("…"))
    }

    /// NIT 8 on this PR: a combining mark (base + U+0301 COMBINING ACUTE
    /// ACCENT) is two scalars but ONE grapheme cluster/`Character`, the
    /// same "never torn mid-cluster" rule as the ZWJ family and flag
    /// cases above — different Unicode mechanism (combining, not ZWJ or
    /// regional-indicator pairing), same truncation guarantee.
    func testPreviewTruncationNeverSplitsACombiningMarkCluster() {
        let eAcute = "e\u{0301}" // "é" as two scalars, one Character
        let text = String(repeating: "x", count: 41) + eAcute + "y"
        let preview = InboxText.preview(text, maxLength: 42)
        // Either the whole combining-mark cluster is present or wholly
        // absent — never a bare base character with its accent torn off.
        XCTAssertTrue(preview.hasSuffix("…"))
        XCTAssertFalse(preview.unicodeScalars.contains(where: { $0.value == 0 }))
        for character in preview where character != "…" {
            XCTAssertFalse(character.unicodeScalars.isEmpty)
        }
    }

    // MARK: - InboxViewModel: refresh + delivery-state wiring

    func testObserveRefreshesFromTheProviderAndDeliveryUpdatesFlow() async {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let vm = InboxViewModel(provider: store, client: client)
        vm.observe()
        XCTAssertEqual(vm.conversations.count, 1) // CREW only, nothing pushed yet

        store.push(FeedMessage(id: 1, kind: .text, direction: .out, text: "omw", timestamp: Date(),
                                destination: meshBroadcastAddress, packetID: 7, deliveryState: .sent),
                    into: .crew)
        store.registerMember(9, displayName: "Nine", initial: "N", colorIndex: 0)

        // The stub client never fabricates deliveries, but the store
        // supports the same setStatus(packetID:) path a real client's
        // deliveryUpdates() would drive; call the setter directly here
        // and confirm InboxViewModel.refresh() reflects it (ThreadViewModelTests
        // exercises the live client.deliveryUpdates() subscription end to end).
        store.setStatus(packetID: 7, state: .delivered, at: Date())
        vm.refresh()
        let crew = vm.conversations.first { $0.kind == .crew }!
        XCTAssertEqual(crew.previewDeliveryState, .delivered)

        vm.stopObserving()
    }

    func testOpenThreadMarksItReadAndReturnsAThreadViewModelForTheSameConversation() {
        let store = InMemoryInboxStore()
        let client = StubMeshtasticClient()
        let vm = InboxViewModel(provider: store, client: client)
        store.registerMember(3, displayName: "Three", initial: "T", colorIndex: 0)
        store.push(FeedMessage(id: 1, kind: .text, direction: .direct, senderID: 3, text: "hi",
                                timestamp: Date(), unread: true), into: .member(3))
        vm.refresh()
        XCTAssertEqual(vm.conversations.first { $0.kind == .member(3) }?.unreadCount, 1)

        let thread = vm.openThread(.member(3))
        XCTAssertEqual(thread.conversation, .member(3))
        XCTAssertEqual(vm.conversations.first { $0.kind == .member(3) }?.unreadCount, 0)
    }

    // MARK: - ingest(_:) broadcast routing (PR #271 review, SHOULD-FIX 2)

    /// `to == 0` is protobuf's zero-default for an unset field, not a
    /// real broadcast — must route exactly like a direct message would,
    /// same as `AppGraph.pushInboundFeedItem`'s own `to == 0` test
    /// (`AppGraphTests.testStatusAddressedToZeroRoutesAsDirectNotBroadcast`)
    /// now that both call the one shared `isBroadcastDestination` helper.
    func testIngestRoutesToZeroAsDirectNotBroadcast() async {
        let store = InMemoryInboxStore()
        let client = TextInjectingClient()
        let vm = InboxViewModel(provider: store, client: client)
        vm.observe()

        client.yieldText(IncomingText(from: 0x0000_5001, to: 0, channel: 0, packetID: 500,
                                       text: "hi", rxTime: Date(), rssiDbm: nil, snrDb: nil, direct: nil))

        var attempts = 0
        while store.thread(for: .member(0x0000_5001), now: Date()).isEmpty && attempts < 400 {
            try? await Task.sleep(nanoseconds: 5_000_000)
            attempts += 1
        }

        XCTAssertFalse(store.thread(for: .member(0x0000_5001), now: Date()).isEmpty,
                        "to == 0 must route like a direct message, not vanish into CREW")
        XCTAssertTrue(store.thread(for: .crew, now: Date()).isEmpty,
                       "to == 0 is not a real broadcast — must not land in CREW")

        vm.stopObserving()
    }

    /// The wire's actual broadcast address still routes to CREW, exactly
    /// as before — the shared helper changes nothing about this case.
    func testIngestStillRoutesTheRealBroadcastAddressToCrew() async {
        let store = InMemoryInboxStore()
        let client = TextInjectingClient()
        let vm = InboxViewModel(provider: store, client: client)
        vm.observe()

        client.yieldText(IncomingText(from: 0x0000_5002, to: meshBroadcastAddress, channel: 0, packetID: 501,
                                       text: "hey crew", rxTime: Date(), rssiDbm: nil, snrDb: nil, direct: nil))

        var attempts = 0
        while store.thread(for: .crew, now: Date()).isEmpty && attempts < 400 {
            try? await Task.sleep(nanoseconds: 5_000_000)
            attempts += 1
        }

        XCTAssertFalse(store.thread(for: .crew, now: Date()).isEmpty, "the real broadcast address must still route to CREW")

        vm.stopObserving()
    }
}
