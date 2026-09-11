//
//  GeoBridge.swift — the Swift-safe wrapper over `firmware/core/ff_geo`'s
//  distance/bearing/compass-point functions (docs/specs/S01-core-geo.md).
//
//  Map tab slice: the selected-crew card's "distance/bearing" line, and
//  every crew-pin distance this feature computes, MUST go through this
//  bridge — `ff_geo_distance_m`/`ff_geo_bearing_deg` are the ONE place
//  great-circle distance/bearing math lives in this codebase (the puck
//  and the phone agree because they run the same object code, same
//  rationale as `Bridge/RadarBridge.swift`/`Bridge/CrewStore.swift`).
//  Never reimplemented in Swift.
//
//  Threading: stateless — every function is a pure value-in/value-out
//  call, no heap allocation, safe from any isolation domain.
//
import FireflyCore
import Foundation

/// A bare WGS84 coordinate — the smallest shape this bridge needs.
/// Deliberately not `CrewMember.Position`/`LocationFix` (this file
/// doesn't import enough context to depend on either, and both already
/// carry a `latitude`/`longitude` pair a caller can pass here directly).
public struct GeoCoordinate: Sendable, Equatable {
    public let latitude: Double
    public let longitude: Double

    public init(latitude: Double, longitude: Double) {
        self.latitude = latitude
        self.longitude = longitude
    }

    var ffValue: ff_latlon_t { ff_latlon_t(lat: latitude, lon: longitude) }
}

public enum GeoBridge {
    /// `ff_geo_distance_m` — great-circle distance, meters, always >= 0.
    public static func distanceMeters(from: GeoCoordinate, to: GeoCoordinate) -> Double {
        Double(ff_geo_distance_m(from.ffValue, to.ffValue))
    }

    /// `ff_geo_bearing_deg` — initial true bearing `from` -> `to`, degrees
    /// [0, 360).
    public static func bearingDegrees(from: GeoCoordinate, to: GeoCoordinate) -> Double {
        Double(ff_geo_bearing_deg(from.ffValue, to.ffValue))
    }

    /// `ff_geo_arrow_deg` — screen rotation for an arrow pointing at
    /// `bearingDegrees` given the device's own `headingDegrees`.
    public static func arrowDegrees(bearingDegrees: Double, headingDegrees: Double) -> Double {
        Double(ff_geo_arrow_deg(Float(bearingDegrees), Float(headingDegrees)))
    }

    /// `ff_geo_compass_point` — the 16-point compass name for an
    /// absolute true bearing.
    public static func compassPoint(bearingDegrees: Double) -> String {
        var buf: [CChar] = [0, 0, 0, 0]
        ff_geo_compass_point(Float(bearingDegrees), &buf)
        return String(cString: buf)
    }
}
