# Firefly - Complete File Index

## 📱 Application Files

### App Entry Point
- `FireflyApp.swift` - Main app with dependency injection

## 🏗️ Core Layer (10 files)

### Models (4 files)
- `Core/Models/MeshNode.swift` - Node representation with metadata
- `Core/Models/MeshMessage.swift` - Text message model
- `Core/Models/MeshChannel.swift` - Channel configuration
- `Core/Models/NodeLocation.swift` - GPS coordinates wrapper

### Protocols (5 files)
- `Core/Protocols/BluetoothServiceProtocol.swift` - BLE transport abstraction
- `Core/Protocols/MeshtasticClientProtocol.swift` - High-level mesh client
- `Core/Protocols/MessagingServiceProtocol.swift` - Messaging business logic
- `Core/Protocols/MapServiceProtocol.swift` - Location/map services
- `Core/Protocols/NotificationServiceProtocol.swift` - Push notifications

### Dependency Injection (1 file)
- `Core/DependencyContainer.swift` - DI container for all services

## ⚙️ Services Layer (6 files)

### Bluetooth (2 files)
- `Services/Bluetooth/MeshtasticBLEConstants.swift` - BLE UUIDs and constants
- `Services/Bluetooth/CoreBluetoothService.swift` - Full CoreBluetooth implementation

### Meshtastic (1 file)
- `Services/Meshtastic/MeshtasticClient.swift` - Protobuf encode/decode + node DB

### Notifications (1 file)
- `Services/Notification/LocalNotificationService.swift` - Local push notifications

## 🎯 Domain Layer (2 files)

### Messaging
- `Domain/Messaging/MessagingService.swift` - Message routing, persistence, filtering

### Map
- `Domain/Map/MapService.swift` - Location management and map annotations

## 🎨 Presentation Layer (6 files)

### ViewModels (3 files)
- `ViewModels/InboxViewModel.swift` - Conversation list logic
- `ViewModels/ConversationViewModel.swift` - Chat thread logic
- `ViewModels/MapViewModel.swift` - Map display logic

### Views (3 files)
- `Views/InboxView.swift` - Main conversation list + EDM theme
- `Views/ConversationView.swift` - Message thread UI
- `Views/MapView.swift` - MapKit integration

## 🧪 Testing Layer (11 files)

### Mocks (5 files)
- `Mocks/MockBluetoothService.swift` - Bluetooth mock with simulation
- `Mocks/MockMeshtasticClient.swift` - Client mock with helpers
- `Mocks/MockMessagingService.swift` - Messaging mock
- `Mocks/MockMapService.swift` - Map service mock
- `Mocks/MockNotificationService.swift` - Notification mock

### Tests (6 files)
- `Tests/MessagingServiceTests.swift` - 7 messaging tests
- `Tests/MeshtasticClientTests.swift` - 5 client tests
- `Tests/InboxViewModelTests.swift` - 4 inbox tests
- `Tests/ConversationViewModelTests.swift` - 6 conversation tests
- `Tests/MapViewModelTests.swift` - 4 map tests
- `Tests/DependencyContainerTests.swift` - 3 DI tests

## 📚 Documentation (5 files)

- `Architecture.md` - Complete architecture overview with diagrams
- `README.md` - Project documentation and features
- `IMPLEMENTATION_GUIDE.md` - Developer guide for extending codebase
- `INFO_PLIST_GUIDE.md` - Required permissions and configuration
- `PROJECT_SUMMARY.md` - Complete implementation summary
- `SETUP_CHECKLIST.md` - Step-by-step Xcode setup guide

## 📊 Statistics

### Code Files
- **Total Files**: 35 Swift files
- **Lines of Code**: ~3,500+ LOC
- **Test Coverage**: 29 tests across 6 test suites

### Architecture Breakdown
- **Models**: 4 files
- **Protocols**: 5 files
- **Services**: 4 implementations
- **Domain Logic**: 2 services
- **ViewModels**: 3 files
- **Views**: 3 files
- **Mocks**: 5 files
- **Tests**: 6 files
- **Infrastructure**: 2 files (App + Container)
- **Documentation**: 6 markdown files

## 🗂️ File Dependency Graph

