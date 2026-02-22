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
    
    var connectionState: AnyPublisher<BluetoothConnectionState, Never> {
        client.connectionState
    }
    
    var discoveredDevicesPublisher: AnyPublisher<[PeripheralDevice], Never> {
        client.discoveredDevicesPublisher
    }
    
    var discoveredDevices: [PeripheralDevice] {
        client.discoveredDevices
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
    
    init(client: MeshtasticClientProtocol, notificationService: NotificationServiceProtocol) {
        self.client = client
        self.notificationService = notificationService
        
        client.messagesPublisher
            .sink { [weak self] message in
                self?.handleIncomingMessage(message)
            }
            .store(in: &cancellables)
    }
    
    func messages(for channel: UInt32) -> [MeshMessage] {
        messageRepository.filter { $0.channel == channel && !$0.isDirectMessage }.sorted { $0.timestamp < $1.timestamp }
    }
    
    func directMessages(with nodeId: UInt32) -> [MeshMessage] {
        messageRepository.filter { $0.isDirectMessage && (($0.from == nodeId && $0.to == client.myNodeId) || ($0.to == nodeId && $0.from == client.myNodeId)) }.sorted { $0.timestamp < $1.timestamp }
    }
    
    func channels() -> [MeshChannel] {
        client.channels
    }
    
    func sendMessage(text: String, to channel: UInt32) async throws {
        try await client.sendMessage(text: text, to: nil, channel: channel)
        let message = MeshMessage(id: UInt32.random(in: 1...UInt32.max), from: client.myNodeId ?? 0, to: 0xFFFFFFFF, channel: channel, text: text, timestamp: Date(), isFromMe: true)
        messageRepository.append(message)
        newMessageSubject.send(message)
    }
    
    func sendDirectMessage(text: String, to nodeId: UInt32) async throws {
        try await client.sendMessage(text: text, to: nodeId, channel: 0)
        let message = MeshMessage(id: UInt32.random(in: 1...UInt32.max), from: client.myNodeId ?? 0, to: nodeId, channel: 0, text: text, timestamp: Date(), isFromMe: true)
        messageRepository.append(message)
        newMessageSubject.send(message)
    }
    
    func recentConversations() -> [Conversation] {
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
        
        return conversations.sorted { ($0.lastMessage?.timestamp ?? .distantPast) > ($1.lastMessage?.timestamp ?? .distantPast) }
    }
    
    func startScanning() {
        client.startScanning()
    }
    
    func stopScanning() {
        client.stopScanning()
    }
    
    func connect(to deviceIdentifier: UUID) async throws {
        try await client.connect(to: deviceIdentifier)
    }
    
    func disconnect() {
        client.disconnect()
    }
    
    private func handleIncomingMessage(_ message: MeshMessage) {
        messageRepository.append(message)
        newMessageSubject.send(message)
        
        if !message.isFromMe, let node = client.node(for: message.from) {
            notificationService.notifyNewMessage(message, from: node)
        }
    }
}
