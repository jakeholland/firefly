//
//  MainTabView.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import SwiftUI

/// Main tab view containing inbox and map tabs
struct MainTabView: View {
    private let container: DependencyContainer
    
    init(container: DependencyContainer = .production()) {
        self.container = container
    }
    
    var body: some View {
        TabView {
            InboxView(container: container)
                .tabItem {
                    Label("Inbox", systemImage: "tray.fill")
                }
            
            MapViewTab(container: container)
                .tabItem {
                    Label("Map", systemImage: "map.fill")
                }
        }
        .preferredColorScheme(.dark)
    }
}

#Preview {
    MainTabView()
}
