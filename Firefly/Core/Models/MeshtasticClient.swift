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
        
        bluetoothService.receivedDataPublisher
            .sink { [weak self] data in self?.handleReceivedData(data) }
            .store(in: &cancellables)
        
        let primaryChannel = MeshChannel(id: 0, name: "Primary", role: .primary)
        channelDatabase[0] = primaryChannel
        channelUpdatesSubject.send(primaryChannel)
    }
    
    func connect() async throws {
        bluetoothService.startScanning()
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            var resumed = false
            bluetoothService.connectionState
                .sink { state in
                    guard !resumed else { return }
                    switch state {
                    case .connected:
                        resumed = true
                        continuation.resume()
                    case .failed(let error):
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
        bluetoothService.disconnect()
        nodeDatabase.removeAll()
        myNodeId = nil
    }
    
    func sendMessage(text: String, to destination: UInt32?, channel: UInt32) async throws {
        if myNodeId == nil { myNodeId = 0x12345678 }
        let messageId = messageIdCounter
        messageIdCounter += 1
        let packet = createTextMessagePacket(id: messageId, from: myNodeId!, to: destination ?? 0xFFFFFFFF, channel: channel, text: text)
        try await bluetoothService.send(packet)
    }
    
    func requestNodeInfo(for nodeId: UInt32) async throws { }
    func node(for id: UInt32) -> MeshNode? { nodeDatabase[id] }
    
    private func handleReceivedData(_ data: Data) {
        guard data.count >= 20 else { return }
        let messageId = data.withUnsafeBytes { $0.load(fromByteOffset: 0, as: UInt32.self) }
        let from = data.withUnsafeBytes { $0.load(fromByteOffset: 4, as: UInt32.self) }
        let to = data.withUnsafeBytes { $0.load(fromByteOffset: 8, as: UInt32.self) }
        let channel = data.withUnsafeBytes { $0.load(fromByteOffset: 12, as: UInt32.self) }
        let portNum = data.withUnsafeBytes { $0.load(fromByteOffset: 16, as: UInt32.self) }
        
        if portNum == 1, let text = String(data: data.dropFirst(20), encoding: .utf8) {
            let message = MeshMessage(id: messageId, from: from, to: to, channel: channel, text: text, timestamp: Date(), isFromMe: from == myNodeId)
            messagesSubject.send(message)
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
        return data
    }
}
