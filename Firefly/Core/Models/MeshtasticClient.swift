//
//  MeshtasticClient.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import Combine

/// High-level Meshtastic client implementation
final class MeshtasticClient: MeshtasticClientProtocol {
    private let bluetoothService: BluetoothServiceProtocol
    
    var connectionState: AnyPublisher<BluetoothConnectionState, Never> {
        bluetoothService.connectionState.eraseToAnyPublisher()
    }
    
    var discoveredDevicesPublisher: AnyPublisher<[PeripheralDevice], Never> {
        bluetoothService.discoveredDevicesPublisher
    }
    
    var discoveredDevices: [PeripheralDevice] {
        bluetoothService.discoveredDevices
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
    
    private var nodeDatabase: [UInt32: MeshNode] = [:]
    private var channelDatabase: [UInt32: MeshChannel] = [:]
    private(set) var myNodeId: UInt32?
    private var cancellables = Set<AnyCancellable>()
    private var messageIdCounter: UInt32 = 1
    
    var nodes: [MeshNode] { Array(nodeDatabase.values) }
    var channels: [MeshChannel] { Array(channelDatabase.values) }
    
    init(bluetoothService: BluetoothServiceProtocol) {
        self.bluetoothService = bluetoothService
        
        NSLog("📡 [Meshtastic] Initializing MeshtasticClient")
        
        bluetoothService.receivedDataPublisher
            .sink { [weak self] data in self?.handleReceivedData(data) }
            .store(in: &cancellables)
        
        NSLog("📡 [Meshtastic] ✅ Setting up Primary channel (ID: 0)")
        let primaryChannel = MeshChannel(id: 0, name: "Primary", role: .primary)
        channelDatabase[0] = primaryChannel
        channelUpdatesSubject.send(primaryChannel)
    }
    
    func startScanning() {
        NSLog("📡 [Meshtastic] 📡 Starting device scan")
        bluetoothService.startScanning()
    }
    
    func stopScanning() {
        NSLog("📡 [Meshtastic] 🛑 Stopping device scan")
        bluetoothService.stopScanning()
    }
    
    func connect(to deviceIdentifier: UUID) async throws {
        NSLog("📡 [Meshtastic] 🔌 Connecting to device: \(deviceIdentifier.uuidString)")
        bluetoothService.connect(to: deviceIdentifier)
        
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            var resumed = false
            bluetoothService.connectionState
                .sink { state in
                    guard !resumed else { return }
                    switch state {
                    case .connected:
                        NSLog("📡 [Meshtastic] ✅ Successfully connected to device")
                        resumed = true
                        continuation.resume()
                    case .failed(let error):
                        NSLog("📡 [Meshtastic] ❌ Connection failed: \(error.localizedDescription)")
                        resumed = true
                        continuation.resume(throwing: error)
                    default:
                        break
                    }
                }
                .store(in: &cancellables)
        }
    }
    
    func disconnect() {
        NSLog("📡 [Meshtastic] 🔌 Disconnecting from device")
        bluetoothService.disconnect()
        
        let nodeCount = nodeDatabase.count
        nodeDatabase.removeAll()
        myNodeId = nil
        
        NSLog("📡 [Meshtastic] 🧹 Cleared \(nodeCount) nodes from database")
    }
    
    func sendMessage(text: String, to destination: UInt32?, channel: UInt32) async throws {
        if myNodeId == nil {
            myNodeId = 0x12345678
            NSLog("📡 [Meshtastic] ⚠️ No node ID set, using default: 0x\(String(myNodeId!, radix: 16))")
        }
        
        let messageId = messageIdCounter
        messageIdCounter += 1
        
        let destDescription = destination.map { "0x\(String($0, radix: 16))" } ?? "broadcast"
        NSLog("📡 [Meshtastic] 📤 Sending message (ID: \(messageId)) to \(destDescription) on channel \(channel)")
        NSLog("📡 [Meshtastic]    Text: \"\(text.prefix(50))\(text.count > 50 ? "..." : "")\"")
        
        let packet = createTextMessagePacket(id: messageId, from: myNodeId!, to: destination ?? 0xFFFFFFFF, channel: channel, text: text)
        
        do {
            try await bluetoothService.send(packet)
            NSLog("📡 [Meshtastic] ✅ Message sent successfully")
        } catch {
            NSLog("📡 [Meshtastic] ❌ Failed to send message: \(error.localizedDescription)")
            throw error
        }
    }
    
    func requestNodeInfo(for nodeId: UInt32) async throws {
        NSLog("📡 [Meshtastic] 🔍 Requesting node info for: 0x\(String(nodeId, radix: 16))")
    }
    
    func node(for id: UInt32) -> MeshNode? {
        let node = nodeDatabase[id]
        if node == nil {
            NSLog("📡 [Meshtastic] ⚠️ Node not found in database: 0x\(String(id, radix: 16))")
        }
        return node
    }
    
    private func handleReceivedData(_ data: Data) {
        NSLog("📡 [Meshtastic] 📥 Received \(data.count) bytes from device")
        
        guard data.count >= 20 else {
            NSLog("📡 [Meshtastic] ⚠️ Packet too small (expected ≥20 bytes, got \(data.count))")
            return
        }
        
        let messageId = data.withUnsafeBytes { $0.load(fromByteOffset: 0, as: UInt32.self) }
        let from = data.withUnsafeBytes { $0.load(fromByteOffset: 4, as: UInt32.self) }
        let to = data.withUnsafeBytes { $0.load(fromByteOffset: 8, as: UInt32.self) }
        let channel = data.withUnsafeBytes { $0.load(fromByteOffset: 12, as: UInt32.self) }
        let portNum = data.withUnsafeBytes { $0.load(fromByteOffset: 16, as: UInt32.self) }
        
        NSLog("📡 [Meshtastic] 📦 Packet details: ID=\(messageId), from=0x\(String(from, radix: 16)), to=0x\(String(to, radix: 16)), channel=\(channel), port=\(portNum)")
        
        if portNum == 1 {
            if let text = String(data: data.dropFirst(20), encoding: .utf8) {
                NSLog("📡 [Meshtastic] 💬 Text message: \"\(text.prefix(50))\(text.count > 50 ? "..." : "")\"")
                let message = MeshMessage(id: messageId, from: from, to: to, channel: channel, text: text, timestamp: Date(), isFromMe: from == myNodeId)
                messagesSubject.send(message)
                NSLog("📡 [Meshtastic] ✅ Message published to subscribers")
            } else {
                NSLog("📡 [Meshtastic] ⚠️ Failed to decode text message payload")
            }
        } else {
            NSLog("📡 [Meshtastic] ℹ️ Non-text message (port \(portNum)), skipping")
        }
    }
    
    private func createTextMessagePacket(id: UInt32, from: UInt32, to: UInt32, channel: UInt32, text: String) -> Data {
        var data = Data()
        data.append(contentsOf: withUnsafeBytes(of: id) { Data($0) })
        data.append(contentsOf: withUnsafeBytes(of: from) { Data($0) })
        data.append(contentsOf: withUnsafeBytes(of: to) { Data($0) })
        data.append(contentsOf: withUnsafeBytes(of: channel) { Data($0) })
        data.append(contentsOf: withUnsafeBytes(of: UInt32(1)) { Data($0) })
        data.append(text.data(using: .utf8) ?? Data())
        
        NSLog("📡 [Meshtastic] 📦 Created packet: \(data.count) bytes")
        return data
    }
}
