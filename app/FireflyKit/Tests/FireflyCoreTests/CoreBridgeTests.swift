//
//  CoreBridgeTests.swift — proof that the puck's C core really is
//  running inside the app, not a Swift lookalike.
//
//  These call firmware/core functions directly through the generated
//  module map. If the symlink farm, the include path, or the C11 build
//  ever breaks, this target stops compiling — which is the point.
//
import FireflyCore
import XCTest

final class CoreBridgeTests: XCTestCase {

    /// One degree of latitude is ~111 km; the puck's own haversine says
    /// so, and so must the phone's, because it is the same function.
    func testGeoDistanceOneDegreeOfLatitude() {
        let a = ff_latlon_t(lat: 40.0, lon: -82.0)
        let b = ff_latlon_t(lat: 41.0, lon: -82.0)
        let metres = ff_geo_distance_m(a, b)
        XCTAssertEqual(Double(metres), 111_195, accuracy: 500)
    }

    /// Due north is 0 degrees, due east is 90.
    func testGeoBearingCardinals() {
        let origin = ff_latlon_t(lat: 40.0, lon: -82.0)
        let north = ff_latlon_t(lat: 40.1, lon: -82.0)
        let east = ff_latlon_t(lat: 40.0, lon: -81.9)
        XCTAssertEqual(Double(ff_geo_bearing_deg(origin, north)), 0.0, accuracy: 0.5)
        XCTAssertEqual(Double(ff_geo_bearing_deg(origin, east)), 90.0, accuracy: 0.5)
    }

    /// Freshness thresholds are the puck's (`FF_CREW_LIVE_MS` 45 s,
    /// `FF_CREW_LOST_MS` 20 min) and are read from C, not retyped here.
    func testCrewFreshnessUsesPuckThresholds() {
        var m = ff_crew_member_t()
        m.has_heard = true
        m.has_pos = true
        m.last_heard_ms = 0

        XCTAssertEqual(ff_crew_freshness(&m, 1_000), FF_FRESH_LIVE)
        XCTAssertEqual(ff_crew_freshness(&m, FF_CREW_LIVE_MS + 1), FF_FRESH_STALE)
        XCTAssertEqual(ff_crew_freshness(&m, FF_CREW_LOST_MS + 1), FF_FRESH_LOST)
    }

    /// A position that was typed in, not measured, is a different KIND
    /// of thing — never an aged measurement. Both Heltec bench boards
    /// carry asserted positions, so the app meets this on day one.
    func testAssertedPositionIsNotAStaleMeasurement() {
        var m = ff_crew_member_t()
        m.has_heard = true
        m.has_pos = true
        m.pos_asserted = true
        m.last_heard_ms = 0
        XCTAssertEqual(ff_crew_freshness(&m, FF_CREW_LOST_MS + 1), FF_FRESH_ASSERTED)
    }

    /// Signal tiers come from the puck's own thresholds.
    func testSignalTierBoundaries() {
        XCTAssertEqual(ff_radar_signal_tier(-70), FF_SIGNAL_STRONG)
        XCTAssertEqual(ff_radar_signal_tier(Int16(FF_SIGNAL_STRONG_MIN_DBM)), FF_SIGNAL_GOOD)
        XCTAssertEqual(ff_radar_signal_tier(-100), FF_SIGNAL_WEAK)
        XCTAssertEqual(ff_radar_signal_tier(-120), FF_SIGNAL_FAINT)
    }

    /// The app and the puck must agree on the Firefly portnum, or
    /// nothing Firefly-specific crosses between them.
    func testFireflyPortnumAndProtocolVersion() {
        XCTAssertEqual(FF_PORTNUM, 269)
        XCTAssertEqual(FF_PROTO_VERSION, 1)
    }
}
