//
//  FireflyApp.swift
//  Firefly
//
//  Created by Jake Holland on 2/19/26.
//

import SwiftUI

@main
struct FireflyApp: App {
    // Production dependency container
    private let container = DependencyContainer.production()
    
    init() {
        // Request notification authorization on app launch
        let notificationService = container.notificationService
        Task {
            try? await notificationService.requestAuthorization()
        }
    }
    
    var body: some Scene {
        WindowGroup {
            MainTabView(container: container)
        }
    }
}

