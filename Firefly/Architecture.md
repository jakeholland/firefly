# Firefly Architecture Overview

> A production-quality iOS application for Meshtastic mesh networking
> Built with MVVM, Protocol-Oriented Design, and Dependency Injection

## Folder Structure
```
Firefly/
├── Core/
│   ├── Protocols/
│   │   ├── BluetoothServiceProtocol.swift
│   │   ├── MeshtasticClientProtocol.swift
│   │   ├── MessagingServiceProtocol.swift
│   │   ├── MapServiceProtocol.swift
│   │   └── NotificationServiceProtocol.swift
│   └── Models/
│       ├── MeshNode.swift
│       ├── MeshMessage.swift
│       ├── MeshChannel.swift
│       └── NodeLocation.swift
├── Services/
│   ├── Bluetooth/
│   │   ├── CoreBluetoothService.swift
│   │   └── MeshtasticBLEConstants.swift
│   ├── Meshtastic/
│   │   ├── MeshtasticClient.swift
│   │   └── ProtobufEncoder.swift
│   └── Notification/
│       └── LocalNotificationService.swift
├── Domain/
│   ├── Messaging/
│   │   ├── MessagingService.swift
│   │   └── MessageRepository.swift
│   └── Map/
│       └── MapService.swift
├── ViewModels/
│   ├── InboxViewModel.swift
│   ├── ChannelViewModel.swift
│   ├── DirectMessageViewModel.swift
│   └── MapViewModel.swift
├── Views/
│   ├── InboxView.swift
│   ├── ChannelListView.swift
│   ├── ConversationView.swift
│   ├── DirectMessageView.swift
│   ├── MapView.swift
│   └── Components/
│       ├── MessageRowView.swift
│       ├── ChannelRowView.swift
│       └── NodeAnnotationView.swift
├── Mocks/
│   ├── MockBluetoothService.swift
│   ├── MockMeshtasticClient.swift
│   ├── MockMessagingService.swift
│   └── MockMapService.swift
└── Tests/
    ├── MessagingServiceTests.swift
    ├── MeshtasticClientTests.swift
    ├── InboxViewModelTests.swift
    ├── ChannelViewModelTests.swift
    └── MapViewModelTests.swift
```

## Architecture Layers

### 1. Core/Protocols
Protocol definitions for all major services. Enables dependency injection and testability.

**BluetoothServiceProtocol**
- Manages CoreBluetooth connection lifecycle
- Discovers Meshtastic devices
- Sends/receives raw data packets
- Publishes connection state changes

**MeshtasticClientProtocol**
- High-level Meshtastic API
- Encodes/decodes protobuf messages
- Manages node information
- Handles channels and routing
- Provides Combine publishers for incoming data

**MessagingServiceProtocol**
- Business logic for messaging features
- Send/receive messages
- Channel management
- Direct messaging
- Message persistence

**MapServiceProtocol**
- Manages node location data
- Provides map annotations
- Updates friend positions in real-time

**NotificationServiceProtocol**
- Local push notifications
- Triggered when app is backgrounded

### 2. Services Layer

**CoreBluetoothService**
- Implements BluetoothServiceProtocol
- Uses CoreBluetooth framework
- Manages BLE peripheral connection
- Reads/writes to Meshtastic service characteristics

**MeshtasticClient**
- Implements MeshtasticClientProtocol
- Depends on BluetoothServiceProtocol (injected)
- Handles ToRadio/FromRadio packet encoding/decoding
- Manages node database
- Publishes typed events (new message, node update, etc.)

**LocalNotificationService**
- Implements NotificationServiceProtocol
- Uses UserNotifications framework
- Triggers local notifications for new messages

### 3. Domain Layer

**MessagingService**
- Implements MessagingServiceProtocol
- Depends on MeshtasticClientProtocol (injected)
- Orchestrates messaging workflows
- Manages message repository (in-memory store)
- Provides filtered/sorted message streams

**MapService**
- Implements MapServiceProtocol
- Depends on MeshtasticClientProtocol (injected)
- Transforms node data into map annotations
- Filters friends vs. all nodes

### 4. ViewModels

**InboxViewModel**
- Depends on MessagingServiceProtocol
- Provides list of channels and recent DMs
- Connection status
- @Published properties for SwiftUI binding

**ChannelViewModel**
- Manages single channel conversation
- Send message action
- List of messages for channel

**DirectMessageViewModel**
- Manages 1:1 direct message conversation
- Send direct message action
- List of messages with specific node

**MapViewModel**
- Depends on MapServiceProtocol
- Provides map region and annotations
- Real-time location updates

### 5. Views

**InboxView**
- Main entry point
- Shows connection status
- Lists channels and direct messages
- Navigation to detail views

**ChannelListView**
- Displays available channels
- Create/join channel actions

**ConversationView**
- Generic message list view
- Used for both channels and DMs
- Message input field

**MapView**
- SwiftUI MapKit integration
- Node annotations
- Friend locations

### 6. Mocks & Tests

**Mocks**
- Implement all protocols
- Provide controllable test data
- Use Combine PassthroughSubjects for event simulation

**Tests**
- Unit tests for all services
- ViewModel tests with mocked dependencies
- No UI tests (focus on logic)

## Data Flow

1. **Bluetooth Layer**
   - CoreBluetoothService receives raw BLE packets
   - Publishes via `receivedDataPublisher`

2. **Meshtastic Layer**
   - MeshtasticClient subscribes to Bluetooth data
   - Decodes protobuf (FromRadio → MeshPacket)
   - Updates node database
   - Publishes domain events (messageReceived, nodeInfoUpdated, etc.)

3. **Domain Layer**
   - MessagingService subscribes to MeshtasticClient events
   - Stores messages in repository
   - Applies business logic (filtering, routing)
   - Publishes high-level events

4. **ViewModel Layer**
   - ViewModels subscribe to domain service publishers
   - Transform data for view presentation
   - Handle user actions (send message, etc.)

5. **View Layer**
   - SwiftUI views observe @Published properties
   - Render UI
   - Invoke ViewModel actions

## Dependency Injection

All services are injected via protocols using a simple container pattern:

```swift
class DependencyContainer {
    let bluetoothService: BluetoothServiceProtocol
    let meshtasticClient: MeshtasticClientProtocol
    let messagingService: MessagingServiceProtocol
    let mapService: MapServiceProtocol
    let notificationService: NotificationServiceProtocol
    
    init(
        bluetoothService: BluetoothServiceProtocol? = nil,
        meshtasticClient: MeshtasticClientProtocol? = nil,
        messagingService: MessagingServiceProtocol? = nil,
        mapService: MapServiceProtocol? = nil,
        notificationService: NotificationServiceProtocol? = nil
    ) {
        self.bluetoothService = bluetoothService ?? CoreBluetoothService()
        self.meshtasticClient = meshtasticClient ?? MeshtasticClient(bluetoothService: self.bluetoothService)
        self.messagingService = messagingService ?? MessagingService(client: self.meshtasticClient)
        self.mapService = mapService ?? MapService(client: self.meshtasticClient)
        self.notificationService = notificationService ?? LocalNotificationService()
    }
}
```

## Testing Strategy

- All business logic tested via protocol mocks
- No real Bluetooth hardware required
- Mock services use PassthroughSubject to simulate events
- Test coverage: services, domain logic, ViewModels
- Tests validate message routing, channel management, location updates

