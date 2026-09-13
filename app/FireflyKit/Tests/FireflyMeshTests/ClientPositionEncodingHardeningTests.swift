//
//  ClientPositionEncodingHardeningTests.swift — hardening QA pass.
//
//  `MeshtasticClient.sendPosition` built its `Position` out of four
//  TRAPPING conversions (`Int32(_:)`/`UInt32(_:)` from `Double`), on
//  values that arrive from CoreLocation via `MeshPositionSink` — i.e.
//  from outside this module. `Int32(Double.nan)` and
//  `UInt32(1e12)` are runtime CRASHES in Swift, not errors, so a single
//  odd fix would have taken the whole app down mid-festival.
//
//  Two of these inputs are not hypothetical at a festival with no cell
//  service:
//
//  - `CLLocationCoordinate2DInvalid` is `(-180, -180)`, which is a
//    perfectly representable `Int32` after the 1e7 scaling — so the old
//    code did not crash on it, it QUIETLY TRANSMITTED a latitude of
//    -180°. That is the fabrication half of this fix.
//  - `location.timestamp` comes from a phone whose clock has had no
//    NTP for three days. A date past 2106 overflows the wire's `uint32`
//    epoch seconds, and `UInt32(max(0, hugeDouble))` traps.
//
//  The fix refuses rather than clamps, for the reason the repo's own
//  honesty rule states: a clamped NaN latitude is the Gulf of Guinea
//  reported as this phone's position.
//
//  `@testable` for `encodePosition`, which is deliberately `static` and
//  pure so these cases need no actor, no transport and no radio.
//
import Foundation
import MeshtasticProto
import XCTest
@testable import FireflyMesh

final class ClientPositionEncodingHardeningTests: XCTestCase {

    private func fix(latitude: Double = 47.708135, longitude: Double = -122.2820993,
                     altitude: Double? = 42,
                     time: Date = Date(timeIntervalSince1970: 1_780_000_000),
                     speed: Double? = nil, track: Double? = nil) -> ExternalPositionFix {
        ExternalPositionFix(latitude: latitude, longitude: longitude, altitudeMeters: altitude,
                            time: time, groundSpeedMetersPerSecond: speed, groundTrackDegrees: track)
    }

    // MARK: - Inputs that used to trap

    /// Each of these crashed the process (not threw) before the guard.
    /// The assertion is both that the call RETURNS and that it returns
    /// `nil` — a fix that cannot be honestly encoded is refused.
    func testNonFiniteAndOutOfRangeCoordinatesAreRefusedNotTrapped() {
        let refused: [(String, ExternalPositionFix)] = [
            ("NaN latitude", fix(latitude: .nan)),
            ("NaN longitude", fix(longitude: .nan)),
            ("+inf latitude", fix(latitude: .infinity)),
            ("-inf longitude", fix(longitude: -.infinity)),
            ("latitude past the pole", fix(latitude: 91)),
            ("longitude past the antimeridian", fix(longitude: 180.5)),
            // The one that did NOT crash, and was worse for it.
            ("CLLocationCoordinate2DInvalid", fix(latitude: -180, longitude: -180)),
        ]
        for (name, bad) in refused {
            XCTAssertNil(MeshtasticClient.encodePosition(bad),
                         "\(name) must be refused, never clamped into a plausible-looking coordinate")
        }
    }

    /// A phone that has been off the network for three days is the
    /// premise of this app, so its clock is exactly the input that
    /// should not be trusted into a trapping conversion.
    func testTimestampsOutsideTheWireEpochAreRefusedNotTrapped() {
        XCTAssertNil(MeshtasticClient.encodePosition(fix(time: .distantFuture)),
                     "a uint32 epoch second runs out in 2106")
        XCTAssertNil(MeshtasticClient.encodePosition(fix(time: .distantPast)),
                     "a pre-1970 fix is a broken clock, not a position from the past")
        XCTAssertNil(MeshtasticClient.encodePosition(fix(time: Date(timeIntervalSince1970: .nan))),
                     "a NaN timestamp traps `UInt32(_:)`")
    }

    /// Altitude is optional on the wire AND optional in truth, so an
    /// unrepresentable one is DROPPED rather than taking a perfectly
    /// good lat/lon down with it.
    func testUnrepresentableAltitudeIsDroppedButTheFixStillGoesOut() throws {
        for bad in [Double.nan, .infinity, 1e30, -1e30] {
            let position = try XCTUnwrap(MeshtasticClient.encodePosition(fix(altitude: bad)),
                                         "altitude \(bad) must not veto the whole fix")
            XCTAssertFalse(position.hasAltitude,
                           "an altitude that cannot be represented is absent, not zero metres")
            XCTAssertEqual(position.latitudeI, 477_081_350, "the lat/lon is still the real one")
        }
    }

    /// `NaN > 0` is false, so the existing "only when it means
    /// something" guards already reject a NaN speed/track — pinned here
    /// because that is a subtle reason to rely on, not an obvious one.
    func testNonFiniteSpeedAndTrackAreOmitted() throws {
        let position = try XCTUnwrap(MeshtasticClient.encodePosition(fix(speed: .nan, track: .nan)))
        XCTAssertFalse(position.hasGroundSpeed)
        XCTAssertFalse(position.hasGroundTrack)
        let huge = try XCTUnwrap(MeshtasticClient.encodePosition(fix(speed: 1e30, track: .infinity)))
        XCTAssertFalse(huge.hasGroundSpeed, "a speed past uint32 traps the conversion")
        XCTAssertFalse(huge.hasGroundTrack)
    }

    // MARK: - A good fix is unchanged

    /// The guards must not have narrowed what a REAL fix encodes to —
    /// the boundary values a real device genuinely produces all pass.
    func testRealFixesIncludingExactBoundariesStillEncode() throws {
        let good: [(String, ExternalPositionFix)] = [
            ("normal", fix()),
            ("north pole", fix(latitude: 90)),
            ("south pole", fix(latitude: -90)),
            ("antimeridian east", fix(longitude: 180)),
            ("antimeridian west", fix(longitude: -180)),
            ("null island", fix(latitude: 0, longitude: 0)),
            ("epoch", fix(time: Date(timeIntervalSince1970: 0))),
            ("no altitude", fix(altitude: nil)),
        ]
        for (name, f) in good {
            let position = try XCTUnwrap(MeshtasticClient.encodePosition(f), "\(name) is a real fix")
            XCTAssertEqual(position.locationSource, .locExternal, "\(name): an external fix, always")
        }
    }

    // MARK: - The actor path

    /// End-to-end through the real `sendPosition`: a refused fix throws
    /// a named error the caller can handle, and — the honest half —
    /// puts NOTHING on the wire. A `PhoneGPSUplink` push that fails is
    /// a push that did not happen, not a push of a wrong coordinate.
    func testSendPositionThrowsAndWritesNothingForAFixItCannotEncode() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        do {
            _ = try await client.sendPosition(fix(latitude: .nan), to: 1)
            XCTFail("a NaN latitude must not reach the wire")
        } catch let error as MeshtasticClientError {
            XCTAssertEqual(error, .invalidPositionFix)
        }
        XCTAssertTrue(transport.sentMessages.isEmpty,
                      "a refused fix must write no bytes at all")
    }
}
