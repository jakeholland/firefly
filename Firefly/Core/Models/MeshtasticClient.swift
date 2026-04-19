//
//  MeshtasticClient.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import Combine
import SwiftProtobuf

/// High-level Meshtastic client implementation using proper protobuf encoding/decoding
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

    // Keyed by wantConfigID nonce; resumed when matching configCompleteID arrives.
    private var pendingConfigCompletions: [UInt32: CheckedContinuation<Void, Never>] = [:]

    // Magic nonces the Meshtastic firmware uses to distinguish request types:
    //   69420 → device config + channels (no node DB)
    //   69421 → full node database
    private static let nonceConfig: UInt32 = 69420
    private static let nonceNodeDB: UInt32  = 69421

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

    // MARK: - Scanning

    func startScanning() {
        NSLog("📡 [Meshtastic] 📡 Starting device scan")
        bluetoothService.startScanning()
    }

    func stopScanning() {
        NSLog("📡 [Meshtastic] 🛑 Stopping device scan")
        bluetoothService.stopScanning()
    }

    // MARK: - Connection

    func connect(to deviceIdentifier: UUID) async throws {
        NSLog("📡 [Meshtastic] 🔌 Connecting to device: \(deviceIdentifier.uuidString)")
        bluetoothService.connect(to: deviceIdentifier)

        // A one-shot subscription: captured by the closure, released when continuation resumes.
        var connectCancellables = Set<AnyCancellable>()
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            var resumed = false
            bluetoothService.connectionState
                .sink { state in
                    guard !resumed else { return }
                    switch state {
                    case .connected:
                        NSLog("📡 [Meshtastic] ✅ BLE connected, starting config handshake")
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
                .store(in: &connectCancellables)
        }
        connectCancellables.removeAll() // release the one-shot subscription after connect completes

        // 1. Heartbeat — primes the BLE connection (nonce must be >= 2)
        await sendHeartbeat()

        // 2. Config + channels dump (nonce 69420 is the Meshtastic firmware constant
        //    for "give me device config/channels but NOT the node database").
        NSLog("📡 [Meshtastic] 📋 Requesting device config (nonce \(Self.nonceConfig))...")
        await sendWantConfig(nonce: Self.nonceConfig)
        await waitForConfigComplete(nonce: Self.nonceConfig, timeout: 30)
        NSLog("📡 [Meshtastic] ✅ Device config received")

        // 3. Node database dump (nonce 69421 = "give me all NodeInfo entries").
        //    This is what populates the 300+ node list.
        NSLog("📡 [Meshtastic] 📋 Requesting node database (nonce \(Self.nonceNodeDB))...")
        await sendWantConfig(nonce: Self.nonceNodeDB)
        await waitForConfigComplete(nonce: Self.nonceNodeDB, timeout: 60)
        NSLog("📡 [Meshtastic] ✅ Node database received (\(nodeDatabase.count) nodes)")
    }

    func disconnect() {
        NSLog("📡 [Meshtastic] 🔌 Disconnecting from device")
        bluetoothService.disconnect()

        let nodeCount = nodeDatabase.count
        nodeDatabase.removeAll()
        myNodeId = nil

        NSLog("📡 [Meshtastic] 🧹 Cleared \(nodeCount) nodes from database")
    }

    // MARK: - Messaging

    func sendMessage(text: String, to destination: UInt32?, channel: UInt32) async throws {
        let messageId = messageIdCounter
        messageIdCounter += 1

        let destDescription = destination.map { "0x\(String($0, radix: 16))" } ?? "broadcast"
        NSLog("📡 [Meshtastic] 📤 Sending message (ID: \(messageId)) to \(destDescription) on channel \(channel): \"\(text.prefix(50))\"")

        var dataMessage = DataMessage()
        dataMessage.portnum = .textMessageApp
        dataMessage.payload = text.data(using: .utf8) ?? Data()

        var packet = MeshPacket()
        packet.id = messageId
        packet.to = destination ?? meshBroadcastAddress
        packet.channel = channel
        packet.decoded = dataMessage
        packet.wantAck = destination != nil // ACK for DMs, not broadcasts
        if let myId = myNodeId {
            packet.from = myId
        }

        var toRadio = ToRadio()
        toRadio.packet = packet

        do {
            let data = try toRadio.serializedData()
            try await bluetoothService.send(data)
            NSLog("📡 [Meshtastic] ✅ Message sent successfully (\(data.count) bytes)")
        } catch {
            NSLog("📡 [Meshtastic] ❌ Failed to send message: \(error.localizedDescription)")
            throw error
        }
    }

    func requestNodeInfo(for nodeId: UInt32) async throws {
        NSLog("📡 [Meshtastic] 🔍 Requesting node info for: 0x\(String(nodeId, radix: 16))")
        // Node info arrives automatically via the wantConfig flow; nothing extra needed here
    }

    func setOwner(longName: String, shortName: String) async throws {
        guard let myId = myNodeId else {
            NSLog("📡 [Meshtastic] ❌ setOwner called but myNodeId is unknown")
            throw BluetoothError.notConnected
        }

        let trimmedLong  = longName.trimmingCharacters(in: .whitespacesAndNewlines)
        // Preserve existing short name; fall back to last-4-hex of node ID (device default)
        let existingShort = nodeDatabase[myId]?.shortName ?? String(format: "%04x", myId & 0xFFFF)
        let trimmedShort  = shortName.isEmpty ? existingShort : String(shortName.prefix(4))

        NSLog("📡 [Meshtastic] ✏️ Setting owner: longName='\(trimmedLong)' shortName='\(trimmedShort)'")

        var user = User()
        user.id        = "!\(String(format: "%08x", myId))"
        user.longName  = trimmedLong
        user.shortName = trimmedShort

        var adminMessage = AdminMessage()
        adminMessage.setOwner = user

        var dataMessage = DataMessage()
        dataMessage.portnum      = .adminApp
        dataMessage.wantResponse = false
        dataMessage.payload      = try adminMessage.serializedData()

        let packetId = messageIdCounter
        messageIdCounter += 1

        var packet = MeshPacket()
        packet.id       = packetId
        packet.to       = myId
        packet.from     = myId
        packet.decoded  = dataMessage
        packet.wantAck  = false          // admin packets don't need ACK (matches official app)
        packet.priority = .reliable      // matches official Meshtastic iOS app

        var toRadio = ToRadio()
        toRadio.packet = packet

        let data = try toRadio.serializedData()
        try await bluetoothService.send(data)
        NSLog("📡 [Meshtastic] ✅ setOwner sent (\(data.count) bytes)")

        // Optimistically update local node so the profile reflects the change immediately
        let updated = MeshNode(
            id: myId,
            shortName: trimmedShort,
            longName: trimmedLong,
            hardwareModel: nodeDatabase[myId]?.hardwareModel,
            macAddress: nodeDatabase[myId]?.macAddress,
            lastHeard: nodeDatabase[myId]?.lastHeard,
            location: nodeDatabase[myId]?.location,
            isFriend: nodeDatabase[myId]?.isFriend ?? false
        )
        nodeDatabase[myId] = updated
        nodeUpdatesSubject.send(updated)
    }

    func node(for id: UInt32) -> MeshNode? {
        let node = nodeDatabase[id]
        if node == nil {
            NSLog("📡 [Meshtastic] ⚠️ Node not found in database: 0x\(String(id, radix: 16))")
        }
        return node
    }

    // MARK: - Private: Handshake helpers

    private func sendHeartbeat() async {
        var heartbeat = Heartbeat()
        // Must be >= 2; nonce == 1 can trigger special firmware behavior.
        heartbeat.nonce = UInt32.random(in: 2...UInt32.max)
        var toRadio = ToRadio()
        toRadio.heartbeat = heartbeat
        do {
            let data = try toRadio.serializedData()
            try await bluetoothService.send(data)
            NSLog("📡 [Meshtastic] 💓 Sent heartbeat (nonce: \(heartbeat.nonce))")
        } catch {
            NSLog("📡 [Meshtastic] ⚠️ Heartbeat failed (non-fatal): \(error.localizedDescription)")
        }
    }

    private func sendWantConfig(nonce: UInt32) async {
        var toRadio = ToRadio()
        toRadio.wantConfigID = nonce
        do {
            let data = try toRadio.serializedData()
            try await bluetoothService.send(data)
            NSLog("📡 [Meshtastic] 📤 Sent WantConfig (nonce: \(nonce), \(data.count) bytes)")
        } catch {
            NSLog("📡 [Meshtastic] ❌ Failed to send WantConfig (nonce \(nonce)): \(error.localizedDescription)")
            // Resume any waiting continuation so connect() doesn't hang
            pendingConfigCompletions.removeValue(forKey: nonce)?.resume()
        }
    }

    /// Suspends until a `configCompleteID` matching `nonce` arrives, or until `timeout` seconds.
    private func waitForConfigComplete(nonce: UInt32, timeout: TimeInterval) async {
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            pendingConfigCompletions[nonce] = continuation
            // Timeout guard — resume even if the device never sends configCompleteID
            Task { @MainActor [weak self] in
                try? await Task.sleep(nanoseconds: UInt64(timeout * 1_000_000_000))
                guard let self else { return }
                if let cont = self.pendingConfigCompletions.removeValue(forKey: nonce) {
                    NSLog("📡 [Meshtastic] ⏱ waitForConfigComplete timed out (nonce \(nonce))")
                    cont.resume()
                }
            }
        }
    }

    // MARK: - Private: Incoming data

    private func handleReceivedData(_ data: Data) {
        NSLog("📡 [Meshtastic] 📥 Received \(data.count) bytes from device")

        guard let fromRadio = try? FromRadio(serializedBytes: data) else {
            NSLog("📡 [Meshtastic] ⚠️ Failed to parse FromRadio protobuf (\(data.count) bytes)")
            return
        }

        switch fromRadio.payloadVariant {
        case .myInfo(let myInfo):
            myNodeId = myInfo.myNodeNum
            NSLog("📡 [Meshtastic] ✅ Got MyNodeInfo: nodeId=0x\(String(myInfo.myNodeNum, radix: 16))")

        case .nodeInfo(let nodeInfo):
            handleNodeInfo(nodeInfo)

        case .packet(let meshPacket):
            handleMeshPacket(meshPacket)

        case .configCompleteID(let id):
            NSLog("📡 [Meshtastic] ✅ Config complete (ID: \(id)) — resuming handshake")
            pendingConfigCompletions.removeValue(forKey: id)?.resume()

        case .channel(let channel):
            handleChannelUpdate(channel)

        case .logRecord(let log):
            NSLog("📡 [Meshtastic] 📋 Device log [\(log.source)]: \(log.message)")

        case .config:
            NSLog("📡 [Meshtastic] ⚙️ Received device config")

        case .moduleConfig:
            NSLog("📡 [Meshtastic] ⚙️ Received module config")

        case .none:
            NSLog("📡 [Meshtastic] ℹ️ Empty FromRadio payload")

        default:
            NSLog("📡 [Meshtastic] ℹ️ Unhandled FromRadio payload type")
        }
    }

    // MARK: - Private: Packet handlers

    private func handleMeshPacket(_ packet: MeshPacket) {
        NSLog("📡 [Meshtastic] 📦 MeshPacket: from=0x\(String(packet.from, radix: 16)), to=0x\(String(packet.to, radix: 16)), id=\(packet.id)")

        guard case .decoded(let dataMessage) = packet.payloadVariant else {
            NSLog("📡 [Meshtastic] ℹ️ Encrypted or undecodable packet, skipping")
            return
        }

        switch dataMessage.portnum {
        case .textMessageApp:
            guard let text = String(data: dataMessage.payload, encoding: .utf8), !text.isEmpty else {
                NSLog("📡 [Meshtastic] ⚠️ Failed to decode text message payload")
                return
            }
            NSLog("📡 [Meshtastic] 💬 Text message: \"\(text.prefix(50))\"")
            let message = MeshMessage(
                id: packet.id,
                from: packet.from,
                to: packet.to,
                channel: packet.channel,
                text: text,
                timestamp: packet.rxTime > 0 ? Date(timeIntervalSince1970: TimeInterval(packet.rxTime)) : Date(),
                isFromMe: packet.from == myNodeId
            )
            messagesSubject.send(message)
            NSLog("📡 [Meshtastic] ✅ Message published to subscribers")

        case .positionApp:
            if let position = try? Position(serializedBytes: dataMessage.payload) {
                updateNodePosition(nodeId: packet.from, position: position)
            }

        case .nodeinfoApp:
            if let user = try? User(serializedBytes: dataMessage.payload) {
                handleUserInfo(nodeId: packet.from, user: user)
            }

        default:
            NSLog("📡 [Meshtastic] ℹ️ Non-text packet (portnum: \(dataMessage.portnum)), skipping")
        }
    }

    private func handleNodeInfo(_ nodeInfo: NodeInfo) {
        var node = nodeDatabase[nodeInfo.num] ?? MeshNode(id: nodeInfo.num)

        if nodeInfo.hasUser {
            node.longName = nodeInfo.user.longName.isEmpty ? nil : nodeInfo.user.longName
            node.shortName = nodeInfo.user.shortName.isEmpty ? nil : nodeInfo.user.shortName
            node.hardwareModel = "\(nodeInfo.user.hwModel)"
            if !nodeInfo.user.macaddr.isEmpty {
                node.macAddress = nodeInfo.user.macaddr.map { String(format: "%02X", $0) }.joined(separator: ":")
            }
        }

        if nodeInfo.hasPosition {
            let lat = Double(nodeInfo.position.latitudeI) * 1e-7
            let lon = Double(nodeInfo.position.longitudeI) * 1e-7
            if lat != 0 || lon != 0 {
                node.location = NodeLocation(
                    latitude: lat,
                    longitude: lon,
                    altitude: nodeInfo.position.hasAltitude ? nodeInfo.position.altitude : nil,
                    timestamp: Date()
                )
            }
        }

        node.lastHeard = nodeInfo.lastHeard > 0
            ? Date(timeIntervalSince1970: TimeInterval(nodeInfo.lastHeard))
            : Date()
        node.isFriend = nodeInfo.isFavorite

        nodeDatabase[node.id] = node
        nodeUpdatesSubject.send(node)
        NSLog("📡 [Meshtastic] 👤 NodeInfo: id=0x\(String(node.id, radix: 16)), name=\(node.displayName)")
    }

    private func updateNodePosition(nodeId: UInt32, position: Position) {
        let lat = Double(position.latitudeI) * 1e-7
        let lon = Double(position.longitudeI) * 1e-7
        guard lat != 0 || lon != 0 else { return }

        var node = nodeDatabase[nodeId] ?? MeshNode(id: nodeId)
        node.location = NodeLocation(
            latitude: lat,
            longitude: lon,
            altitude: position.hasAltitude ? position.altitude : nil,
            timestamp: Date()
        )
        node.lastHeard = Date()
        nodeDatabase[nodeId] = node
        nodeUpdatesSubject.send(node)
        NSLog("📡 [Meshtastic] 📍 Position update for 0x\(String(nodeId, radix: 16)): \(lat), \(lon)")
    }

    private func handleUserInfo(nodeId: UInt32, user: User) {
        var node = nodeDatabase[nodeId] ?? MeshNode(id: nodeId)
        node.longName = user.longName.isEmpty ? nil : user.longName
        node.shortName = user.shortName.isEmpty ? nil : user.shortName
        node.hardwareModel = "\(user.hwModel)"
        node.lastHeard = Date()
        nodeDatabase[nodeId] = node
        nodeUpdatesSubject.send(node)
        NSLog("📡 [Meshtastic] 👤 User update for 0x\(String(nodeId, radix: 16)): \(node.displayName)")
    }

    private func handleChannelUpdate(_ channel: Channel) {
        let channelId = UInt32(max(0, channel.index))
        let name: String
        if channel.hasSettings && !channel.settings.name.isEmpty {
            name = channel.settings.name
        } else if channelId == 0 {
            name = "Primary"
        } else {
            name = "Channel \(channelId)"
        }

        let role: MeshChannel.ChannelRole
        switch channel.role {
        case .primary:   role = .primary
        case .secondary: role = .secondary
        default:         role = .disabled
        }

        let meshChannel = MeshChannel(id: channelId, name: name, role: role)
        channelDatabase[channelId] = meshChannel
        channelUpdatesSubject.send(meshChannel)
        NSLog("📡 [Meshtastic] 📻 Channel update: id=\(channelId), name=\(name), role=\(role)")
    }
}
