//
//  MapViewModel.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import Combine
import MapKit

@MainActor
final class MapViewModel: ObservableObject {
    @Published var annotations: [NodeMapAnnotation] = []
    @Published var region: MKCoordinateRegion = MKCoordinateRegion(
        center: CLLocationCoordinate2D(latitude: 37.7749, longitude: -122.4194),
        span: MKCoordinateSpan(latitudeDelta: 0.1, longitudeDelta: 0.1)
    )
    @Published var showFriendsOnly: Bool = false
    
    private let mapService: MapServiceProtocol
    private var cancellables = Set<AnyCancellable>()
    
    init(mapService: MapServiceProtocol) {
        self.mapService = mapService
        
        mapService.nodeLocationsPublisher
            .receive(on: RunLoop.main)
            .sink { [weak self] _ in
                self?.refreshAnnotations()
            }
            .store(in: &cancellables)
        
        $showFriendsOnly
            .sink { [weak self] _ in
                self?.refreshAnnotations()
            }
            .store(in: &cancellables)
        
        refreshAnnotations()
        updateRegion()
    }
    
    func refreshAnnotations() {
        annotations = showFriendsOnly ? mapService.friendAnnotations() : mapService.nodeAnnotations()
    }
    
    func updateRegion() {
        guard let suggestedRegion = mapService.suggestedRegion() else { return }
        region = MKCoordinateRegion(
            center: suggestedRegion.center,
            span: MKCoordinateSpan(
                latitudeDelta: suggestedRegion.span.latitudeDelta,
                longitudeDelta: suggestedRegion.span.longitudeDelta
            )
        )
    }
    
    func centerOnAnnotations() {
        updateRegion()
    }
}
