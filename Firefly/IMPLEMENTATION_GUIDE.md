# Implementation Guide

This document provides step-by-step instructions for understanding and extending the Firefly codebase.

## Quick Start

1. **Open the project in Xcode**
2. **Build the project** (Cmd+B)
3. **Run tests** (Cmd+U)
4. **Run the app** (Cmd+R)

## Understanding the Architecture

### 1. Protocols First

Every major component is defined as a protocol:

```swift
protocol MessagingServiceProtocol {
    func sendMessage(text: String, to channel: UInt32) async throws
    var newMessagePublisher: AnyPublisher<MeshMessage, Never> { get }
}
```

This allows:
- Easy testing with mocks
- Swapping implementations
- Clear contracts between layers

### 2. Dependency Injection

Services are injected via constructors:

```swift
class InboxViewModel {
    private let messagingService: MessagingServiceProtocol
    
    init(messagingService: MessagingServiceProtocol) {
        self.messagingService = messagingService
    }
}
```

**Never** use singletons or global state.

### 3. Data Flow with Combine

Services publish events using Combine:

```swift
// Service publishes
let messageSubject = PassthroughSubject<MeshMessage, Never>()

// ViewModel subscribes
messagingService.newMessagePublisher
    .sink { message in
        // Handle message
    }
    .store(in: &cancellables)
```

### 4. MVVM Pattern

**Model** → Core/Models (MeshMessage, MeshNode)  
**ViewModel** → ViewModels/ (InboxViewModel)  
**View** → Views/ (InboxView)

ViewModels are `@MainActor` and expose `@Published` properties:

```swift
@MainActor
class InboxViewModel: ObservableObject {
    @Published var conversations: [Conversation] = []
}
```

## Adding New Features

### Example: Add a "Mark as Read" Feature

#### 1. Update the Model

```swift
// Core/Models/MeshMessage.swift
struct MeshMessage {
    // ... existing properties
    var isRead: Bool = false
}
```

#### 2. Update the Protocol

```swift
// Core/Protocols/MessagingServiceProtocol.swift
protocol MessagingServiceProtocol {
    // ... existing methods
    func markAsRead(_ messageId: UInt32)
}
```

#### 3. Implement in Service

```swift
// Domain/Messaging/MessagingService.swift
func markAsRead(_ messageId: UInt32) {
    if let index = messageRepository.firstIndex(where: { $0.id == messageId }) {
        messageRepository[index].isRead = true
        newMessageSubject.send(messageRepository[index])
    }
}
```

#### 4. Update Mock

```swift
// Mocks/MockMessagingService.swift
func markAsRead(_ messageId: UInt32) {
    if let index = allMessages.firstIndex(where: { $0.id == messageId }) {
        allMessages[index].isRead = true
    }
}
```

#### 5. Add ViewModel Method

```swift
// ViewModels/ConversationViewModel.swift
func markMessageAsRead(_ message: MeshMessage) {
    messagingService.markAsRead(message.id)
}
```

#### 6. Update View

```swift
// Views/ConversationView.swift
MessageBubbleView(message: message)
    .onAppear {
        viewModel.markMessageAsRead(message)
    }
```

#### 7. Write Tests

```swift
// Tests/MessagingServiceTests.swift
@Test("Mark message as read")
func markAsRead() async throws {
    let service = MessagingService(...)
    let message = MeshMessage(...)
    
    service.markAsRead(message.id)
    
    #expect(service.allMessages[0].isRead == true)
}
```

## Testing Strategy

### Unit Tests

Test all business logic in isolation using mocks:

```swift
@Test("Send message to channel")
func sendMessage() async throws {
    let mockClient = MockMeshtasticClient()
    let service = MessagingService(client: mockClient)
    
    try await service.sendMessage(text: "Hello", to: 0)
    
    #expect(mockClient.sentMessages.count == 1)
}
```

### Mock Helpers

Mocks provide test helpers to simulate events:

```swift
let mockClient = MockMeshtasticClient()
mockClient.simulateMessage(testMessage)  // Trigger message received
mockClient.simulateNodeUpdate(testNode)  // Trigger node update
```

## Common Patterns

### 1. Creating a New Service

