//
//  CrewMapPinTests.swift — Map tab slice: the pin-state table from real
//  `ff_crew` freshness (live/stale/lost/never/asserted/imprecise), per
//  this slice's own test plan.
//
import FireflyCore
@testable import FireflyModel
import XCTest

final class CrewMapPinTests: XCTestCase {
    private let myPosition = GeoCoordinate(latitude: 43.700000, longitude: -121.500000)

    func testLiveFreshnessRendersAsLivePin() {
        let store = CrewStore(now: { 0 })
        store.onPosition(nodeID: 1, latitude: 43.7005, longitude: -121.4995, rxTimeMs: 0)
        let pins = CrewMapPinBuilder.build(from: store.members(now: 1_000), myPosition: myPosition)
        XCTAssertEqual(pins.count, 1)
        XCTAssertEqual(pins[0].treatment, .live)
        XCTAssertFalse(pins[0].ageText.hasPrefix("~"), "a LIVE pin's age must not read as approximate")
    }

    func testStaleFreshnessRendersAsDashedRingWithApproximateAge() {
        let store = CrewStore(now: { 0 })
        store.onPosition(nodeID: 1, latitude: 43.7005, longitude: -121.4995, rxTimeMs: 0)
        // FF_CREW_LIVE_MS == 45_000 — one tick past it is STALE.
        let pins = CrewMapPinBuilder.build(from: store.members(now: 45_001), myPosition: myPosition)
        XCTAssertEqual(pins[0].treatment, .staleRing)
        XCTAssertTrue(pins[0].ageText.hasPrefix("~"), "a STALE pin's age must read as approximate")
    }

    func testLostFreshnessRendersAsDashedRingWithApproximateAge() {
        let store = CrewStore(now: { 0 })
        store.onPosition(nodeID: 1, latitude: 43.7005, longitude: -121.4995, rxTimeMs: 0)
        // FF_CREW_LOST_MS == 1_200_000 — one tick past it is LOST.
        let pins = CrewMapPinBuilder.build(from: store.members(now: 1_200_001), myPosition: myPosition)
        XCTAssertEqual(pins[0].treatment, .lostRing)
        XCTAssertTrue(pins[0].ageText.hasPrefix("~"))
    }

    func testNeverFreshnessIsNotDrawnAtAll() {
        let store = CrewStore(now: { 0 })
        store.upsert(nodeID: 1) // tracked, but no position has ever arrived
        let pins = CrewMapPinBuilder.build(from: store.members(now: 999_999), myPosition: myPosition)
        XCTAssertTrue(pins.isEmpty, "NEVER (no position at all) must not produce a pin — nothing to honestly anchor")
    }

    func testAssertedPositionRendersAsAssertedRegardlessOfAge() {
        let store = CrewStore(now: { 0 })
        store.onPosition(nodeID: 1, latitude: 43.6995, longitude: -121.5000, rxTimeMs: 0,
                          meta: .init(asserted: true))
        let pins = CrewMapPinBuilder.build(from: store.members(now: 100_000_000), myPosition: myPosition)
        XCTAssertEqual(pins[0].treatment, .asserted)
        XCTAssertEqual(pins[0].ageText, "ASSERTED")
    }

    func testDegradedPrecisionRendersAsImpreciseAreaRegardlessOfFreshness() {
        let store = CrewStore(now: { 0 })
        // bits=20 -> below FF_CREW_POS_PRECISION_MIN_BITS (21) — degraded.
        store.onPosition(nodeID: 1, latitude: 43.7005, longitude: -121.4995, rxTimeMs: 0,
                          meta: .init(precisionBits: 20))
        let pins = CrewMapPinBuilder.build(from: store.members(now: 1_000), myPosition: myPosition)
        XCTAssertEqual(pins[0].treatment, .imprecise)
        XCTAssertNotNil(pins[0].precisionGridMeters)
        XCTAssertGreaterThan(pins[0].precisionGridMeters ?? 0, 0)
    }

    func testPreciseBitsAtThresholdStillRendersAsOrdinaryFreshness() {
        let store = CrewStore(now: { 0 })
        // bits=21 -> exactly FF_CREW_POS_PRECISION_MIN_BITS, still "precise".
        store.onPosition(nodeID: 1, latitude: 43.7005, longitude: -121.4995, rxTimeMs: 0,
                          meta: .init(precisionBits: 21))
        let pins = CrewMapPinBuilder.build(from: store.members(now: 1_000), myPosition: myPosition)
        XCTAssertEqual(pins[0].treatment, .live)
        XCTAssertNil(pins[0].precisionGridMeters)
    }

    func testNoOwnPositionYieldsHonestlyNilDistanceAndBearing() {
        let store = CrewStore(now: { 0 })
        store.onPosition(nodeID: 1, latitude: 43.7005, longitude: -121.4995, rxTimeMs: 0)
        let pins = CrewMapPinBuilder.build(from: store.members(now: 1_000), myPosition: nil)
        XCTAssertNil(pins[0].distanceMeters)
        XCTAssertNil(pins[0].bearingDegrees)
    }
}
