//
//  MessagingService.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import Combine

final class MessagingService: MessagingServiceProtocol {
    private let client: MeshtasticClientProtocol
    private let notificationService: NotificationServiceProtocol
    private let persistenceService: PersistenceService?
    
    var connectionState: AnyPublisher<BluetoothConnectionState, Never> {
        client.connectionState
    }
    
    var discoveredDevicesPublisher: AnyPublisher<[PeripheralDevice], Never> {
        client.discoveredDevicesPublisher
    }
    
    var discoveredDevices: [PeripheralDevice] {
        client.discoveredDevices
    }
    
    var myNodeId: UInt32? {
        client.myNodeId
    }
    
    private let newMessageSubject = PassthroughSubject<MeshMessage, Never>()
    var newMessagePublisher: AnyPublisher<MeshMessage, Never> {
        newMessageSubject.eraseToAnyPublisher()
    }
    
    private var messageRepository: [MeshMessage] = []
    private var cancellables = Set<AnyCancellable>()
    
    var allMessages: [MeshMessage] {
        messageRepository.sorted { $0.timestamp > $1.timestamp }
    }
    
    init(client: MeshtasticClientProtocol, notificationService: NotificationServiceProtocol, persistenceService: PersistenceService? = nil) {
        self.client = client
        self.notificationService = notificationService
        self.persistenceService = persistenceService
        
        NSLog("💬 [Messaging] Initializing MessagingService (persistence: \(persistenceService != nil ? "enabled" : "disabled"))")
        
        // Load persisted messages if available
        if let persistenceService = persistenceService {
            Task { @MainActor in
                messageRepository = persistenceService.fetchAllMessages()
                NSLog("💬 [Messaging] 📚 Loaded \(messageRepository.count) messages from persistence")
            }
        }
        
        client.messagesPublisher
            .sink { [weak self] message in
                self?.handleIncomingMessage(message)
            }
            .store(in: &cancellables)
        
        // Subscribe to node updates and persist them if persistence is available
        if persistenceService != nil {
            client.nodeUpdatesPublisher
                .sink { [weak self] node in
                    guard let self = self, let persistenceService = self.persistenceService else { return }
                    Task { @MainActor in
                        persistenceService.saveNode(node)
                    }
                }
                .store(in: &cancellables)
        }
        
        NSLog("💬 [Messaging] ✅ Subscribed to message and node publishers")
    }
    
    func messages(for channel: UInt32) -> [MeshMessage] {
        let filtered = messageRepository.filter { $0.channel == channel && !$0.isDirectMessage }.sorted { $0.timestamp < $1.timestamp }
        NSLog("💬 [Messaging] 📋 Retrieved \(filtered.count) messages for channel \(channel)")
        return filtered
    }
    
    func directMessages(with nodeId: UInt32) -> [MeshMessage] {
        let filtered = messageRepository.filter { $0.isDirectMessage && (($0.from == nodeId && $0.to == client.myNodeId) || ($0.to == nodeId && $0.from == client.myNodeId)) }.sorted { $0.timestamp < $1.timestamp }
        NSLog("💬 [Messaging] 📋 Retrieved \(filtered.count) direct messages with node 0x\(String(nodeId, radix: 16))")
        return filtered
    }
    
    func channels() -> [MeshChannel] {
        let channels = client.channels
        NSLog("💬 [Messaging] 📋 Retrieved \(channels.count) channels")
        return channels
    }
    
    func sendMessage(text: String, to channel: UInt32) async throws {
        NSLog("💬 [Messaging] 📤 Sending message to channel \(channel): \"\(text.prefix(50))\(text.count > 50 ? "..." : "")\"")
        
        do {
            try await client.sendMessage(text: text, to: nil, channel: channel)
            
            let message = MeshMessage(id: UInt32.random(in: 1...UInt32.max), from: client.myNodeId ?? 0, to: 0xFFFFFFFF, channel: channel, text: text, timestamp: Date(), isFromMe: true)
            messageRepository.append(message)
            newMessageSubject.send(message)
            
            // Persist the message if persistence is available
            if let persistenceService = persistenceService {
                Task { @MainActor in
                    persistenceService.saveMessage(message)
                }
            }
            
            NSLog("💬 [Messaging] ✅ Message sent and stored (repository now has \(messageRepository.count) messages)")
        } catch {
            NSLog("💬 [Messaging] ❌ Failed to send message: \(error.localizedDescription)")
            throw error
        }
    }
    
