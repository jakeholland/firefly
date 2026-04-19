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
    
    /// Convenience initializer for production use.
    /// On the simulator CoreBluetooth doesn't function, so we return a pre-populated
    /// mock instead of crashing or hanging on a BLE init.
    static func production() -> DependencyContainer {
        #if targetEnvironment(simulator)
        return simulatorContainer()
        #else
        return DependencyContainer(enablePersistence: true)
        #endif
    }

    /// A fully-mocked container pre-populated with fake nodes and messages for
    /// Simulator builds where CoreBluetooth is unavailable.
    ///
    /// We pass explicit mocks for every layer so that `DependencyContainer.init`
    /// never reaches `CoreBluetoothService()` (which spins up `CBCentralManager`
    /// and crashes / hangs on the simulator).
    static func simulatorContainer() -> DependencyContainer {
        let mockBluetooth = MockBluetoothService(initialState: .connected)
        let mockClient   = MockMeshtasticClient()
        let mock         = MockMessagingService(initialConnectionState: .connected)
        mock.myNodeId = 0xDEADBEEF

        // Fake channels
        mock.setChannels([
            MeshChannel(id: 0, name: "LongFast", role: .primary),
            MeshChannel(id: 1, name: "Local", role: .secondary),
        ])

        // Fake nodes
        let nodes: [MeshNode] = [
            MeshNode(id: 0x11111111, shortName: "ALCE", longName: "Alice",   hardwareModel: "T-Beam", macAddress: nil, lastHeard: Date().addingTimeInterval(-120),  location: nil, isFriend: false),
            MeshNode(id: 0x22222222, shortName: "BOBM", longName: "Bob",     hardwareModel: "Heltec", macAddress: nil, lastHeard: Date().addingTimeInterval(-300),  location: nil, isFriend: false),
            MeshNode(id: 0x33333333, shortName: "CHAR", longName: "Charlie", hardwareModel: "T-Beam", macAddress: nil, lastHeard: Date().addingTimeInterval(-3600), location: nil, isFriend: false),
        ]
        mock.setNodes(nodes)

        // Fake channel messages
        let now = Date()
        let channelMessages: [MeshMessage] = [
            MeshMessage(id: 1, from: 0x11111111, to: meshBroadcastAddress, channel: 0, text: "Hey everyone on the mesh!", timestamp: now.addingTimeInterval(-600), isFromMe: false),
            MeshMessage(id: 2, from: 0xDEADBEEF, to: meshBroadcastAddress, channel: 0, text: "Hello from Firefly!", timestamp: now.addingTimeInterval(-540), isFromMe: true),
            MeshMessage(id: 3, from: 0x22222222, to: meshBroadcastAddress, channel: 0, text: "Range test — anyone copy?", timestamp: now.addingTimeInterval(-300), isFromMe: false),
            MeshMessage(id: 4, from: 0x33333333, to: meshBroadcastAddress, channel: 0, text: "Copy, 5 by 5 👋", timestamp: now.addingTimeInterval(-240), isFromMe: false),
            MeshMessage(id: 5, from: 0x11111111, to: meshBroadcastAddress, channel: 1, text: "Local channel test", timestamp: now.addingTimeInterval(-180), isFromMe: false),
        ]

        // Fake DM thread
        let dmMessages: [MeshMessage] = [
            MeshMessage(id: 6, from: 0x11111111, to: 0xDEADBEEF, channel: 0, text: "Hey, got a sec?", timestamp: now.addingTimeInterval(-900), isFromMe: false),
            MeshMessage(id: 7, from: 0xDEADBEEF, to: 0x11111111, channel: 0, text: "Sure, what's up?",  timestamp: now.addingTimeInterval(-870), isFromMe: true),
        ]

        for msg in channelMessages + dmMessages {
            mock.allMessages.append(msg)
        }

        // Pass all three mock layers so the init never instantiates CoreBluetoothService.
        return DependencyContainer(
            bluetoothService: mockBluetooth,
            meshtasticClient: mockClient,
            messagingService: mock,
            mapService: MockMapService(),
            notificationService: MockNotificationService(),
            enablePersistence: false
        )
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
