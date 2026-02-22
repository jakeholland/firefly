//
//  MockNotificationService.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation

/// Mock notification service for testing
final class MockNotificationService: NotificationServiceProtocol {
    var didRequestAuthorization = false
    var notifiedMessages: [(MeshMessage, MeshNode)] = []
    var didClearNotifications = false
    
    func requestAuthorization() async throws {
        didRequestAuthorization = true
    }
    
    func notifyNewMessage(_ message: MeshMessage, from node: MeshNode) {
        notifiedMessages.append((message, node))
    }
    
    func clearNotifications() {
        didClearNotifications = true
    }
}
