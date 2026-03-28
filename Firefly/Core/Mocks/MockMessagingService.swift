//
//  MockMessagingService.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import Combine

/// Mock messaging service for testing
final class MockMessagingService: MessagingServiceProtocol {
    private let connectionStateSubject = PassthroughSubject<BluetoothConnectionState, Never>()
    var connectionState: AnyPublisher<BluetoothConnectionState, Never> {
        connectionStateSubject.eraseToAnyPublisher()
    }
    
    private let newMessageSubject = PassthroughSubject<MeshMessage, Never>()
    var newMessagePublisher: AnyPublisher<MeshMessage, Never> {
        newMessageSubject.eraseToAnyPublisher()
    }
    
    private let discoveredDevicesSubject = CurrentValueSubject<[PeripheralDevice], Never>([])
    var discoveredDevicesPublisher: AnyPublisher<[PeripheralDevice], Never> {
        discoveredDevicesSubject.eraseToAnyPublisher()
    }
    
    var discoveredDevices: [PeripheralDevice] {
        discoveredDevicesSubject.value
    }
    
    var myNodeId: UInt32?
    
    var allMessages: [MeshMessage] = []
    private var channelsList: [MeshChannel] = []
    private var nodesList: [MeshNode] = []
    
    var sentMessages: [(text: String, channel: UInt32)] = []
    var sentDirectMessages: [(text: String, nodeId: UInt32)] = []
    var didCallStartScanning = false
    var didCallStopScanning = false
    var didCallConnect = false
    var didCallDisconnect = false
    
    init() {
        // Empty initializer for mock service
    }
    
    func messages(for channel: UInt32) -> [MeshMessage] {
        allMessages.filter { $0.channel == channel && !$0.isDirectMessage }
    }
    
    func directMessages(with nodeId: UInt32) -> [MeshMessage] {
        allMessages.filter { message in
            message.isDirectMessage && (message.from == nodeId || message.to == nodeId)
        }
    }
    
    func channels() -> [MeshChannel] {
        channelsList
    }
    
    func nodes() -> [MeshNode] {
        nodesList
    }
    
    func sendMessage(text: String, to channel: UInt32) async throws {
        sentMessages.append((text, channel))
    }
    
    func sendDirectMessage(text: String, to nodeId: UInt32) async throws {
        sentDirectMessages.append((text, nodeId))
    }
    
    func startScanning() {
        didCallStartScanning = true
    }
    
    func stopScanning() {
        didCallStopScanning = true
    }
    
    func connect(to deviceIdentifier: UUID) async throws {
        didCallConnect = true
        connectionStateSubject.send(.connected)
    }
    
    func disconnect() {
        didCallDisconnect = true
        connectionStateSubject.send(.disconnected)
    }
    
    func recentConversations() -> [Conversation] {
        var conversations: [Conversation] = []
        
        // Add channels
        for channel in channelsList {
            let msgs = messages(for: channel.id)
            conversations.append(.channel(channel, lastMessage: msgs.last))
        }
        
        // Add DMs
        let dmNodeIds = Set(allMessages.filter { $0.isDirectMessage }.flatMap { [$0.from, $0.to] })
        for nodeId in dmNodeIds {
            let dms = directMessages(with: nodeId)
            let node = MeshNode(
                id: nodeId,
                shortName: "Node\(nodeId)",
                longName: "Test Node \(nodeId)",
                hardwareModel: nil,
                macAddress: nil,
                lastHeard: nil,
                location: nil,
                isFriend: false
            )
            conversations.append(.directMessage(node: node, lastMessage: dms.last))
        }
        
        return conversations
    }
    
    // Test helpers
    func simulateMessage(_ message: MeshMessage) {
        allMessages.append(message)
        newMessageSubject.send(message)
    }
    
    func setChannels(_ channels: [MeshChannel]) {
        channelsList = channels
    }
    
    func setNodes(_ nodes: [MeshNode]) {
        nodesList = nodes
    }
    
    func simulateDiscoveredDevices(_ devices: [PeripheralDevice]) {
        discoveredDevicesSubject.send(devices)
    }
}
