//
//  DemoRunnerTests.swift — S20's honesty rule, pinned at the app
//  level: the demo timeline drives the REAL `CoreStore`/`ff_crew`/
//  `ff_feed` bridges (never a stand-in), and the view models built on
//  top of them end up in the states `docs/specs/S20-demo-mode.md`
//  promises — Taylor LIVE, Sam STALE, Mo LOST, CAMP an asserted PLACE,
//  a heard-only stranger nobody paired, a Thread showing DELIVERED and
//  NO ACK, and the no-GPS signal view actually falling back to
//  RADAR_SIGNAL when the phone's own fix is withdrawn.
//
import FireflyMesh
import FireflyModel
import XCTest

@MainActor
final class DemoRunnerTests: XCTestCase {

    private func waitUntil(_ condition: @escaping () -> Bool, timeout: Int = 600) async {
        for _ in 0..<timeout where !condition() {
            try? await Task.sleep(nanoseconds: 5_000_000)
        }
    }

    private func makeRunner() -> (AppGraph, DemoRunner, InboxViewModel, RadarViewModel) {
        let bundle = AppDependencies.demoBundle()
        let graph = AppGraph(dependencies: bundle.dependencies)
        let connect = graph.makeConnectViewModel()
        let inbox = graph.makeInboxViewModel()
        let radar = graph.makeRadarViewModel()
        let runner = DemoRunner(graph: graph, client: bundle.client, location: bundle.location,
                                 heading: bundle.heading, connect: connect, inbox: inbox, radar: radar)
        return (graph, runner, inbox, radar)
    }

    func testDemoTimelinePopulatesEveryRadarStateThroughTheRealCore() async {
        let (graph, runner, _, radar) = makeRunner()
        await graph.start()
        await runner.start()
        radar.observe()

        // Taylor, Dana, Sam, the stranger: the scripted nodeDB dump.
        await waitUntil { graph.core.crew.count >= 4 }
        let now = FireflyClock.nowMillis()

        let taylor = graph.core.crew.member(nodeID: DemoCrew.taylor, now: now)
        XCTAssertEqual(taylor?.paired, true)
        XCTAssertNotNil(taylor?.position, "Taylor is LIVE and must carry a real position")

        let dana = graph.core.crew.member(nodeID: DemoCrew.dana, now: now)
        XCTAssertEqual(dana?.paired, true)

        let sam = graph.core.crew.member(nodeID: DemoCrew.sam, now: now)
        XCTAssertEqual(sam?.paired, true)
        XCTAssertNotNil(sam?.position)

        // Mo: paired, but never in the nodeDB dump — the honest LOST
        // case (`DemoRunner`'s own header comment).
        let mo = graph.core.crew.member(nodeID: DemoCrew.mo, now: now)
        XCTAssertEqual(mo?.paired, true)
        XCTAssertNil(mo?.position)

        // The stranger: heard (has an RSSI-bearing entry) but NEVER
        // paired — S20's "one heard-only stranger", never selectable.
        let strangerBefore = graph.core.crew.selected(now: now)?.nodeID
        graph.core.crew.selectNode(DemoCrew.stranger)
        XCTAssertEqual(graph.core.crew.selected(now: now)?.nodeID, strangerBefore,
                        "an unpaired node must never become the selection")

        // CAMP: an asserted landmark — RADAR_PLACE, never an age.
        graph.core.crew.selectNode(DemoCrew.camp)
        await waitUntil { radar.snapshot.mode == .place }
        XCTAssertEqual(radar.snapshot.mode, .place)

        // Sam: STALE.
        graph.core.crew.selectNode(DemoCrew.sam)
        await waitUntil { radar.snapshot.mode == .stale }
        XCTAssertEqual(radar.snapshot.mode, .stale)

        // Mo: LOST.
        graph.core.crew.selectNode(DemoCrew.mo)
        await waitUntil { radar.snapshot.mode == .lost }
        XCTAssertEqual(radar.snapshot.mode, .lost)

        // Taylor: LIVE, the default selection.
        graph.core.crew.selectNode(DemoCrew.taylor)
        await waitUntil { radar.snapshot.mode == .live }
        XCTAssertEqual(radar.snapshot.mode, .live)

        radar.stopObserving()
        await graph.stop()
    }

