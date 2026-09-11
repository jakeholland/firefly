//
//  DemoHistoryIsolationTests.swift — "Demo doesn't persist across
//  launches — in-memory store only" (docs/specs/A01-companion-app.md,
//  M3), plus `-FireflyDemoRestored`'s own seeded-restore path.
//
import FireflyMesh
import FireflyModel
import XCTest

@MainActor
final class DemoHistoryIsolationTests: XCTestCase {

    /// `AppGraph.init`'s own rule: `dependencies.store is
    /// InMemorySettingsStore` (true for both `.stub()` and every
    /// `.demoBundle()`) picks `HistoryStore.inMemory()` automatically.
    /// Proved here the same way `HistoryStoreTests
    /// .testTwoInMemoryStoresNeverShareData` proves it for the store
    /// alone: two independently-constructed graphs over the disposable
    /// stack never see each other's messages, because each one got its
    /// OWN fresh, throwaway store — not because anything was ever
    /// cleared.
    func testEachDemoOrStubGraphGetsItsOwnDisposableHistory() {
        let first = AppGraph(dependencies: .stub())
        first.inboxProvider.push(
            FeedMessage(id: 1, kind: .text, direction: .broadcast, text: "only in the first graph",
                        timestamp: Date()),
            into: .crew)
        XCTAssertEqual(first.inboxProvider.thread(for: .crew, now: Date()).count, 1)

        let second = AppGraph(dependencies: .stub())
        XCTAssertTrue(second.inboxProvider.thread(for: .crew, now: Date()).isEmpty,
                       "a fresh stub/demo graph never inherits another one's history — nothing persisted between them")
    }

    /// The bundle `AppDependencies.demoBundle()` returns is built the
    /// identical way `.demo()` is (that function's own doc comment) —
    /// checked directly here so this test does not depend on
    /// `FireflyApp`'s own simulator-only wiring to exercise the rule.
    func testDemoBundleDependenciesAlsoGetDisposableHistory() {
        let bundle = AppDependencies.demoBundle()
        let graph = AppGraph(dependencies: bundle.dependencies)
        graph.inboxProvider.push(
            FeedMessage(id: 1, kind: .text, direction: .broadcast, text: "demo traffic", timestamp: Date()),
            into: .crew)
        XCTAssertEqual(graph.inboxProvider.thread(for: .crew, now: Date()).count, 1)

        // A SECOND demo bundle — as a real relaunch of the simulator
        // with `-FireflyDemo` would build — starts empty.
        let secondBundle = AppDependencies.demoBundle()
        let secondGraph = AppGraph(dependencies: secondBundle.dependencies)
        XCTAssertTrue(secondGraph.inboxProvider.thread(for: .crew, now: Date()).isEmpty)
    }

    /// The live stack is the ONLY one that ever picks `HistoryStore
    /// .live()` — never touched by this test suite (no test here
    /// constructs `AppDependencies.live()` or `HistoryStore.live()`),
    /// which is itself the point: nothing in `swift test` may write to
    /// disk (A01_AC1's own "no hardware, no network beyond dependency
    /// resolution" rule, extended to "no real files" for the identical
    /// reason). This test only documents that boundary; it is not
    /// exercised by construction elsewhere in this file.
    func testLiveHistoryStoreIsNeverConstructedByThisSuite() {
        // Intentionally empty of any `.live()`/`AppDependencies.live()`
        // call — see this test's own doc comment.
    }

    // MARK: - `-FireflyDemoRestored`'s own seed path

    /// `DemoHistorySeed.seed(into:)` writes straight into an in-memory
    /// `HistoryStore` — `AppGraph.init(historyStore:)` then restores
    /// from that SAME store through the real `HistoryRestorer.restore`
    /// path, exactly like `FireflyApp.init`'s own `-FireflyDemoRestored`
    /// wiring does. Pinned directly here (no simulator, no launch
    /// argument, no UI) so this rule is `swift test`-checkable.
    func testDemoHistorySeedRestoresIntoASeededGraphWithHonestNoAck() {
        let history = HistoryStore.inMemory()
        DemoHistorySeed.seed(into: history)

        let graph = AppGraph(dependencies: .demo(), historyStore: history)
        let thread = graph.inboxProvider.thread(for: .member(DemoCrew.taylor), now: Date())
            .sorted { $0.timestamp < $1.timestamp }

        XCTAssertEqual(thread.count, 2)
        XCTAssertTrue(thread.allSatisfy(\.isRestored), "every seeded message renders as restored, not live")
        XCTAssertEqual(thread[0].direction, .direct)
        XCTAssertEqual(thread[1].direction, .out)
        XCTAssertEqual(thread[1].deliveryState, .noAck,
                        "the seed records SENT — HistoryRestorer is what turns it into NO ACK on the way back in")
    }

