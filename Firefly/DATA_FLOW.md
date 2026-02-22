# Firefly - Data Flow Diagrams

## 1. Sending a Message Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                         USER ACTION                              │
│              User types message and taps send                    │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                    ConversationView                              │
│                    Button tap event                              │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                 ConversationViewModel                            │
│              viewModel.sendMessage()                             │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                  MessagingService                                │
│   messagingService.sendMessage(text: "Hello", to: channel: 0)   │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                  MeshtasticClient                                │
│  client.sendMessage(text: "Hello", to: nil, channel: 0)         │
│               - Creates protobuf packet                          │
│               - Encodes message                                  │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│              CoreBluetoothService                                │
│         bluetoothService.send(packetData)                        │
│         - Writes to ToRadio characteristic                       │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                   BLUETOOTH DEVICE                               │
│                  (Meshtastic Node)                               │
│              Message sent to mesh network                        │
└─────────────────────────────────────────────────────────────────┘
```

## 2. Receiving a Message Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                   BLUETOOTH DEVICE                               │
│              Message received from mesh                          │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│              CoreBluetoothService                                │
│         peripheral.didUpdateValue(characteristic)                │
│         - Reads FromRadio characteristic                         │
│         - Publishes via receivedDataPublisher                    │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                  MeshtasticClient                                │
│              handleReceivedData(data)                            │
│         - Decodes protobuf packet                                │
│         - Extracts message content                               │
│         - Updates node database                                  │
│         - Publishes via messagesPublisher                        │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                  MessagingService                                │
│           handleIncomingMessage(message)                         │
│         - Stores in message repository                           │
│         - Publishes via newMessagePublisher                      │
│         - Triggers notification service                          │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                ┌───────────┴──────────┐
                │                      │
                ▼                      ▼
┌──────────────────────┐   ┌──────────────────────────┐
│ ConversationViewModel│   │ LocalNotificationService │
│  - Receives update   │   │  - Shows notification    │
│  - Refreshes messages│   │    (if app backgrounded) │
└──────────┬───────────┘   └──────────────────────────┘
           │
           ▼
┌──────────────────────┐
│   ConversationView   │
│  - Updates UI        │
│  - Shows new message │
└──────────────────────┘
```

## 3. Connection Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                         USER ACTION                              │
│               User taps "Connect" button                         │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                      InboxViewModel                              │
│                  viewModel.connect()                             │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│              CoreBluetoothService                                │
│             bluetoothService.startScanning()                     │
│         - Scans for Meshtastic service UUID                      │
│         - Updates connectionState to .scanning                   │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                  DEVICE DISCOVERED                               │
│        centralManager.didDiscover(peripheral)                    │
│         - Auto-connects to first device                          │
│         - Updates connectionState to .connecting                 │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│              SERVICES & CHARACTERISTICS                          │
│         peripheral.didDiscoverServices                           │
│         peripheral.didDiscoverCharacteristics                    │
│         - Finds ToRadio & FromRadio                              │
│         - Subscribes to notifications                            │
│         - Updates connectionState to .connected                  │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                  State Propagation                               │
│         connectionState publisher emits .connected               │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                ┌───────────┴──────────┐
                │                      │
                ▼                      ▼
