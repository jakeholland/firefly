//
//  BridgeCrewStoreTests.swift — CrewStore (FireflyModel/Bridge/CrewStore.swift)
//  against the SAME numbers firmware/core/tests/test_crew.c and
//  test_geo.c use, table-driven where the core's own suite is.
//
//  `@testable import FireflyModel`: Bridge/* types keep their raw C
//  pointer accessors `internal`, not `public` — "no C pointer or tuple
//  in any public API" (docs/specs/A01-companion-app.md). Testability is
//  how a same-repo test target pokes at that without widening the real
//  API surface.
//
import FireflyCore
@testable import FireflyModel
import XCTest

final class BridgeCrewStoreTests: XCTestCase {

    // MARK: - Freshness, mirroring test_crew.c's own boundary table
    // (FF_CREW_LIVE_MS = 45_000, FF_CREW_LOST_MS = 1_200_000).

    func testFreshnessBoundaries() {
        let cases: [(now: UInt32, expected: FreshnessCategory)] = [
            (44_999, .live),
            (45_000, .stale),
            (1_199_999, .stale),
            (1_200_000, .stale),
            (1_200_001, .lost),
        ]
        for (now, expected) in cases {
            let store = CrewStore(now: { 0 })
            store.onPosition(nodeID: 1, latitude: 0, longitude: 0, rxTimeMs: 0)
            let member = store.member(nodeID: 1, now: now)
            XCTAssertEqual(member?.freshness, expected, "at now=\(now)")
        }
    }

    func testFreshnessNeverWhenNoPositionEverArrived() {
        let store = CrewStore(now: { 0 })
        store.upsert(nodeID: 7)
        XCTAssertEqual(store.member(nodeID: 7, now: 999_999)?.freshness, .never)
    }

    /// issue #33: an asserted (LOC_MANUAL) position is a DIFFERENT KIND
    /// of fact, never an aged measurement — mutually exclusive with
    /// LIVE/STALE/LOST no matter how far `now` runs.
    func testAssertedPositionNeverReadsAsStaleOrLost() {
        let store = CrewStore(now: { 0 })
        store.onPosition(nodeID: 1, latitude: 47.708135, longitude: -122.2820993, rxTimeMs: 0,
                          meta: .init(asserted: true))
        XCTAssertEqual(store.member(nodeID: 1, now: 10_000_000)?.freshness, .asserted)
    }

    // MARK: - Heard presence (2026-09-07 amendment), FF_CREW_HEARD_LIVE_MS
    // = 120_000, FF_CREW_HEARD_LOST_MS = 600_000.

    func testHeardPresenceBoundaries() {
        let cases: [(now: UInt32, expected: HeardPresence)] = [
            (119_999, .heard),
            (120_000, .stale),
            (600_000, .stale),
            (600_001, .lost),
        ]
        for (now, expected) in cases {
            let store = CrewStore(now: { 0 })
            store.onHeard(nodeID: 2, rxTimeMs: 0, direct: false)
            XCTAssertEqual(store.member(nodeID: 2, now: now)?.heardPresence, expected, "at now=\(now)")
        }
    }

    func testHeardPresenceNeverWhenNothingEverArrived() {
        let store = CrewStore(now: { 0 })
        store.upsert(nodeID: 3)
        XCTAssertEqual(store.member(nodeID: 3, now: 0)?.heardPresence, .never)
    }

    /// Presence (ANY packet) and freshness (position only) are
    /// deliberately independent axes: heard recently but no position at
    /// all is HEARD + NEVER, not HEARD + LIVE.
    func testPresenceAndFreshnessAreIndependentAxes() {
        let store = CrewStore(now: { 0 })
        store.onHeard(nodeID: 4, rxTimeMs: 0, direct: true)
        let member = store.member(nodeID: 4, now: 1_000)
        XCTAssertEqual(member?.heardPresence, .heard)
        XCTAssertEqual(member?.freshness, .never)
        XCTAssertNil(member?.position)
    }

    // MARK: - The bench pair (docs/specs/A01-companion-app.md's own
    // fixture, matching the two Heltec V3 boards' real bench
    // coordinates): same longitude, so bearing is due south (180°) and
    // the distance is the short walk between two festival tents, not a
    // fabricated round number.

    func testBenchPairBearingAndDistance() {
        let a = ff_latlon_t(lat: 47.707135, lon: -122.2820993)
        let b = ff_latlon_t(lat: 47.705785, lon: -122.2820993)
        let bearing = ff_geo_bearing_deg(a, b)
        let distance = ff_geo_distance_m(a, b)
        XCTAssertEqual(Double(bearing), 180.0, accuracy: 0.5)
        XCTAssertGreaterThan(distance, 100)
        XCTAssertLessThan(distance, 200)
    }

    /// The same fixture, through the Swift bridge end to end: two crew
    /// members positioned at the bench coordinates, and the distance
    /// formatter (`ff_fmt_distance`, called through the bridge) agrees.
    func testBenchPairThroughCrewStoreAndFormatter() {
        let store = CrewStore(now: { 0 })
        store.onPosition(nodeID: 1, latitude: 47.707135, longitude: -122.2820993, rxTimeMs: 0)
        store.onPosition(nodeID: 2, latitude: 47.705785, longitude: -122.2820993, rxTimeMs: 0)
        let m1 = store.member(nodeID: 1, now: 0)!
        let m2 = store.member(nodeID: 2, now: 0)!
        let distance = ff_geo_distance_m(ff_latlon_t(lat: m1.position!.latitude, lon: m1.position!.longitude),
                                         ff_latlon_t(lat: m2.position!.latitude, lon: m2.position!.longitude))
        let text = CrewStore.formatDistance(meters: distance, imperial: false)
        XCTAssertTrue(text.hasSuffix("m"), "expected a metres string under 1km, got \(text)")
    }

