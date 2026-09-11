//
//  HeadingProvider.swift — the real `HeadingProviding` implementation
//  (docs/specs/A01-companion-app.md, "Compass heading").
//
//  `HeadingProviding`, `HeadingReading` and `NoHeadingProvider` are
//  landed elsewhere (`HeadingProviding.swift`, S5) and are NOT this
//  file's. `NoHeadingProvider` is already "the permanent macOS answer"
//  per that file's own doc comment — there is no magnetometer on a Mac,
//  and there never will be, so macOS's `HeadingProvider` below IS
//  `NoHeadingProvider`, not a second implementation of the same "always
//  nil" behaviour. iOS gets the real thing: `CLLocationManager`'s
//  compass, `trueHeading` preferred over `magneticHeading`, never a
//  fabricated arrow.
//
import CoreLocation
import FireflyMesh
import Foundation

#if os(iOS)

/// `HeadingProviding` over `CLLocationManager.startUpdatingHeading` —
/// iOS only, where a magnetometer actually exists.
public final class HeadingProvider: NSObject, HeadingProviding, @unchecked Sendable {
    private let manager: CLLocationManager
    private let hub = EventHub<HeadingReading?>()

    public override init() {
        self.manager = CLLocationManager()
        super.init()
        manager.delegate = self
        if CLLocationManager.headingAvailable() {
            manager.startUpdatingHeading()
        }
        // No magnetometer on this particular device (old hardware, or
        // the Simulator without a synthetic heading configured): NOHDG,
        // honestly, rather than silence that a caller might mistake for
        // "still waiting."
    }

    public func headings() -> AsyncStream<HeadingReading?> {
        hub.subscribe()
    }

    /// Pure, and the thing under test: `trueHeading` when CoreLocation
    /// itself considers it valid (its own convention: `< 0` means
    /// invalid), `magneticHeading` otherwise — exactly the spec's rule
    /// — with `accuracy` carried through unchanged so a negative
    /// accuracy still reads as invalid downstream
    /// (`HeadingReading.isValid`, and `ff_radar_compute`'s
    /// `arrow_valid` on the other side of the bridge).
    public static func reading(trueHeading: Double, magneticHeading: Double, accuracy: Double) -> HeadingReading {
        let degrees = trueHeading >= 0 ? trueHeading : magneticHeading
        return HeadingReading(headingDegrees: degrees, accuracyDegrees: accuracy)
    }
}

extension HeadingProvider: CLLocationManagerDelegate {
    public func locationManager(_ manager: CLLocationManager, didUpdateHeading newHeading: CLHeading) {
        hub.yield(Self.reading(
            trueHeading: newHeading.trueHeading,
            magneticHeading: newHeading.magneticHeading,
            accuracy: newHeading.headingAccuracy))
    }

    public func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        switch manager.authorizationStatus {
        case .authorizedAlways, .authorizedWhenInUse:
            if CLLocationManager.headingAvailable() {
                manager.startUpdatingHeading()
            }
        case .denied, .restricted:
            manager.stopUpdatingHeading()
            hub.yield(nil) // NOHDG, explicitly — never a stuck last-known arrow.
        case .notDetermined:
            break
        @unknown default:
            break
        }
    }
}

#else

/// macOS: no magnetometer, permanently — the puck's own `RADAR_NOHDG`
/// mode exists for exactly this case, and running a tilt-compensated
/// fusion on top of CoreLocation would just be two filters fighting
/// (the spec is explicit that `ff_geo_heading_deg` is not used here).
/// A type alias, not a second class, so a caller writes
/// `HeadingProvider()` on either platform and gets the behaviourally
/// correct thing without a `#if os(iOS)` of their own — and so there is
/// exactly one "always nil" implementation in this package, not two
/// that could quietly drift apart.
public typealias HeadingProvider = NoHeadingProvider

#endif