    /// The "Radar no-GPS signal view" screenshot's own precondition:
    /// withdrawing the phone's fix must fall back to RADAR_SIGNAL for a
    /// heard, paired member — never a special-cased "signal mode" flag,
    /// just what `ff_radar_compute` honestly does with `my_pos_ok ==
    /// false` (`firmware/core/src/ff_radar.c`).
    func testWithdrawingThePhoneFixFallsBackToSignalMode() async {
        let (graph, runner, _, radar) = makeRunner()
        await graph.start()
        await runner.start()
        radar.observe()

        await waitUntil { graph.core.crew.count >= 4 }
        graph.core.crew.selectNode(DemoCrew.taylor)
        await waitUntil { radar.snapshot.mode == .live }

        runner.withdrawPhoneFix()
        await waitUntil { radar.snapshot.mode == .signal }
        XCTAssertEqual(radar.snapshot.mode, .signal)
        // `signalDots` is the RING of OTHER paired-but-position-less
        // members (`ff_radar.c`'s own gate: `!m->paired || m->has_pos ||
        // !m->has_heard` skips it) — legitimately empty in this world,
        // since every OTHER member either has a position (Taylor, Dana,
        // Sam, CAMP) or has never been heard (Mo). The selected member's
        // OWN tier is the thing this screenshot needs, and it must not
        // be NONE now that a real, direct RSSI sample is on file.
        XCTAssertNotEqual(radar.snapshot.signalTier, .none,
                          "the no-GPS signal view needs an actual tier for the selected member")

        runner.restorePhoneFix()
        await waitUntil { radar.snapshot.mode == .live }
        XCTAssertEqual(radar.snapshot.mode, .live, "restoring the fix must bring the arrow back")

        radar.stopObserving()
        await graph.stop()
    }

    /// The Thread screenshot's own precondition: an incoming bubble
    /// from Taylor, and two outgoing DMs that resolve WAITING -> SENT ->
    /// DELIVERED and WAITING -> SENT -> NO ACK respectively — through
    /// the REAL compose path (`ThreadViewModel.sendCompose()`), not a
    /// row invented straight in the feed.
    func testThreadShowsTheIncomingTextAndBothDeliveryOutcomes() async {
        let (graph, runner, inbox, _) = makeRunner()
        await graph.start()
        await runner.start()

        await waitUntil { graph.core.crew.count >= 4 }
        // `openThread` hands back a FRESH `ThreadViewModel` bound to the
        // same provider — it starts with `messages == []` until
        // `observe()`/`refresh()` reads the provider itself
        // (`ThreadViewModel.swift`'s own `refresh()`), exactly like
        // `ThreadContainerView`'s `.onAppear { model.observe() }`.
        let thread = inbox.openThread(.member(DemoCrew.taylor))
        thread.observe()
        await waitUntil { thread.refresh(); return thread.messages.count >= 3 }

        XCTAssertTrue(thread.messages.contains { $0.direction == .direct },
                      "Taylor's incoming text must land in this thread")

        let outgoing = thread.messages.filter { $0.direction == .out }
        XCTAssertEqual(outgoing.count, 2, "both scripted DMs must appear")
        await waitUntil {
            thread.refresh()
            return outgoing.map(\.id).allSatisfy { id in
                guard let message = thread.messages.first(where: { $0.id == id }) else { return false }
                let state = thread.renderedDeliveryState(for: message)
                return state == .delivered || state == .noAck
            }
        }
        let states = outgoing.compactMap { original -> DeliveryState? in
            guard let message = thread.messages.first(where: { $0.id == original.id }) else { return nil }
            return thread.renderedDeliveryState(for: message)
        }
        XCTAssertTrue(states.contains(.delivered), "one DM must resolve DELIVERED")
        XCTAssertTrue(states.contains(.noAck), "one DM must resolve NO ACK")

        thread.stopObserving()

        await graph.stop()
    }

    /// The one hard architectural guarantee this whole feature rests
    /// on: `.live()` never references a demo type at all, so a real
    /// device build cannot construct a `DemoMeshtasticClient` no matter
    /// what launch arguments it is handed.
    func testLiveDependenciesNeverConstructTheDemoClient() {
        let live = AppDependencies.live()
        XCTAssertFalse(live.client is DemoMeshtasticClient,
                        "live mode must never construct the demo client")
        XCTAssertFalse(live.location is DemoLocationProvider)
        XCTAssertFalse(live.heading is DemoHeadingProvider)
    }

    func testDemoBundleConstructsTheDemoClient() {
        let bundle = AppDependencies.demoBundle()
        XCTAssertTrue(bundle.dependencies.client is DemoMeshtasticClient)
    }
}
