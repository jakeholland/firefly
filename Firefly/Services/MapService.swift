//
//  MapService.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import Combine
import CoreLocation

final class MapService: MapServiceProtocol {
    private let client: MeshtasticClientProtocol
    
    private let nodeLocationsSubject = PassthroughSubject<[NodeMapAnnotation], Never>()
    var nodeLocationsPublisher: AnyPublisher<[NodeMapAnnotation], Never> {
        nodeLocationsSubject.eraseToAnyPublisher()
    }
    
    private var cancellables = Set<AnyCancellable>()
    
    init(client: MeshtasticClientProtocol) {
        self.client = client
        
        client.nodeUpdatesPublisher
            .sink { [weak self] _ in
                self?.publishNodeLocations()
            }
            .store(in: &cancellables)
    }
    
    func nodeAnnotations() -> [NodeMapAnnotation] {
        client.nodes.compactMap { node in
            guard let location = node.location else { return nil }
            return NodeMapAnnotation(id: node.id, node: node, location: location)
        }
    }
    
    func friendAnnotations() -> [NodeMapAnnotation] {
        nodeAnnotations().filter { $0.node.isFriend }
    }
    
    func suggestedRegion() -> MapRegion? {
        let annotations = nodeAnnotations()
        guard !annotations.isEmpty else { return nil }
        
        let coordinates = annotations.map { $0.coordinate }
        let minLat = coordinates.map { $0.latitude }.min() ?? 0
        let maxLat = coordinates.map { $0.latitude }.max() ?? 0
        let minLon = coordinates.map { $0.longitude }.min() ?? 0
        let maxLon = coordinates.map { $0.longitude }.max() ?? 0
        
        let centerLat = (minLat + maxLat) / 2
        let centerLon = (minLon + maxLon) / 2
        let spanLat = max((maxLat - minLat) * 1.5, 0.01)
        let spanLon = max((maxLon - minLon) * 1.5, 0.01)
        
        return MapRegion(
            center: CLLocationCoordinate2D(latitude: centerLat, longitude: centerLon),
            span: MapSpan(latitudeDelta: spanLat, longitudeDelta: spanLon)
        )
    }
    
    private func publishNodeLocations() {
        nodeLocationsSubject.send(nodeAnnotations())
    }
}
