# Firefly - Meshtastic Messaging App

A production-quality iOS application for connecting to Meshtastic devices via Bluetooth, enabling mesh network communication at music festivals and events.

## Features

- **Bluetooth Connectivity**: Connects to Meshtastic devices over BLE
- **Channel Messaging**: Public channel-based group chat
- **Direct Messages**: Private 1:1 messaging with other nodes
- **Live Map**: Real-time GPS tracking of friends on a map
- **Push Notifications**: Local notifications for new messages when app is backgrounded
- **EDM-Inspired UI**: Dark-mode-first design with subtle neon accents

## Architecture

This project follows a **clean MVVM architecture** with **protocol-oriented design** and **dependency injection**.

### Layer Structure

```
┌─────────────────────────────────────────┐
│            SwiftUI Views                │
│   (InboxView, ConversationView, Map)    │
└──────────────┬──────────────────────────┘
               │
┌──────────────▼──────────────────────────┐
│           ViewModels                    │
│  (InboxVM, ConversationVM, MapVM)       │
└──────────────┬──────────────────────────┘
               │
┌──────────────▼──────────────────────────┐
│        Domain Services                  │
│   (MessagingService, MapService)        │
└──────────────┬──────────────────────────┘
               │
┌──────────────▼──────────────────────────┐
│       Meshtastic Client                 │
│  (Protobuf encode/decode, node DB)      │
└──────────────┬──────────────────────────┘
               │
┌──────────────▼──────────────────────────┐
│      Bluetooth Transport                │
│    (CoreBluetooth BLE layer)            │
└─────────────────────────────────────────┘
```

### Key Design Principles

1. **Protocol-Oriented**: All services defined as protocols for testability
2. **Dependency Injection**: Services injected via constructor, not singletons
3. **No Global State**: All state managed through Combine publishers
4. **Testable**: Comprehensive unit tests with mock implementations
5. **MVVM**: Clear separation between business logic and UI

## Project Structure

```
Firefly/
├── Core/
│   ├── Models/              # Data models (MeshNode, MeshMessage, etc.)
│   ├── Protocols/           # Service protocol definitions
│   └── DependencyContainer.swift
├── Services/
│   ├── Bluetooth/           # CoreBluetooth BLE implementation
│   ├── Meshtastic/          # Meshtastic client + protobuf
│   └── Notification/        # Local notification service
├── Domain/
│   ├── Messaging/           # Messaging business logic
│   └── Map/                 # Map service logic
├── ViewModels/              # SwiftUI view models
├── Views/                   # SwiftUI views
├── Mocks/                   # Mock implementations for testing
└── Tests/                   # Unit tests
```

## Protocols

### BluetoothServiceProtocol
Manages CoreBluetooth connection lifecycle, scanning, and data transmission.

### MeshtasticClientProtocol
High-level Meshtastic API for sending/receiving messages and managing nodes.

### MessagingServiceProtocol
Domain logic for messaging: persistence, filtering, channel management.

### MapServiceProtocol
Manages node locations and provides map annotations.

### NotificationServiceProtocol
Handles local push notifications for new messages.

## Models

### MeshNode
Represents a node in the mesh network with metadata (name, location, etc.)

### MeshMessage
A text message sent/received over the mesh

### MeshChannel
A Meshtastic channel (primary, secondary, etc.)

### NodeLocation
GPS coordinates and timestamp for a node

## Testing

All business logic is unit tested using **Swift Testing** framework.

Run tests:
```bash
cmd+U in Xcode
```

### Test Coverage

- ✅ Messaging service (send/receive/filter)
- ✅ Meshtastic client (protobuf encode/decode)
- ✅ ViewModels (InboxVM, ConversationVM, MapVM)
- ✅ Connection state management
- ✅ Notification triggering

### Mock Services

All protocols have mock implementations in the `Mocks/` folder:
- `MockBluetoothService`
- `MockMeshtasticClient`
- `MockMessagingService`
- `MockMapService`
- `MockNotificationService`

## Usage

### Production

```swift
let container = DependencyContainer.production()
let inboxView = InboxView(container: container)
```

### Testing

```swift
let mockMessaging = MockMessagingService()
let viewModel = InboxViewModel(messagingService: mockMessaging)

mockMessaging.simulateMessage(testMessage)
```

## Requirements

- iOS 17.0+
- Xcode 15.0+
- Swift 5.9+
- SwiftProtobuf (for Meshtastic protobufs)

## Dependencies

- **SwiftProtobuf**: Protocol buffer support for Meshtastic messages
- **CoreBluetooth**: BLE connectivity
- **MapKit**: Map view integration
- **UserNotifications**: Local push notifications
- **Combine**: Reactive data flow

## Future Enhancements

- [ ] Persistent message storage (CoreData or SwiftData)
- [ ] Full protobuf implementation (currently simplified)
- [ ] Message encryption visualization
- [ ] Friend management UI
- [ ] Multiple device support
- [ ] Message search
- [ ] Media attachments
- [ ] Group channel creation
- [ ] Settings/preferences

## License

MIT License - See LICENSE file for details

## References

- [Meshtastic-Apple](https://github.com/meshtastic/Meshtastic-Apple)
- [Meshtastic Protocol Documentation](https://meshtastic.org)
- [GitCraft Architecture Reference](https://github.com/jakeholland/GitCraft)
- [Halite Architecture Reference](https://github.com/jakeholland/Halite)

## Author

Jake Holland - 2026
