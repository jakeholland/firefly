//
//  MockMeshtasticClient.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import Combine

/// Mock Meshtastic client for testing
final class MockMeshtasticClient: MeshtasticClientProtocol {
    private let connectionStateSubject = PassthroughSubject<BluetoothConnectionState, Never>()
    var connectionState: AnyPublisher<BluetoothConnectionState, Never> {
        connectionStateSubject.eraseToAnyPublisher()
    }
    
    private let messagesSubject = PassthroughSubject<MeshMessage, Never>()
    var messagesPublisher: AnyPublisher<MeshMessage, Never> {
        messagesSubject.eraseToAnyPublisher()
    }
    
    private let nodeUpdatesSubject = PassthroughSubject<MeshNode, Never>()
    var nodeUpdatesPublisher: AnyPublisher<MeshNode, Never> {
        nodeUpdatesSubject.eraseToAnyPublisher()
    }
    
    private let channelUpdatesSubject = PassthroughSubject<MeshChannel, Never>()
    var channelUpdatesPublisher: AnyPublisher<MeshChannel, Never> {
        channelUpdatesSubject.eraseToAnyPublisher()
    }
    
    private let discoveredDevicesSubject = CurrentValueSubject<[PeripheralDevice], Never>([])
    var discoveredDevicesPublisher: AnyPublisher<[PeripheralDevice], Never> {
        discoveredDevicesSubject.eraseToAnyPublisher()
    }
    
    var discoveredDevices: [PeripheralDevice] {
        discoveredDevicesSubject.value
    }
    
    var nodes: [MeshNode] = []
    var channels: [MeshChannel] = []
    var myNodeId: UInt32?
    
    var didCallConnect = false
    var didCallDisconnect = false
    var didCallStartScanning = false
    var didCallStopScanning = false
    var sentMessages: [(text: String, destination: UInt32?, channel: UInt32)] = []
    
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
    
    func sendMessage(text: String, to destination: UInt32?, channel: UInt32) async throws {
        sentMessages.append((text, destination, channel))
    }
    
    func requestNodeInfo(for nodeId: UInt32) async throws {
        // No-op for mock
    }
    
    func node(for id: UInt32) -> MeshNode? {
        nodes.first { $0.id == id }
    }
    
    // Test helpers
    func simulateMessage(_ message: MeshMessage) {
        messagesSubject.send(message)
    }
    
    func simulateNodeUpdate(_ node: MeshNode) {
        if let index = nodes.firstIndex(where: { $0.id == node.id }) {
            nodes[index] = node
        } else {
            nodes.append(node)
        }
        nodeUpdatesSubject.send(node)
    }
    
    func simulateChannelUpdate(_ channel: MeshChannel) {
        if let index = channels.firstIndex(where: { $0.id == channel.id }) {
            channels[index] = channel
        } else {
            channels.append(channel)
        }
        channelUpdatesSubject.send(channel)
    }
    
    func simulateDiscoveredDevices(_ devices: [PeripheralDevice]) {
        discoveredDevicesSubject.send(devices)
    }
}
