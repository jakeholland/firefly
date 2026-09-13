//
//  FindLifecycleTests.swift — `FindLifecycle.apply`/`.stopAll`
//  (`FindSegment.swift`, owner decision 2026-09-13: "combine the Radar
//  and Map tabs into ONE tab named Find"). Run via `xcodebuild test
//  -only-testing:FireflyAppTests` (see project.yml) — same reason
//  `MoreScreenNavigationTests`' own header comment gives: this is
//  app-target Swift, not part of the FireflyKit SwiftPM package `swift
//  test` covers.
//
//  "Only the visible segment's view model observes/pumps" (owner
//  decision) is the property under test here — with a plain spy
//  instead of a real `RadarViewModel`/`MapViewModel`, since this file
//  (like `FindSegment.swift` itself) stays SwiftUI/FireflyModel-free.
//
import XCTest

/// Records every `observe()`/`stopObserving()` call, in order, so a
/// test can assert not just the FINAL state but that a call actually
/// happened (an already-stopped spy staying stopped would otherwise
/// pass a test that never exercised the stop path at all).
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
    /// segments must stop whichever pump was running and start the
    /// other one, not just "leave the new one on".
    func testSwitchingFromRadarToMapStopsRadarAndStartsMap() {
        let radar = ObservingSpy()
        let map = ObservingSpy()
        FindLifecycle.apply(segment: .radar, radar: radar, map: map)
        FindLifecycle.apply(segment: .map, radar: radar, map: map)
        XCTAssertEqual(radar.callLog, ["observe", "stop"], "Radar must actually stop, not merely end up unread")
        // Map's own "stop the other segment first" half of `.apply` ran
        // on BOTH calls (`.radar` stops Map defensively even though it
        // was never observing yet, same idempotent shape every
        // `stopObserving()` in this app already has) — the second
        // "stop" is the one this test is actually about.
        XCTAssertEqual(map.callLog, ["stop", "observe"], "Map must actually start")
    }

    /// And the reverse direction, since the rule is not written as a
    /// one-way transition.
    func testSwitchingFromMapToRadarStopsMapAndStartsRadar() {
        let radar = ObservingSpy()
        let map = ObservingSpy()
        FindLifecycle.apply(segment: .map, radar: radar, map: map)
        FindLifecycle.apply(segment: .radar, radar: radar, map: map)
        XCTAssertEqual(map.callLog, ["observe", "stop"])
        XCTAssertEqual(radar.callLog, ["stop", "observe"])
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
                       "idempotent re-observe is fine; a stop in between would be the bug")
        XCTAssertTrue(map.isObserving)
    }

    /// `FindScreen`'s own `.onDisappear` — leaving Find entirely (a
    /// different tab selected) must stop BOTH, regardless of which
    /// segment was showing.
    func testStopAllStopsBothRegardlessOfWhichWasObserving() {
        let radar = ObservingSpy()
        let map = ObservingSpy()
        FindLifecycle.apply(segment: .radar, radar: radar, map: map)
        FindLifecycle.stopAll(radar: radar, map: map)
        XCTAssertFalse(radar.isObserving)
        XCTAssertFalse(map.isObserving)
    }
}
