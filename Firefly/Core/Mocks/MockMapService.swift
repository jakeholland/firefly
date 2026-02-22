//
//  MockMapService.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import Combine
import CoreLocation

/// Mock map service for testing
final class MockMapService: MapServiceProtocol {
    private let nodeLocationsSubject = PassthroughSubject<[NodeMapAnnotation], Never>()
    var nodeLocationsPublisher: AnyPublisher<[NodeMapAnnotation], Never> {
        nodeLocationsSubject.eraseToAnyPublisher()
    }
    
    var annotations: [NodeMapAnnotation] = []
    
    func nodeAnnotations() -> [NodeMapAnnotation] {
        annotations
    }
    
    func friendAnnotations() -> [NodeMapAnnotation] {
        annotations.filter { $0.node.isFriend }
    }
    
    func suggestedRegion() -> MapRegion? {
        guard !annotations.isEmpty else { return nil }
        
        return MapRegion(
            center: CLLocationCoordinate2D(latitude: 37.7749, longitude: -122.4194),
            span: MapSpan(latitudeDelta: 0.1, longitudeDelta: 0.1)
        )
    }
    
    // Test helpers
    func simulateAnnotations(_ newAnnotations: [NodeMapAnnotation]) {
        annotations = newAnnotations
        nodeLocationsSubject.send(newAnnotations)
    }
}
