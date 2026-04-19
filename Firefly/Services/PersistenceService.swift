//
//  PersistenceService.swift
//  Firefly
//
//  Created by Jake Holland on 2/22/26.
//

import Foundation
import SwiftData

/// Service for persisting messages and nodes using SwiftData.
/// The class itself is not actor-isolated so the singleton can be created from any context.
/// All methods are @MainActor because ModelContext is not thread-safe.
final class PersistenceService {
    let modelContainer: ModelContainer

    /// mainContext is @MainActor, so access it via the computed property only from @MainActor code.
    @MainActor
    var modelContext: ModelContext { modelContainer.mainContext }

    static let shared: PersistenceService = {
        let schema = Schema([
            PersistedMessage.self,
            PersistedNode.self
        ])
        let modelConfiguration = ModelConfiguration(schema: schema, isStoredInMemoryOnly: false)
        do {
            let container = try ModelContainer(for: schema, configurations: [modelConfiguration])
            return PersistenceService(modelContainer: container)
        } catch {
            fatalError("Could not create ModelContainer: \(error)")
        }
    }()

    init(modelContainer: ModelContainer) {
        self.modelContainer = modelContainer
        NSLog("💾 [Persistence] Initialized PersistenceService")
    }

    // MARK: - Message Operations

    @MainActor
    func saveMessage(_ message: MeshMessage) {
        let persistedMessage = PersistedMessage(from: message)
        modelContext.insert(persistedMessage)
        do {
            try modelContext.save()
            NSLog("💾 [Persistence] ✅ Saved message (ID: \(message.id))")
        } catch {
            NSLog("💾 [Persistence] ❌ Failed to save message: \(error.localizedDescription)")
        }
    }

