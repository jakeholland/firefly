//
//  BridgeRadarTests.swift — proof that `ff_radar_compute`'s output
//  survives the round trip into Swift values (docs/specs/A01-companion-app.md,
//  slice B's "Must add"), covering every `RadarMode` the app can reach.
//
import FireflyCore
@testable import FireflyModel
import XCTest

final class BridgeRadarTests: XCTestCase {

    func testNoSelWhenNobodyIsPaired() {
        let crew = CrewStore(now: { 0 })
        crew.upsert(nodeID: 1) // unpaired
        let radar = RadarBridge()
        let view = radar.compute(crew: crew, headingDeg: 0, myPosition: (0, 0), imperial: false, now: 0)
        XCTAssertEqual(view.mode, .noSel)
        XCTAssertFalse(view.arrowValid)
    }

    func testNoFixWhenMyPositionIsUnknown() {
        let crew = CrewStore(now: { 0 })
        crew.setPaired(nodeID: 1, paired: true)
        let radar = RadarBridge()
        let view = radar.compute(crew: crew, headingDeg: 0, myPosition: nil, imperial: false, now: 0)
        XCTAssertEqual(view.mode, .noFix)
        XCTAssertFalse(view.arrowValid)
    }

    /// 2026-09-05 amendment: my position AND the member's position are
    /// both known, but MY heading isn't — a real bearing exists, just
    /// no screen-relative arrow. This is the permanent state of the
    /// macOS build (no magnetometer).
    func testNoHdgWhenHeadingUnknownButGeometryIsKnown() {
        let crew = CrewStore(now: { 0 })
        crew.setPaired(nodeID: 1, paired: true)
        crew.onPosition(nodeID: 1, latitude: 47.705785, longitude: -122.2820993, rxTimeMs: 0)
        let radar = RadarBridge()
        let view = radar.compute(crew: crew, headingDeg: nil,
                                  myPosition: (47.707135, -122.2820993), imperial: false, now: 0)
        XCTAssertEqual(view.mode, .noHdg)
        XCTAssertFalse(view.arrowValid)
        XCTAssertTrue(view.bearingValid, "a bearing needs no heading at all, only two positions")
        XCTAssertEqual(Double(view.bearingDeg), 180.0, accuracy: 0.5)
    }

    func testLiveWhenEverythingIsFreshAndKnown() {
        let crew = CrewStore(now: { 0 })
        crew.setPaired(nodeID: 1, paired: true)
        crew.onPosition(nodeID: 1, latitude: 47.705785, longitude: -122.2820993, rxTimeMs: 0)
        let radar = RadarBridge()
        let view = radar.compute(crew: crew, headingDeg: 0,
                                  myPosition: (47.707135, -122.2820993), imperial: false, now: 1_000)
        XCTAssertEqual(view.mode, .live)
        XCTAssertTrue(view.arrowValid)
        XCTAssertFalse(view.distanceText.isEmpty)
        XCTAssertFalse(view.ageText.isEmpty)
    }

    /// issue #33: an asserted position resolves to PLACE, and — unlike
    /// every other freshness-derived mode — carries no age string, even
    /// though a real fix exists (RADAR_PLACE's own documented departure).
    func testPlaceModeForAnAssertedPosition() {
        let crew = CrewStore(now: { 0 })
        crew.setPaired(nodeID: 1, paired: true)
        crew.onPosition(nodeID: 1, latitude: 47.705785, longitude: -122.2820993, rxTimeMs: 0,
                         meta: .init(asserted: true))
        let radar = RadarBridge()
        let view = radar.compute(crew: crew, headingDeg: 0,
                                  myPosition: (47.707135, -122.2820993), imperial: false, now: 10_000_000)
        XCTAssertEqual(view.mode, .place)
        XCTAssertTrue(view.place)
        XCTAssertEqual(view.ageText, "", "an asserted fix's receive time is not a placement time")
    }

    /// S29: a paired member with no position at all, but heard, reaches
    /// SIGNAL instead of the old dead-end NOFIX/LOST — and never
    /// fabricates an arrow (my own position is unknown here).
    func testSignalModeWhenHeardButNoPositionAndNoFix() {
        let crew = CrewStore(now: { 0 })
        crew.setPaired(nodeID: 1, paired: true)
        crew.onHeard(nodeID: 1, rxTimeMs: 0, direct: true)
        crew.onRSSI(nodeID: 1, rssiDbm: -70)
        let radar = RadarBridge()
        let view = radar.compute(crew: crew, headingDeg: 0, myPosition: nil, imperial: false, now: 1_000)
        XCTAssertEqual(view.mode, .signal)
        XCTAssertFalse(view.arrowValid, "my own position is unknown; nothing to smooth an arrow toward")
        XCTAssertEqual(view.signalTier, .strong)
        XCTAssertTrue(view.signalHeard)
        XCTAssertFalse(view.signalViaRelay)
    }

    /// Every ring dot honestly reports precision, not just the
    /// selection (issue #74): a degraded-precision fix never renders as
    /// an ordinary crisp dot.
    func testDegradedPrecisionFlagsDistanceAsImprecise() {
        let crew = CrewStore(now: { 0 })
        crew.setPaired(nodeID: 1, paired: true)
        crew.onPosition(nodeID: 1, latitude: 47.705785, longitude: -122.2820993, rxTimeMs: 0,
                         meta: .init(precisionBits: 13)) // well under FF_CREW_POS_PRECISION_MIN_BITS (21)
        let radar = RadarBridge()
        let view = radar.compute(crew: crew, headingDeg: 0,
                                  myPosition: (47.707135, -122.2820993), imperial: false, now: 0)
        XCTAssertTrue(view.distanceImprecise, "a degraded fix must never render a metre-level point distance")
    }

    func testCrewRingDotsMirrorEveryPairedMemberWithAPosition() {
        let crew = CrewStore(now: { 0 })
        crew.setPaired(nodeID: 1, paired: true)
        crew.setPaired(nodeID: 2, paired: true)
        crew.onPosition(nodeID: 1, latitude: 47.705785, longitude: -122.2820993, rxTimeMs: 0)
        crew.onPosition(nodeID: 2, latitude: 47.706, longitude: -122.28, rxTimeMs: 0)
        let radar = RadarBridge()
        let view = radar.compute(crew: crew, headingDeg: 0,
                                  myPosition: (47.707135, -122.2820993), imperial: false, now: 0)
        XCTAssertEqual(view.dots.count, 2)
    }

    // MARK: - Enum-growth guard (PR #261 review, finding 2 — the
    // `DeliveryStateTests.testEveryStateMapsToTheCEnum` pattern).

    func testRadarModeEnumeratesAllNineCoreValues() {
        XCTAssertEqual(RadarMode(ffMode: RADAR_LIVE), .live)
        XCTAssertEqual(RadarMode(ffMode: RADAR_STALE), .stale)
        XCTAssertEqual(RadarMode(ffMode: RADAR_LOST), .lost)
        XCTAssertEqual(RadarMode(ffMode: RADAR_PLACE), .place)
        XCTAssertEqual(RadarMode(ffMode: RADAR_CLOSE), .close)
        XCTAssertEqual(RadarMode(ffMode: RADAR_NOFIX), .noFix)
        XCTAssertEqual(RadarMode(ffMode: RADAR_NOHDG), .noHdg)
        XCTAssertEqual(RadarMode(ffMode: RADAR_SIGNAL), .signal)
        XCTAssertEqual(RadarMode(ffMode: RADAR_NOSEL), .noSel)
        XCTAssertEqual(RadarMode.allCases.count, 9, "a new radar_mode_t value needs a matching RadarMode case")
    }
}
