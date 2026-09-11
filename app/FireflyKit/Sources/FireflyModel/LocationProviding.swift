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
import FireflyCore
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

/// Staleness of OUR OWN fix (PR #271 review, SHOULD-FIX 3) — reused by
/// both `FlareTakeoverViewModel.show` and `AppGraph.formatRallyText`,
/// the two places a phone GPS reading turns into a confident-looking
/// bearing.
///
/// Deliberately reuses `ff_crew.h`'s `FF_CREW_LIVE_MS` — the puck's own
/// POSITION-freshness threshold (45 s; see that header's own doc
/// comment: "how old is this member's last known coordinate," a
/// separate axis from radio-heard presence) — rather than inventing a
/// second number. `ff_crew_freshness` itself only ever classifies a
/// STORED CREW MEMBER's position, never the phone's own fix, so this
/// mirrors its exact `age < FF_CREW_LIVE_MS` → LIVE boundary (strict
/// less-than; `ff_crew.c`'s own `ff_crew_freshness`) by hand rather than
/// calling it.
extension LocationFix {
    /// Honestly clamped to zero — a `now` that claims to precede `time`
    /// is clock skew, not a fix from the future, and must never read as
    /// "extra fresh."
    func age(now: Date) -> TimeInterval { max(0, now.timeIntervalSince(time)) }

    /// True once this fix is no longer LIVE by the puck's own
    /// POSITION-freshness cutoff — i.e. it has aged into STALE or LOST,
    /// puck-vocabulary-wise. Not honest grounds for a confident bearing
    /// any more.
    func isStale(now: Date) -> Bool { age(now: now) * 1000 >= Double(FF_CREW_LIVE_MS) }

    /// "N min old" phrasing for the staleness message. Floored at 1 —
    /// `isStale` only ever returns true at 45s or later, and "0 min old"
    /// would read as fresh, contradicting the very message it's part of.
    func ageMinutesText(now: Date) -> Int { max(1, Int((age(now: now) / 60).rounded())) }
}
