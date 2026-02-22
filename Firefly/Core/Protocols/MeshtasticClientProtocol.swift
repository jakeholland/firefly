//
//  MeshtasticClientProtocol.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import Combine

/// High-level protocol for Meshtastic mesh network client
/// Handles protobuf encoding/decoding and provides domain-level API
protocol MeshtasticClientProtocol {
    /// Current connection state (from underlying Bluetooth service)
    var connectionState: AnyPublisher<BluetoothConnectionState, Never> { get }
    
    /// Publisher for received text messages
    var messagesPublisher: AnyPublisher<MeshMessage, Never> { get }
    
    /// Publisher for node information updates
    var nodeUpdatesPublisher: AnyPublisher<MeshNode, Never> { get }
    
    /// Publisher for channel updates
    var channelUpdatesPublisher: AnyPublisher<MeshChannel, Never> { get }
    
    /// All known nodes in the mesh
    var nodes: [MeshNode] { get }
    
    /// All available channels
    var channels: [MeshChannel] { get }
    
    /// The local node's ID (once connected)
    var myNodeId: UInt32? { get }
    
    /// Start connection process
    func connect() async throws
    
    /// Disconnect from mesh
    func disconnect()
    
    /// Send a text message
    /// - Parameters:
    ///   - text: The message text
    ///   - destination: Destination node ID (nil for broadcast)
    ///   - channel: Channel index (default 0)
    func sendMessage(text: String, to destination: UInt32?, channel: UInt32) async throws
    
    /// Request node information
    func requestNodeInfo(for nodeId: UInt32) async throws
    
    /// Get a specific node by ID
    func node(for id: UInt32) -> MeshNode?
}
