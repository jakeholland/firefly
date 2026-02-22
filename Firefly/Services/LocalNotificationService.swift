//
//  LocalNotificationService.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import UserNotifications
import UIKit

final class LocalNotificationService: NotificationServiceProtocol {
    private let center = UNUserNotificationCenter.current()
    
    func requestAuthorization() async throws {
        try await center.requestAuthorization(options: [.alert, .sound, .badge])
    }
    
    func notifyNewMessage(_ message: MeshMessage, from node: MeshNode) {
        guard UIApplication.shared.applicationState != .active else { return }
        
        let content = UNMutableNotificationContent()
        content.title = node.displayName
        content.body = message.text
        content.sound = .default
        content.badge = 1
        
        let request = UNNotificationRequest(identifier: "message_\(message.id)", content: content, trigger: nil)
        
        center.add(request) { error in
            if let error = error {
                print("Failed to deliver notification: \(error)")
            }
        }
    }
    
    func clearNotifications() {
        center.removeAllDeliveredNotifications()
        center.removeAllPendingNotificationRequests()
        UIApplication.shared.applicationIconBadgeNumber = 0
    }
}
