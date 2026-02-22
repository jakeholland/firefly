//
//  NodeLocation.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import CoreLocation

/// Represents the geographic location of a mesh node
struct NodeLocation: Equatable, Codable, Hashable {
    let latitude: Double
    let longitude: Double
    let altitude: Int32?
    let timestamp: Date
    
    init(latitude: Double, longitude: Double, altitude: Int32? = nil, timestamp: Date = Date()) {
        self.latitude = latitude
        self.longitude = longitude
        self.altitude = altitude
        self.timestamp = timestamp
    }
    
    var coordinate: CLLocationCoordinate2D {
        CLLocationCoordinate2D(latitude: latitude, longitude: longitude)
    }
    
    var clLocation: CLLocation {
        CLLocation(
            coordinate: coordinate,
            altitude: Double(altitude ?? 0),
            horizontalAccuracy: kCLLocationAccuracyBest,
            verticalAccuracy: kCLLocationAccuracyBest,
            timestamp: timestamp
        )
    }
}
