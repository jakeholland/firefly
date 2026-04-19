//
//  MockMessagingService.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import Combine

/// Mock messaging service for testing and simulator builds
final class MockMessagingService: MessagingServiceProtocol {
    private let connectionStateSubject: CurrentValueSubject<BluetoothConnectionState, Never>
    var connectionState: AnyPublisher<BluetoothConnectionState, Never> {
        connectionStateSubject.eraseToAnyPublisher()
    }
    
    private let newMessageSubject = PassthroughSubject<MeshMessage, Never>()
    var newMessagePublisher: AnyPublisher<MeshMessage, Never> {
        newMessageSubject.eraseToAnyPublisher()
    }

    private let nodeUpdatesSubject = PassthroughSubject<MeshNode, Never>()
    var nodeUpdatesPublisher: AnyPublisher<MeshNode, Never> {
        nodeUpdatesSubject.eraseToAnyPublisher()
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
    
    init(initialConnectionState: BluetoothConnectionState = .disconnected) {
        connectionStateSubject = CurrentValueSubject(initialConnectionState)
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
        connectionStateSubject.value = .connected
    }

    func disconnect() {
        didCallDisconnect = true
        connectionStateSubject.value = .disconnected
    }
    
    func recentConversations() -> [Conversation] {
        var conversations: [Conversation] = []

        // Active (non-disabled) channels
        for channel in channelsList where channel.role != .disabled {
            let msgs = messages(for: channel.id)
            conversations.append(.channel(channel, lastMessage: msgs.last))
        }

        // DM threads — collect unique remote node IDs (exclude our own ID)
        let selfId = myNodeId   // capture as non-optional context so Swift doesn't widen the set type
        let dmNodeIds = Set(
            allMessages
                .filter { $0.isDirectMessage }
                .flatMap { [$0.from, $0.to] }
                .filter { id in
                    guard let selfId else { return true }
                    return id != selfId
                }
        )
        for nodeId in dmNodeIds {
            let dms = directMessages(with: nodeId)
            // Use a known node from nodesList when available; fall back to a stub
            let node = nodesList.first { $0.id == nodeId } ?? MeshNode(
                id: nodeId,
                shortName: String(format: "%08X", nodeId),
                longName: "Unknown Node",
                hardwareModel: nil,
                macAddress: nil,
                lastHeard: nil,
                location: nil,
                isFriend: false
            )
            conversations.append(.directMessage(node: node, lastMessage: dms.last))
        }

        return conversations.sorted {
            ($0.lastMessage?.timestamp ?? .distantPast) > ($1.lastMessage?.timestamp ?? .distantPast)
        }
    }
    
    func renameNode(longName: String) async throws {
        // no-op for mock
    }

    func myNodeInfo() -> MeshNode? {
        guard let id = myNodeId else { return nil }
        return nodesList.first { $0.id == id }
    }

    func deleteConversation(_ conversation: Conversation) {
        if case .directMessage(let node, _) = conversation {
            allMessages.removeAll {
                $0.isDirectMessage && ($0.from == node.id || $0.to == node.id)
            }
        }
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

    func simulateNodeUpdate(_ node: MeshNode) {
        nodesList.removeAll { $0.id == node.id }
        nodesList.append(node)
        nodeUpdatesSubject.send(node)
    }
}