┌──────────────────────┐   ┌──────────────────────────┐
│  MeshtasticClient    │   │    InboxViewModel        │
│  - Ready to send     │   │  - Updates UI            │
│  - Receives messages │   │  - Shows "Connected"     │
└──────────────────────┘   └──────────────────────────┘
```

## 4. Map Update Flow

```
┌─────────────────────────────────────────────────────────────────┐
│              Node sends position update                          │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│              CoreBluetoothService                                │
│         Receives position packet via BLE                         │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                  MeshtasticClient                                │
│         handlePositionUpdate(from: nodeId, data)                 │
│         - Parses GPS coordinates                                 │
│         - Updates node in database                               │
│         - Publishes via nodeUpdatesPublisher                     │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                     MapService                                   │
│            Receives node update                                  │
│         - Creates NodeMapAnnotation                              │
│         - Publishes via nodeLocationsPublisher                   │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                   MapViewModel                                   │
│              refreshAnnotations()                                │
│         - Updates @Published annotations array                   │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                      MapView                                     │
│              SwiftUI re-renders map                              │
│         - Shows updated marker position                          │
└─────────────────────────────────────────────────────────────────┘
```

## 5. Dependency Injection Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                      FireflyApp.swift                            │
│         let container = DependencyContainer.production()         │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                  DependencyContainer                             │
│                                                                  │
│  1. Creates CoreBluetoothService                                 │
│  2. Injects into MeshtasticClient(bluetooth)                     │
│  3. Creates LocalNotificationService                             │
│  4. Injects into MessagingService(client, notification)          │
│  5. Injects into MapService(client)                              │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                      InboxView(container)                        │
│              Creates InboxViewModel(messagingService)            │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                ┌───────────┴──────────┐
                │                      │
                ▼                      ▼
┌──────────────────────┐   ┌──────────────────────────┐
│  ConversationView    │   │       MapView            │
│  (messagingService)  │   │     (mapService)         │
└──────────────────────┘   └──────────────────────────┘
```

## 6. Test Data Flow (with Mocks)

```
┌─────────────────────────────────────────────────────────────────┐
│                         Test Setup                               │
│              let mockClient = MockMeshtasticClient()             │
│              let service = MessagingService(client: mockClient)  │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Test Execution                              │
│         mockClient.simulateMessage(testMessage)                  │
│         - Calls messagesSubject.send(testMessage)                │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                   MessagingService                               │
│         messagesPublisher subscription receives message          │
│         handleIncomingMessage(testMessage)                       │
│         - Stores in repository                                   │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Test Assertion                              │
│         #expect(service.allMessages.count == 1)                  │
│         #expect(service.allMessages[0].text == "Test")           │
└─────────────────────────────────────────────────────────────────┘
```

## 7. Combine Publishers & Subscribers

```
CoreBluetoothService
│
├─ connectionState: CurrentValueSubject<ConnectionState>
│   └── Subscribers:
│       ├── MeshtasticClient
│       ├── MessagingService
│       └── InboxViewModel
│
└─ receivedDataPublisher: PassthroughSubject<Data>
    └── Subscribers:
        └── MeshtasticClient

MeshtasticClient
│
├─ messagesPublisher: PassthroughSubject<MeshMessage>
│   └── Subscribers:
│       └── MessagingService
│
└─ nodeUpdatesPublisher: PassthroughSubject<MeshNode>
    └── Subscribers:
        └── MapService

MessagingService
│
└─ newMessagePublisher: PassthroughSubject<MeshMessage>
    └── Subscribers:
        ├── InboxViewModel
        └── ConversationViewModel

MapService
│
└─ nodeLocationsPublisher: PassthroughSubject<[NodeMapAnnotation]>
    └── Subscribers:
        └── MapViewModel
```

## 8. State Management

```
                        ┌──────────────────┐
                        │   User Action    │
                        └────────┬─────────┘
                                 │
                                 ▼
                        ┌──────────────────┐
                        │    ViewModel     │
                        │  @Published var  │
                        └────────┬─────────┘
                                 │
                                 ▼
                        ┌──────────────────┐
                        │  SwiftUI View    │
                        │  Auto-refreshes  │
                        └──────────────────┘

Example:
  User taps send
      ↓
  viewModel.sendMessage()
      ↓
  Task { try await service.sendMessage(...) }
      ↓
  await MainActor.run { self.isSending = false }
      ↓
  @Published property changes
      ↓
  SwiftUI re-renders view
```

## Summary

The architecture follows a **unidirectional data flow**:

1. **User Action** → ViewModel
2. **ViewModel** → Service (via protocol)
3. **Service** → Lower-level service/client
4. **Client** → Bluetooth/External
5. **Bluetooth/External** → Client (via Combine)
6. **Client** → Service (via Combine)
7. **Service** → ViewModel (via Combine)
8. **ViewModel** → View (via @Published)

This ensures:
- Clear data flow
- Easy debugging
- Testable components
- Reactive UI updates