```
FireflyApp.swift
    └── DependencyContainer.swift
            ├── CoreBluetoothService.swift
            │       └── MeshtasticBLEConstants.swift
            ├── MeshtasticClient.swift
            │       └── BluetoothServiceProtocol
            ├── MessagingService.swift
            │       ├── MeshtasticClientProtocol
            │       └── NotificationServiceProtocol
            ├── MapService.swift
            │       └── MeshtasticClientProtocol
            └── LocalNotificationService.swift

InboxView.swift
    └── InboxViewModel.swift
            └── MessagingServiceProtocol

ConversationView.swift
    └── ConversationViewModel.swift
            └── MessagingServiceProtocol

MapView.swift
    └── MapViewModel.swift
            └── MapServiceProtocol
```

## 🔍 Quick Reference

### Need to...
- **Understand architecture?** → Read `Architecture.md`
- **Set up project?** → Follow `SETUP_CHECKLIST.md`
- **Add features?** → Check `IMPLEMENTATION_GUIDE.md`
- **Configure permissions?** → See `INFO_PLIST_GUIDE.md`
- **See what's done?** → Review `PROJECT_SUMMARY.md`
- **Find a specific file?** → Use this index!

### File Locations

```
Firefly/
├── FireflyApp.swift
├── Core/
│   ├── Models/
│   │   ├── MeshNode.swift
│   │   ├── MeshMessage.swift
│   │   ├── MeshChannel.swift
│   │   └── NodeLocation.swift
│   ├── Protocols/
│   │   ├── BluetoothServiceProtocol.swift
│   │   ├── MeshtasticClientProtocol.swift
│   │   ├── MessagingServiceProtocol.swift
│   │   ├── MapServiceProtocol.swift
│   │   └── NotificationServiceProtocol.swift
│   └── DependencyContainer.swift
├── Services/
│   ├── Bluetooth/
│   │   ├── MeshtasticBLEConstants.swift
│   │   └── CoreBluetoothService.swift
│   ├── Meshtastic/
│   │   └── MeshtasticClient.swift
│   └── Notification/
│       └── LocalNotificationService.swift
├── Domain/
│   ├── Messaging/
│   │   └── MessagingService.swift
│   └── Map/
│       └── MapService.swift
├── ViewModels/
│   ├── InboxViewModel.swift
│   ├── ConversationViewModel.swift
│   └── MapViewModel.swift
├── Views/
│   ├── InboxView.swift
│   ├── ConversationView.swift
│   └── MapView.swift
├── Mocks/
│   ├── MockBluetoothService.swift
│   ├── MockMeshtasticClient.swift
│   ├── MockMessagingService.swift
│   ├── MockMapService.swift
│   └── MockNotificationService.swift
└── Tests/
    ├── MessagingServiceTests.swift
    ├── MeshtasticClientTests.swift
    ├── InboxViewModelTests.swift
    ├── ConversationViewModelTests.swift
    ├── MapViewModelTests.swift
    └── DependencyContainerTests.swift
```

## 🎯 Key Entry Points

### For Users
1. Start at `FireflyApp.swift` - App entry point
2. View `InboxView.swift` - Main UI

### For Developers
1. Read `Architecture.md` - Understand design
2. Check `DependencyContainer.swift` - See service wiring
3. Review protocols in `Core/Protocols/` - Understand contracts

### For Testers
1. See `Tests/` folder - All test suites
2. Use `Mocks/` - Mock implementations
3. Run tests with Cmd+U

## 📝 File Purposes Summary

| File | Purpose | Lines |
|------|---------|-------|
| FireflyApp.swift | App entry + DI setup | ~25 |
| DependencyContainer.swift | Service injection | ~70 |
| CoreBluetoothService.swift | BLE implementation | ~250 |
| MeshtasticClient.swift | Mesh protocol handler | ~300 |
| MessagingService.swift | Business logic | ~170 |
| MapService.swift | Location logic | ~100 |
| InboxViewModel.swift | UI logic | ~100 |
| ConversationViewModel.swift | Chat logic | ~120 |
| MapViewModel.swift | Map logic | ~90 |
| InboxView.swift | Main UI | ~180 |
| ConversationView.swift | Chat UI | ~150 |
| MapView.swift | Map UI | ~120 |

**Total Implementation**: ~1,675 lines of production code + ~1,850 lines of test code

## 🚀 Ready to Build!

All files are organized, documented, and tested. Follow the `SETUP_CHECKLIST.md` to get started.
