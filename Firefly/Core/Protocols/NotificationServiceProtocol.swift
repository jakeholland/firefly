//
//  NotificationServiceProtocol.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation

/// Protocol for push notification handling
protocol NotificationServiceProtocol {
    /// Request authorization for notifications
    func requestAuthorization() async throws
    
    /// Send a local notification for a new message
    /// Only sends if app is not active
    func notifyNewMessage(_ message: MeshMessage, from node: MeshNode)
    
    /// Clear all pending notifications
    func clearNotifications()
}
