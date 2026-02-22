//
//  MessagingServiceProtocol.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import Combine

/// Protocol for messaging domain logic
/// Handles message persistence, routing, and business logic
protocol MessagingServiceProtocol {
    /// Connection state from underlying client
    var connectionState: AnyPublisher<BluetoothConnectionState, Never> { get }
    
    /// All messages across all channels and DMs
    var allMessages: [MeshMessage] { get }
    
    /// Publisher for new messages
    var newMessagePublisher: AnyPublisher<MeshMessage, Never> { get }
    
    /// Publisher for discovered devices during scanning
    var discoveredDevicesPublisher: AnyPublisher<[PeripheralDevice], Never> { get }
    
    /// Currently discovered devices
    var discoveredDevices: [PeripheralDevice] { get }
    
    /// Get messages for a specific channel
    func messages(for channel: UInt32) -> [MeshMessage]
    
    /// Get direct messages with a specific node
    func directMessages(with nodeId: UInt32) -> [MeshMessage]
    
    /// Get all available channels
    func channels() -> [MeshChannel]
    
    /// Send a message to a channel
    func sendMessage(text: String, to channel: UInt32) async throws
    
    /// Send a direct message to a node
    func sendDirectMessage(text: String, to nodeId: UInt32) async throws
    
    /// Get recent conversations (channels + DMs)
    func recentConversations() -> [Conversation]
    
    /// Start scanning for devices
    func startScanning()
    
    /// Stop scanning
    func stopScanning()
    
    /// Connect to a specific device
    func connect(to deviceIdentifier: UUID) async throws
    
    /// Disconnect from current device
    func disconnect()
}

/// Represents a conversation (either a channel or DM)
enum Conversation: Identifiable, Equatable, Hashable {
    case channel(MeshChannel, lastMessage: MeshMessage?)
    case directMessage(node: MeshNode, lastMessage: MeshMessage?)
    
    var id: String {
        switch self {
        case .channel(let channel, _):
            return "channel_\(channel.id)"
        case .directMessage(let node, _):
            return "dm_\(node.id)"
        }
    }
    
    var displayName: String {
        switch self {
        case .channel(let channel, _):
            return channel.displayName
        case .directMessage(let node, _):
            return node.displayName
        }
    }
    
    var lastMessage: MeshMessage? {
        switch self {
        case .channel(_, let message):
            return message
        case .directMessage(_, let message):
            return message
        }
    }
}
