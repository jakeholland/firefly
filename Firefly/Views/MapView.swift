//
//  MapView.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import SwiftUI
import MapKit

/// Map view for use in tab view
struct MapViewTab: View {
    @StateObject private var viewModel: MapViewModel

    init(container: DependencyContainer = .production()) {
        _viewModel = StateObject(wrappedValue: MapViewModel(mapService: container.mapService))
    }

    var body: some View {
        NavigationStack {
            ZStack {
                // Use MapKit.Map explicitly to avoid SwiftProtobuf name collision
                MapKit.Map(coordinateRegion: $viewModel.region, annotationItems: viewModel.annotations) { annotation in
                    MapAnnotation(coordinate: annotation.coordinate) {
                        NodeMarkerView(node: annotation.node)
                    }
                }
                .ignoresSafeArea()

                VStack {
                    Spacer()

                    HStack {
                        Toggle(isOn: $viewModel.showFriendsOnly) {
                            Label("Friends Only", systemImage: "person.2.fill")
                                .font(.caption)
                        }
                        .toggleStyle(.button)
                        .tint(.cyan)
                        .padding()
                        .background(Color.black.opacity(0.7))
                        .clipShape(RoundedRectangle(cornerRadius: 12))

                        Spacer()

                        Button {
                            viewModel.centerOnAnnotations()
                        } label: {
                            Image(systemName: "location.fill")
                                .font(.title3)
                                .foregroundStyle(.white)
                                .padding()
                                .background(Color.cyan)
                                .clipShape(Circle())
                        }
                    }
                    .padding()
                }
            }
            .navigationTitle("Map")
            #if os(iOS)
            .navigationBarTitleDisplayMode(.large)
            #endif
            .preferredColorScheme(.dark)
        }
    }
}

struct NodeMarkerView: View {
    let node: MeshNode

    var body: some View {
        VStack(spacing: 4) {
            Image(systemName: node.isFriend ? "star.fill" : "circle.fill")
                .font(.title3)
                .foregroundStyle(node.isFriend ? .yellow : .cyan)
                .padding(8)
                .background(Color.black.opacity(0.7))
                .clipShape(Circle())

            Text(node.displayName)
                .font(.caption2)
                .padding(4)
                .background(Color.black.opacity(0.7))
                .clipShape(RoundedRectangle(cornerRadius: 4))
                .foregroundStyle(.white)
        }
    }
}

#Preview("Map Tab") {
    MapViewTab()
}