    func sendDirectMessage(text: String, to nodeId: UInt32) async throws {
        NSLog("💬 [Messaging] 📤 Sending direct message to node 0x\(String(nodeId, radix: 16)): \"\(text.prefix(50))\(text.count > 50 ? "..." : "")\"")
        
        do {
            try await client.sendMessage(text: text, to: nodeId, channel: 0)
            
            let message = MeshMessage(id: UInt32.random(in: 1...UInt32.max), from: client.myNodeId ?? 0, to: nodeId, channel: 0, text: text, timestamp: Date(), isFromMe: true)
            messageRepository.append(message)
            newMessageSubject.send(message)
            
            // Persist the message if persistence is available
            if let persistenceService = persistenceService {
                Task { @MainActor in
                    persistenceService.saveMessage(message)
                }
            }
            
            NSLog("💬 [Messaging] ✅ Direct message sent and stored (repository now has \(messageRepository.count) messages)")
        } catch {
            NSLog("💬 [Messaging] ❌ Failed to send direct message: \(error.localizedDescription)")
            throw error
        }
    }
    
    func recentConversations() -> [Conversation] {
        NSLog("💬 [Messaging] 📋 Building recent conversations list")
        var conversations: [Conversation] = []
        
        for channel in client.channels {
            let channelMessages = messages(for: channel.id)
            conversations.append(.channel(channel, lastMessage: channelMessages.last))
        }
        
        let dmNodeIds = Set(messageRepository.filter { $0.isDirectMessage }.flatMap { [$0.from, $0.to] }.filter { $0 != client.myNodeId })

        for nodeId in dmNodeIds {
            guard
                let nodeId = nodeId,
                let node = client.node(for: nodeId) else {
                continue
            }
            let dms = directMessages(with: nodeId)
            conversations.append(.directMessage(node: node, lastMessage: dms.last))
        }
        
        let sorted = conversations.sorted { ($0.lastMessage?.timestamp ?? .distantPast) > ($1.lastMessage?.timestamp ?? .distantPast) }
        NSLog("💬 [Messaging] ✅ Found \(sorted.count) conversations (\(client.channels.count) channels, \(dmNodeIds.count) DMs)")
        return sorted
    }
    
    func startScanning() {
        NSLog("💬 [Messaging] 📡 Starting device scan")
        client.startScanning()
    }
    
    func stopScanning() {
        NSLog("💬 [Messaging] 🛑 Stopping device scan")
        client.stopScanning()
    }
    
    func connect(to deviceIdentifier: UUID) async throws {
        NSLog("💬 [Messaging] 🔌 Connecting to device: \(deviceIdentifier.uuidString)")
        try await client.connect(to: deviceIdentifier)
        NSLog("💬 [Messaging] ✅ Connected successfully")
    }
    
    func disconnect() {
        NSLog("💬 [Messaging] 🔌 Disconnecting from device")
        client.disconnect()
    }
    
    private func handleIncomingMessage(_ message: MeshMessage) {
        NSLog("💬 [Messaging] 📥 Handling incoming message (ID: \(message.id), from: 0x\(String(message.from, radix: 16)), isFromMe: \(message.isFromMe))")
        NSLog("💬 [Messaging]    Text: \"\(message.text.prefix(50))\(message.text.count > 50 ? "..." : "")\"")
        
        messageRepository.append(message)
        newMessageSubject.send(message)
        
        // Persist the message if persistence is available
        if let persistenceService = persistenceService {
            Task { @MainActor in
                persistenceService.saveMessage(message)
            }
        }
        
        NSLog("💬 [Messaging] 💾 Message stored (repository now has \(messageRepository.count) messages)")
        
        if !message.isFromMe, let node = client.node(for: message.from) {
            NSLog("💬 [Messaging] 🔔 Triggering notification for message from \(node.displayName)")
            notificationService.notifyNewMessage(message, from: node)
        }
    }
}