    @MainActor
    func fetchMessages(for channel: UInt32) -> [MeshMessage] {
        // #Predicate requires a locally-captured constant — cannot use a module-level let.
        let broadcastAddr: UInt32 = meshBroadcastAddress
        let descriptor = FetchDescriptor<PersistedMessage>(
            predicate: #Predicate<PersistedMessage> { message in
                message.channel == channel && message.to == broadcastAddr
            },
            sortBy: [SortDescriptor(\.timestamp, order: .forward)]
        )
        do {
            let messages = try modelContext.fetch(descriptor)
            NSLog("💾 [Persistence] 📋 Fetched \(messages.count) messages for channel \(channel)")
            return messages.map { $0.toMeshMessage() }
        } catch {
            NSLog("💾 [Persistence] ❌ Failed to fetch messages: \(error.localizedDescription)")
            return []
        }
    }

    @MainActor
    func fetchDirectMessages(with nodeId: UInt32, myNodeId: UInt32?) -> [MeshMessage] {
        guard let myNodeId = myNodeId else { return [] }
        // #Predicate requires locally-captured constants.
        let broadcastAddr: UInt32 = meshBroadcastAddress
        let channelZero: UInt32 = 0
        let otherNodeId = nodeId
        let ownNodeId = myNodeId
        let descriptor = FetchDescriptor<PersistedMessage>(
            predicate: #Predicate<PersistedMessage> { message in
                message.to != broadcastAddr &&
                message.channel == channelZero &&
                ((message.from == otherNodeId && message.to == ownNodeId) ||
                 (message.from == ownNodeId && message.to == otherNodeId))
            },
            sortBy: [SortDescriptor(\.timestamp, order: .forward)]
        )
        do {
            let messages = try modelContext.fetch(descriptor)
            NSLog("💾 [Persistence] 📋 Fetched \(messages.count) direct messages with node 0x\(String(nodeId, radix: 16))")
            return messages.map { $0.toMeshMessage() }
        } catch {
            NSLog("💾 [Persistence] ❌ Failed to fetch direct messages: \(error.localizedDescription)")
            return []
        }
    }

    @MainActor
    func fetchAllMessages() -> [MeshMessage] {
        let descriptor = FetchDescriptor<PersistedMessage>(
            sortBy: [SortDescriptor(\.timestamp, order: .reverse)]
        )
        do {
            let messages = try modelContext.fetch(descriptor)
            NSLog("💾 [Persistence] 📋 Fetched \(messages.count) total messages")
            return messages.map { $0.toMeshMessage() }
        } catch {
            NSLog("💾 [Persistence] ❌ Failed to fetch all messages: \(error.localizedDescription)")
            return []
        }
    }

    @MainActor
    func deleteDirectMessages(with nodeId: UInt32, myNodeId: UInt32?) {
        guard let myNodeId else { return }
        // #Predicate requires locally-captured constants.
        let broadcastAddr: UInt32 = meshBroadcastAddress
        let channelZero: UInt32 = 0
        let otherNodeId = nodeId
        let ownNodeId = myNodeId
        let descriptor = FetchDescriptor<PersistedMessage>(
            predicate: #Predicate<PersistedMessage> { message in
                message.to != broadcastAddr &&
                message.channel == channelZero &&
                ((message.from == otherNodeId && message.to == ownNodeId) ||
                 (message.from == ownNodeId && message.to == otherNodeId))
            }
        )
        do {
            let messages = try modelContext.fetch(descriptor)
            messages.forEach { modelContext.delete($0) }
            try modelContext.save()
            NSLog("💾 [Persistence] 🗑 Deleted \(messages.count) DM messages with node 0x\(String(nodeId, radix: 16))")
        } catch {
            NSLog("💾 [Persistence] ❌ Failed to delete DM messages: \(error.localizedDescription)")
        }
    }

    // MARK: - Node Operations

    @MainActor
    func saveNode(_ node: MeshNode) {
        let nodeIdToFind = node.id
        let descriptor = FetchDescriptor<PersistedNode>(
            predicate: #Predicate<PersistedNode> { persistedNode in
                persistedNode.nodeId == nodeIdToFind
            }
        )
        do {
            let existing = try modelContext.fetch(descriptor)
            if let existingNode = existing.first {
                existingNode.shortName = node.shortName
                existingNode.longName = node.longName
                existingNode.hardwareModel = node.hardwareModel
                existingNode.macAddress = node.macAddress
                existingNode.lastHeard = node.lastHeard
                existingNode.latitude = node.location?.latitude
                existingNode.longitude = node.location?.longitude
                existingNode.altitude = node.location?.altitude
                existingNode.isFriend = node.isFriend
                NSLog("💾 [Persistence] ✅ Updated node 0x\(String(node.id, radix: 16))")
            } else {
                let persistedNode = PersistedNode(from: node)
                modelContext.insert(persistedNode)
                NSLog("💾 [Persistence] ✅ Saved new node 0x\(String(node.id, radix: 16))")
            }
            try modelContext.save()
        } catch {
            NSLog("💾 [Persistence] ❌ Failed to save node: \(error.localizedDescription)")
        }
    }

    @MainActor
    func fetchNode(by nodeId: UInt32) -> MeshNode? {
        let nodeIdToFind = nodeId
        let descriptor = FetchDescriptor<PersistedNode>(
            predicate: #Predicate<PersistedNode> { persistedNode in
                persistedNode.nodeId == nodeIdToFind
            }
        )
        do {
            let nodes = try modelContext.fetch(descriptor)
            return nodes.first?.toMeshNode()
        } catch {
            NSLog("💾 [Persistence] ❌ Failed to fetch node: \(error.localizedDescription)")
            return nil
        }
    }

    @MainActor
    func fetchAllNodes() -> [MeshNode] {
        let descriptor = FetchDescriptor<PersistedNode>(
            sortBy: [SortDescriptor(\.lastHeard, order: .reverse)]
        )
        do {
            let nodes = try modelContext.fetch(descriptor)
            NSLog("💾 [Persistence] 📋 Fetched \(nodes.count) nodes")
            return nodes.map { $0.toMeshNode() }
        } catch {
            NSLog("💾 [Persistence] ❌ Failed to fetch all nodes: \(error.localizedDescription)")
            return []
        }
    }

    @MainActor
    func fetchConversationNodes(myNodeId: UInt32?) -> [MeshNode] {
        guard let myNodeId = myNodeId else { return [] }
        // #Predicate requires locally-captured constants.
        let broadcastAddr: UInt32 = meshBroadcastAddress
        let channelZero: UInt32 = 0
        let descriptor = FetchDescriptor<PersistedMessage>(
            predicate: #Predicate<PersistedMessage> { message in
                message.to != broadcastAddr && message.channel == channelZero
            }
        )
        do {
            let messages = try modelContext.fetch(descriptor)
            let nodeIds = Set(messages.flatMap { [$0.from, $0.to] }.filter { $0 != myNodeId })
            var nodes: [MeshNode] = []
            for nodeId in nodeIds {
                if let node = fetchNode(by: nodeId) {
                    nodes.append(node)
                }
            }
            NSLog("💾 [Persistence] 📋 Found \(nodes.count) conversation partners")
            return nodes
        } catch {
            NSLog("💾 [Persistence] ❌ Failed to fetch conversation nodes: \(error.localizedDescription)")
            return []
        }
    }
}