    /// Regression (bench-reproduced while building `DemoHistorySeed`):
    /// `ThreadViewModel`'s own `OutboxIDGenerator.shared` is a
    /// process-global singleton starting at outbox id 1, and
    /// `DemoRunner.sendDemoThreadMessages()` sends live compose messages
    /// through it in the SAME process a seeded restore just ran in. A
    /// seeded outbound message with `id: 1` collided with the live
    /// send's own outbox id: `markSent`/`setStatus(outboxID:)` updated
    /// whichever item the C core found first, silently flipping the
    /// seed's honest NO ACK to the live send's DELIVERED. This pins the
    /// fix: a live push reusing outbox id 1 must never touch the
    /// seeded message's own (very differently numbered) id.
    func testALiveOutboxIDCollisionWithTheGeneratorsOwnStartingValueNeverTouchesSeededHistory() {
        let history = HistoryStore.inMemory()
        DemoHistorySeed.seed(into: history)
        let graph = AppGraph(dependencies: .demo(), historyStore: history)

        let before = graph.inboxProvider.thread(for: .member(DemoCrew.taylor), now: Date())
        let seeded = try! XCTUnwrap(before.first { $0.direction == .out })
        XCTAssertNotEqual(seeded.id, 1, "the seed must never reuse OutboxIDGenerator's own first value")
        XCTAssertEqual(seeded.deliveryState, .noAck)

        // The exact collision scenario: a live send mints outbox id 1
        // (`OutboxIDGenerator`'s real starting value) into the SAME
        // conversation, then resolves DELIVERED — mirroring
        // `ThreadViewModel.attemptSend`/`DemoRunner`'s own live path.
        let now = Date()
        graph.inboxProvider.push(
            FeedMessage(id: 1, kind: .text, direction: .out, text: "on my way", timestamp: now,
                        destination: DemoCrew.taylor, packetID: 1, deliveryState: .waiting, statusAt: now),
            into: .member(DemoCrew.taylor))
        graph.inboxProvider.markSent(outboxID: 1, packetID: 1, at: now)
        graph.inboxProvider.setStatus(packetID: 1, state: .delivered, at: now)

        let after = graph.inboxProvider.thread(for: .member(DemoCrew.taylor), now: Date())
        let stillSeeded = try! XCTUnwrap(after.first { $0.id == seeded.id })
        XCTAssertEqual(stillSeeded.deliveryState, .noAck,
                        "the seeded message's own status must be untouched by an unrelated live send")
        let live = try! XCTUnwrap(after.first { $0.id == 1 })
        XCTAssertEqual(live.deliveryState, .delivered, "the live send resolves independently")
    }

    /// The demo world's own scripted timeline (`DemoRunner`) is
    /// unaffected by a seeded history in a DIFFERENT conversation — the
    /// two coexist in the same ring rather than one silently replacing
    /// the other.
    func testSeededHistoryCoexistsWithFreshLiveTraffic() {
        let history = HistoryStore.inMemory()
        DemoHistorySeed.seed(into: history)
        let graph = AppGraph(dependencies: .demo(), historyStore: history)

        graph.inboxProvider.push(
            FeedMessage(id: 0x9000_0000_0000_0001, kind: .text, direction: .broadcast, text: "fresh crew chatter",
                        timestamp: Date()),
            into: .crew)

        let crew = graph.inboxProvider.thread(for: .crew, now: Date())
        XCTAssertEqual(crew.count, 1)
        XCTAssertFalse(crew[0].isRestored, "live traffic pushed after construction is never mistaken for restored")

        let taylor = graph.inboxProvider.thread(for: .member(DemoCrew.taylor), now: Date())
        XCTAssertEqual(taylor.count, 2, "the seeded thread is untouched by traffic in a different conversation")
    }
}