    // MARK: - Close range (FF_CREW_CLOSE_RANGE_M = 30, RSSI leg
    // -60 dBm / 10s).

    func testCloseRangeByDistance() {
        let store = CrewStore(now: { 0 })
        store.upsert(nodeID: 1)
        XCTAssertTrue(store.closeRange(nodeID: 1, distanceM: 29.9, now: 0))
        XCTAssertFalse(store.closeRange(nodeID: 1, distanceM: 30.1, now: 0))
    }

    func testCloseRangeByRSSIIndependentOfDistance() {
        let store = CrewStore(now: { 0 })
        store.onRSSI(nodeID: 1, rssiDbm: -55)
        // Far away by distance, but RSSI-close and recent -> still CLOSE.
        XCTAssertTrue(store.closeRange(nodeID: 1, distanceM: 5000, now: 1_000))
    }

    func testCloseRangeUnknownDistanceDoesNotFabricateCloseness() {
        let store = CrewStore(now: { 0 })
        store.upsert(nodeID: 1)
        XCTAssertFalse(store.closeRange(nodeID: 1, distanceM: nil, now: 0))
    }

    // MARK: - Identity: display name selection, via the real core
    // function (`ff_crew_display_name`), not a Swift reimplementation.

    func testDisplayNameFallsBackToShortNameWhenLongNameUnknown() {
        var raw = ff_crew_member_t()
        raw.node_id = 42
        FixedCString.encode("TAYL", into: &raw.name)
        let member = CrewMember.decode(raw, now: 0)
        XCTAssertEqual(member.shortName, "TAYL")
        XCTAssertEqual(member.longName, "")
        XCTAssertEqual(member.displayName, "TAYL")
    }

    func testDisplayNamePrefersLongNameWhenKnown() {
        var raw = ff_crew_member_t()
        raw.node_id = 42
        FixedCString.encode("TAYL", into: &raw.name)
        FixedCString.encode("Taylor", into: &raw.long_name)
        let member = CrewMember.decode(raw, now: 0)
        XCTAssertEqual(member.displayName, "Taylor")
    }

    // MARK: - Signal / battery sentinels never fabricated.

    func testNoDirectSignalUntilARSSISampleArrives() {
        let store = CrewStore(now: { 0 })
        store.upsert(nodeID: 1)
        XCTAssertNil(store.member(nodeID: 1, now: 0)?.directSignal)
    }

    func testUnknownBatteryStaysNilNotZero() {
        let store = CrewStore(now: { 0 })
        store.upsert(nodeID: 1)
        XCTAssertNil(store.member(nodeID: 1, now: 0)?.batteryPercent)
    }

    // MARK: - Roster policy: no eviction when full (FF_CREW_MAX = 8).

    func testRosterHasNoEvictionWhenFull() {
        let store = CrewStore(now: { 0 })
        for id in 1...8 { XCTAssertTrue(store.upsert(nodeID: UInt32(id))) }
        XCTAssertFalse(store.upsert(nodeID: 9), "a 9th distinct id must be rejected, not evict an existing one")
        XCTAssertEqual(store.members(now: 0).count, 8)
    }

    // MARK: - Selection.

    func testSelectionSelfHealsToFirstPairedMember() {
        let store = CrewStore(now: { 0 })
        store.setPaired(nodeID: 1, paired: true)
        XCTAssertEqual(store.selected(now: 0)?.nodeID, 1)
    }

    func testNoSelectionWhenNobodyIsPaired() {
        let store = CrewStore(now: { 0 })
        store.upsert(nodeID: 1) // unpaired
        XCTAssertNil(store.selected(now: 0))
    }

    // MARK: - RSSI trend.

    func testRSSITrendIsFlatWithNoHistory() {
        let store = CrewStore(now: { 0 })
        store.upsert(nodeID: 1)
        XCTAssertEqual(store.rssiTrend(nodeID: 1, now: 0), .flat)
    }

    // MARK: - Enum-growth guards (PR #261 review, finding 2 — the
    // `DeliveryStateTests.testEveryStateMapsToTheCEnum` pattern).

    func testHeardPresenceEnumeratesAllFourCoreValues() {
        XCTAssertEqual(HeardPresence(ffPresence: FF_CREW_PRESENCE_HEARD), .heard)
        XCTAssertEqual(HeardPresence(ffPresence: FF_CREW_PRESENCE_STALE), .stale)
        XCTAssertEqual(HeardPresence(ffPresence: FF_CREW_PRESENCE_LOST), .lost)
        XCTAssertEqual(HeardPresence(ffPresence: FF_CREW_PRESENCE_NEVER), .never)
        XCTAssertEqual(HeardPresence.allCases.count, 4,
                       "a new ff_crew_presence_t value needs a matching HeardPresence case")
    }

    func testFreshnessCategoryEnumeratesAllFiveCoreValues() {
        XCTAssertEqual(FreshnessCategory(ffFreshness: FF_FRESH_LIVE), .live)
        XCTAssertEqual(FreshnessCategory(ffFreshness: FF_FRESH_STALE), .stale)
        XCTAssertEqual(FreshnessCategory(ffFreshness: FF_FRESH_LOST), .lost)
        XCTAssertEqual(FreshnessCategory(ffFreshness: FF_FRESH_NEVER), .never)
        XCTAssertEqual(FreshnessCategory(ffFreshness: FF_FRESH_ASSERTED), .asserted)
        XCTAssertEqual(FreshnessCategory.allCases.count, 5,
                       "a new ff_freshness_t value needs a matching FreshnessCategory case")
    }
}
