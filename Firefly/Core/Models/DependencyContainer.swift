//
//  DependencyContainer.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation

/// Dependency injection container
/// Manages service lifecycle and dependency graph
final class DependencyContainer {
    // MARK: - Services
    
    let bluetoothService: BluetoothServiceProtocol
    let meshtasticClient: MeshtasticClientProtocol
    let messagingService: MessagingServiceProtocol
    let mapService: MapServiceProtocol
    let notificationService: NotificationServiceProtocol
    let persistenceService: PersistenceService?
    
    // MARK: - Initialization
    
    init(
        bluetoothService: BluetoothServiceProtocol? = nil,
        meshtasticClient: MeshtasticClientProtocol? = nil,
        messagingService: MessagingServiceProtocol? = nil,
        mapService: MapServiceProtocol? = nil,
        notificationService: NotificationServiceProtocol? = nil,
        persistenceService: PersistenceService? = nil,
        enablePersistence: Bool = true
    ) {
        // Initialize services with dependency injection
        // Use provided mocks for testing, or create real implementations
        
        let bluetooth = bluetoothService ?? CoreBluetoothService()
        self.bluetoothService = bluetooth
        
        let client = meshtasticClient ?? MeshtasticClient(bluetoothService: bluetooth)
        self.meshtasticClient = client
        
        let notification = notificationService ?? LocalNotificationService()
        self.notificationService = notification
        
        // Set up persistence if enabled
        if enablePersistence && persistenceService == nil {
            self.persistenceService = PersistenceService.shared
        } else {
            self.persistenceService = persistenceService
        }
        
        self.messagingService = messagingService ?? MessagingService(
            client: client,
            notificationService: notification,
            persistenceService: self.persistenceService
        )
        
        self.mapService = mapService ?? MapService(client: client)
    }
    
    /// Convenience initializer for production use
    static func production() -> DependencyContainer {
        DependencyContainer(enablePersistence: true)
    }
    
    /// Convenience initializer for testing with mocks
    static func mock(
        bluetoothService: BluetoothServiceProtocol,
        meshtasticClient: MeshtasticClientProtocol,
        messagingService: MessagingServiceProtocol,
        mapService: MapServiceProtocol,
        notificationService: NotificationServiceProtocol
    ) -> DependencyContainer {
        DependencyContainer(
            bluetoothService: bluetoothService,
            meshtasticClient: meshtasticClient,
            messagingService: messagingService,
            mapService: mapService,
            notificationService: notificationService,
            enablePersistence: false // Disable persistence in tests
        )
    }
}
