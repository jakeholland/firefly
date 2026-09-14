//
//  FindLifecycleTests.swift — `FindLifecycle.apply`/`.stopAll`
//  (`FindSegment.swift`, owner decision 2026-09-13: "combine the Radar
//  and Map tabs into ONE tab named Find"; and 2026-09-13's "FIND keeps
//  running across Find segments"). Run via `xcodebuild test
//  -only-testing:FireflyAppTests` (see project.yml) — same reason
//  `MoreScreenNavigationTests`' own header comment gives: this is
//  app-target Swift, not part of the FireflyKit SwiftPM package `swift
//  test` covers.
//
//  "Only the visible segment's view model observes/pumps" (owner
//  decision) is the property under test here — with a plain spy
//  instead of a real `RadarViewModel`/`MapViewModel`, since this file
//  (like `FindSegment.swift` itself) stays SwiftUI/FireflyModel-free.
//  The spy distinguishes `pauseObserving()` from `stopObserving()` in
//  its own call log (a REAL `RadarViewModel` distinguishes them by
//  whether `stopFind()` runs — that behavioral half is
//  `RadarViewModelTests.testPauseObservingStopsThePumpButLeavesAn
//  ActiveFindSessionRunning`, in FireflyKit, since it needs the real
//  view model). What this file pins is the STRUCTURAL half: which verb
//  `FindLifecycle` reaches for at each call site.
//
import XCTest

/// Records every `observe()`/`stopObserving()`/`pauseObserving()` call,
/// in order, so a test can assert not just the FINAL state but that a
/// call actually happened (an already-stopped spy staying stopped would
/// otherwise pass a test that never exercised the stop path at all).
///
/// `@MainActor`: `FindSegmentObserving` is (`FindSegment.swift`'s own
/// doc comment — its two real conformers are `@MainActor @Observable`
/// classes).
@MainActor
private final class ObservingSpy: FindSegmentObserving {
    private(set) var callLog: [String] = []
    private(set) var isObserving = false

    func observe() {
        callLog.append("observe")
        isObserving = true
    }

    func stopObserving() {
        callLog.append("stop")
        isObserving = false
    }

    /// Overridden (rather than left as the protocol's default, which
    /// would alias this to `stopObserving()`) so a test can tell WHICH
    /// verb `FindLifecycle` reached for — the entire point of this
    /// spy's redesign for the 2026-09-13 "FIND survives a segment
    /// switch" decision.
    func pauseObserving() {
        callLog.append("pause")
        isObserving = false
    }
}

@MainActor
final class FindLifecycleTests: XCTestCase {
    /// The default segment (owner decision: "Radar is the default
    /// segment") observing Radar and leaving Map alone.
    func testRadarSegmentObservesRadarAndStopsMap() {
        let radar = ObservingSpy()
        let map = ObservingSpy()
        FindLifecycle.apply(segment: .radar, radar: radar, map: map)
        XCTAssertTrue(radar.isObserving, "the visible segment's view model must observe")
        XCTAssertFalse(map.isObserving, "the hidden segment's view model must not pump")
    }

    /// Map and Field share the ONE `MapViewModel` (`FindSegment.swift`'s
    /// own doc comment) — either segment observes Map and stops Radar.
    func testMapSegmentObservesMapAndStopsRadar() {
        let radar = ObservingSpy()
        let map = ObservingSpy()
        FindLifecycle.apply(segment: .map, radar: radar, map: map)
        XCTAssertTrue(map.isObserving, "the visible segment's view model must observe")
        XCTAssertFalse(radar.isObserving, "the hidden segment's view model must not pump")
    }

    func testFieldSegmentObservesMapAndStopsRadar() {
        let radar = ObservingSpy()
        let map = ObservingSpy()
        FindLifecycle.apply(segment: .field, radar: radar, map: map)
        XCTAssertTrue(map.isObserving, "Field is the OTHER Map segment — same view model as Map")
        XCTAssertFalse(radar.isObserving)
    }

    /// The actual "start/stop on segment change" requirement: switching
    /// segments must pause whichever pump was running and start the
    /// other one, not just "leave the new one on". PAUSE, not STOP
    /// (owner decision, 2026-09-13): a segment switch must never be the
    /// thing that ends an active FIND session on Radar.
    func testSwitchingFromRadarToMapPausesRadarAndStartsMap() {
        let radar = ObservingSpy()
        let map = ObservingSpy()
        FindLifecycle.apply(segment: .radar, radar: radar, map: map)
        FindLifecycle.apply(segment: .map, radar: radar, map: map)
        XCTAssertEqual(radar.callLog, ["observe", "pause"],
                       "Radar must actually pause, not merely end up unread, and never a full stop")
        // Map's own "pause the other segment first" half of `.apply` ran
        // on BOTH calls (`.radar` pauses Map defensively even though it
        // was never observing yet, same idempotent shape every
        // `stopObserving()` in this app already has) — the second
        // "pause" is the one this test is actually about.
        XCTAssertEqual(map.callLog, ["pause", "observe"], "Map must actually start")
    }

    /// And the reverse direction, since the rule is not written as a
    /// one-way transition.
    func testSwitchingFromMapToRadarPausesMapAndStartsRadar() {
        let radar = ObservingSpy()
        let map = ObservingSpy()
        FindLifecycle.apply(segment: .map, radar: radar, map: map)
        FindLifecycle.apply(segment: .radar, radar: radar, map: map)
        XCTAssertEqual(map.callLog, ["observe", "pause"])
        XCTAssertEqual(radar.callLog, ["pause", "observe"])
    }

    /// Switching between Map and Field must NOT stop/restart Map's own
    /// pump — same as `MapTabView`'s old internal Field/GPS toggle
    /// never did (`FindSegment.swift`'s own doc comment): a crew
    /// position pump has no reason to reset just because the RENDERING
    /// changed from a MapKit view to a schematic one.
    func testSwitchingBetweenMapAndFieldNeverStopsMap() {
        let radar = ObservingSpy()
        let map = ObservingSpy()
        FindLifecycle.apply(segment: .map, radar: radar, map: map)
        FindLifecycle.apply(segment: .field, radar: radar, map: map)
        XCTAssertEqual(map.callLog, ["observe", "observe"],
                       "idempotent re-observe is fine; a stop/pause in between would be the bug")
        XCTAssertTrue(map.isObserving)
    }

    /// `FindScreen`'s own `.onDisappear` — leaving Find entirely (a
    /// different tab selected) must PAUSE both, regardless of which
    /// segment was showing. Owner decision, 2026-09-13 reverses part of
    /// #298 here too: leaving the Find tab must not end an active FIND
    /// session either — "ends on explicit cancel or on backgrounding,"
    /// never merely because another tab is now showing.
    func testStopAllPausesBothRegardlessOfWhichWasObserving() {
        let radar = ObservingSpy()
        let map = ObservingSpy()
        FindLifecycle.apply(segment: .radar, radar: radar, map: map)
        FindLifecycle.stopAll(radar: radar, map: map)
        XCTAssertFalse(radar.isObserving)
        XCTAssertFalse(map.isObserving)
        XCTAssertEqual(radar.callLog, ["observe", "pause"], "leaving Find must pause, never fully stop, Radar")
        XCTAssertEqual(map.callLog, ["pause", "pause"])
    }
}
