//
//  LocalNotificationService.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import UserNotifications
#if canImport(UIKit)
import UIKit
#endif

final class LocalNotificationService: NotificationServiceProtocol {
    private let center = UNUserNotificationCenter.current()

    func requestAuthorization() async throws {
        try await center.requestAuthorization(options: [.alert, .sound, .badge])
    }

    func notifyNewMessage(_ message: MeshMessage, from node: MeshNode) {
        // On iOS/iPadOS, suppress the notification while the app is in the foreground.
        // On macOS, UIApplication isn't available — always deliver.
#if canImport(UIKit)
        guard UIApplication.shared.applicationState != .active else { return }
#endif

        let content = UNMutableNotificationContent()
        content.title = node.displayName
        content.body = message.text
        content.sound = .default
#if canImport(UIKit)
        content.badge = 1
#endif

        let request = UNNotificationRequest(identifier: "message_\(message.id)", content: content, trigger: nil)

        center.add(request) { error in
            if let error = error {
                NSLog("🔔 [Notification] ❌ Failed to deliver notification: \(error.localizedDescription)")
            }
        }
    }

    func clearNotifications() {
        center.removeAllDeliveredNotifications()
        center.removeAllPendingNotificationRequests()
#if canImport(UIKit)
        center.setBadgeCount(0)
#endif
    }
}
