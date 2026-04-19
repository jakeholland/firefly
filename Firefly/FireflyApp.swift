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
        // Request notification authorization on app launch.
        // Errors are non-fatal (user may decline) — log but don't propagate.
        let notificationService = container.notificationService
        Task {
            do {
                try await notificationService.requestAuthorization()
            } catch {
                NSLog("🔔 [App] ⚠️ Notification authorization failed: \(error.localizedDescription)")
            }
        }
    }
    
    var body: some Scene {
        WindowGroup {
            MainTabView(container: container)
        }
    }
}

