//
//  LocationProviding.swift — the phone-GPS seam (docs/specs/
//  A01-companion-app.md, "Phone GPS -> node", and S5).
//
//  Landed here, un-owned by any single slice, so slice D (Radar depends
//  on it for "no fix" rendering) and slice F (which owns the CoreLocation
//  implementation) do not each invent a different shape.
//
//  Never fabricate: `fix` is `nil` whenever there is no real fix, and
//  authorization denial is a distinct, explicit state — never silently
//  folded into "no fix" the way a lazier seam would.
//
import Foundation

/// One real GPS reading. `LOC_EXTERNAL` on the wire, never
/// `set_fixed_position` — see the spec's "Mechanism" note.
public struct LocationFix: Sendable, Equatable {
    public let latitude: Double
    public let longitude: Double
    public let altitude: Double?
    public let time: Date
    public let horizontalAccuracyMeters: Double?
    public let groundSpeedMetersPerSecond: Double?
    /// Only meaningful when `0 < value <= 360`, per the spec's payload
    /// rule; the provider is responsible for that check, not the caller.
    public let groundTrackDegrees: Double?

    public init(latitude: Double, longitude: Double, altitude: Double?, time: Date,
                horizontalAccuracyMeters: Double?, groundSpeedMetersPerSecond: Double?,
                groundTrackDegrees: Double?) {
        self.latitude = latitude
        self.longitude = longitude
        self.altitude = altitude
        self.time = time
        self.horizontalAccuracyMeters = horizontalAccuracyMeters
        self.groundSpeedMetersPerSecond = groundSpeedMetersPerSecond
        self.groundTrackDegrees = groundTrackDegrees
    }
}

public enum LocationAuthorization: Sendable, Equatable {
    case notDetermined
    case whenInUse
    case always
    case deniedOrRestricted
}

public protocol LocationProviding: AnyObject, Sendable {
    var authorization: LocationAuthorization { get }
    func requestWhenInUseAuthorization() async
    /// Only called once the user has explicitly turned on location
    /// sharing — see the spec's "Authorisation and background modes".
    func requestAlwaysAuthorization() async
    /// A fresh, independent stream for the caller (same multicast rule
    /// as `MeshtasticClientProtocol`, S1). `nil` means no fix right now
    /// — permission denied, signal not yet acquired, or turned off —
    /// and must never be papered over with a last-known position.
    func fixes() -> AsyncStream<LocationFix?>
}

/// The M1 stand-in and the permanent iOS-Simulator/unit-test default.
/// Reports denied and yields nothing but `nil` — the same honesty rule
/// `StubMeshtasticClient` follows: an "unavailable" answer is truthful,
/// a fabricated coordinate is not.
public final class UnavailableLocationProvider: LocationProviding, @unchecked Sendable {
    public let authorization: LocationAuthorization = .deniedOrRestricted
    public init() {}
    public func requestWhenInUseAuthorization() async {}
    public func requestAlwaysAuthorization() async {}
    public func fixes() -> AsyncStream<LocationFix?> {
        AsyncStream { continuation in
            continuation.yield(nil)
            continuation.finish()
        }
    }
}
