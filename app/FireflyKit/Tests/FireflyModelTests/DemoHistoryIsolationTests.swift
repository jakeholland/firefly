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

    /// PR #281 review, BLOCKING 1 superseded this test (formerly
    /// `testALiveOutboxIDCollisionWithTheGeneratorsOwnStartingValueNeverTouchesSeededHistory`):
    /// it pinned the OLD workaround — `DemoHistorySeed` hand-picking a
    /// large, reserved, out-of-range constant id specifically so it
    /// could never collide with `OutboxIDGenerator.shared`'s own real
    /// starting value — by asserting a manually-pushed `id: 1` never
    /// touched the seed. `DemoHistorySeed` no longer needs that
    /// constant (its own doc comment explains why, now that
    /// `AppGraph.init` seeds `OutboxIDGenerator.shared` from persisted
    /// history before any live id can be minted at all), so this file's
    /// seed now mints its own id from that SAME real, process-global
    /// `.shared` generator — making this test's own "manually push
    /// `id: 1`, assert the seed's id is never `1`" premise flaky by
    /// construction (`.shared`'s ambient counter value depends on
    /// whatever else has run earlier in this same test binary). The
    /// generalized regression this test existed to pin — two
    /// independent generator lifetimes over the SAME store never
    /// collide, and a restored NO ACK never reverts to SENT when a new
    /// session sends — now lives in `AppGraphTests
    /// .testTwoIndependentAppGraphLifetimesOverTheSameHistoryNeverCollide`,
    /// which injects genuinely fresh generator instances (never
    /// `.shared`'s own ambient, cross-test state) to reproduce it
    /// deterministically.

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