```swift
// 1. Define protocol
protocol MyServiceProtocol {
    func doSomething() async throws
}

// 2. Implement
class MyService: MyServiceProtocol {
    func doSomething() async throws {
        // Implementation
    }
}

// 3. Create mock
class MockMyService: MyServiceProtocol {
    var didCallDoSomething = false
    
    func doSomething() async throws {
        didCallDoSomething = true
    }
}

// 4. Add to DependencyContainer
class DependencyContainer {
    let myService: MyServiceProtocol
    
    init(myService: MyServiceProtocol? = nil) {
        self.myService = myService ?? MyService()
    }
}
```

### 2. Creating a New ViewModel

```swift
@MainActor
class MyViewModel: ObservableObject {
    @Published var data: [Item] = []
    
    private let service: MyServiceProtocol
    private var cancellables = Set<AnyCancellable>()
    
    init(service: MyServiceProtocol) {
        self.service = service
        
        service.dataPublisher
            .receive(on: DispatchQueue.main)
            .sink { [weak self] newData in
                self?.data = newData
            }
            .store(in: &cancellables)
    }
    
    func performAction() {
        Task {
            try await service.doSomething()
        }
    }
}
```

### 3. Creating a New View

```swift
struct MyView: View {
    @StateObject private var viewModel: MyViewModel
    
    init(container: DependencyContainer = .production()) {
        _viewModel = StateObject(
            wrappedValue: MyViewModel(service: container.myService)
        )
    }
    
    var body: some View {
        List(viewModel.data) { item in
            Text(item.name)
        }
    }
}
```

## Bluetooth/Meshtastic Implementation

### Current Implementation

The current implementation uses a **simplified packet format** for demonstration. 

### Production Implementation

To use real Meshtastic protobufs:

1. **Import the generated protobuf files** (already in project)
2. **Update MeshtasticClient.swift** to use proper ToRadio/FromRadio messages:

```swift
import SwiftProtobuf

func sendMessage(text: String, to destination: UInt32?, channel: UInt32) async throws {
    var toRadio = ToRadio()
    var meshPacket = MeshPacket()
    
    meshPacket.to = destination ?? 0xFFFFFFFF
    meshPacket.from = myNodeId ?? 0
    meshPacket.channel = channel
    
    var data = Data_pb()
    data.portnum = .textMessageApp
    data.payload = text.data(using: .utf8) ?? Data()
    
    meshPacket.decoded = data
    toRadio.packet = meshPacket
    
    let encodedData = try toRadio.serializedData()
    try await bluetoothService.send(encodedData)
}
```

3. **Decode FromRadio messages** properly:

```swift
func handleReceivedData(_ data: Data) {
    guard let fromRadio = try? FromRadio(serializedData: data) else { return }
    
    if fromRadio.hasPacket {
        handleMeshPacket(fromRadio.packet)
    } else if fromRadio.hasMyInfo {
        myNodeId = fromRadio.myInfo.myNodeNum
    } else if fromRadio.hasNodeInfo {
        handleNodeInfo(fromRadio.nodeInfo)
    }
}
```

## Troubleshooting

### Tests Failing

1. Check that async tasks have enough time to complete
2. Verify mocks are properly configured
3. Ensure `@MainActor` is used for ViewModel tests

### Bluetooth Not Connecting

1. Check Info.plist has Bluetooth permissions
2. Verify device is advertising Meshtastic service
3. Check CoreBluetoothService delegate methods

### UI Not Updating

1. Ensure ViewModels use `@Published` properties
2. Check Combine subscriptions use `.receive(on: DispatchQueue.main)`
3. Verify `@StateObject` vs `@ObservedObject` usage

## Best Practices

1. **Always use protocols** for testability
2. **Inject dependencies** via constructors
3. **Test business logic** thoroughly
4. **Keep ViewModels thin** - delegate to services
5. **Use Combine** for reactive updates
6. **Avoid force unwrapping** - use optional binding
7. **Document complex logic** with comments
8. **Follow Swift naming conventions**

## Resources

- [Swift Concurrency](https://docs.swift.org/swift-book/LanguageGuide/Concurrency.html)
- [Combine Framework](https://developer.apple.com/documentation/combine)
- [SwiftUI Documentation](https://developer.apple.com/documentation/swiftui)
- [Swift Testing](https://developer.apple.com/documentation/testing)
- [CoreBluetooth](https://developer.apple.com/documentation/corebluetooth)
