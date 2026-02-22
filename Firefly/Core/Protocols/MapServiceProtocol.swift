//
//  MapServiceProtocol.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import Combine
import CoreLocation

/// Protocol for map-related functionality
/// Manages node locations and map annotations
protocol MapServiceProtocol {
    /// Publisher for node location updates
    var nodeLocationsPublisher: AnyPublisher<[NodeMapAnnotation], Never> { get }
    
    /// Get all nodes with location data
    func nodeAnnotations() -> [NodeMapAnnotation]
    
    /// Get only friend locations
    func friendAnnotations() -> [NodeMapAnnotation]
    
    /// Get suggested map region to show all nodes
    func suggestedRegion() -> MapRegion?
}

/// Map annotation for a mesh node
struct NodeMapAnnotation: Identifiable, Equatable {
    let id: UInt32
    let node: MeshNode
    let location: NodeLocation
    
    var coordinate: CLLocationCoordinate2D {
        location.coordinate
    }
}

/// Represents a map region
struct MapRegion: Equatable {
    let center: CLLocationCoordinate2D
    let span: MapSpan
}

struct MapSpan: Equatable {
    let latitudeDelta: Double
    let longitudeDelta: Double
}

extension CLLocationCoordinate2D: @retroactive Equatable {
    public static func == (lhs: CLLocationCoordinate2D, rhs: CLLocationCoordinate2D) -> Bool {
        lhs.latitude == rhs.latitude && lhs.longitude == rhs.longitude
    }
}
