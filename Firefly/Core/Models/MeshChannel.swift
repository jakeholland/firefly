//
//  MeshChannel.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation

/// Represents a Meshtastic channel
struct MeshChannel: Identifiable, Equatable, Codable, Hashable {
    let id: UInt32 // Channel index
    var name: String
    var role: ChannelRole
    
    enum ChannelRole: String, Codable {
        case primary
        case secondary
        case disabled
    }
    
    var displayName: String {
        name.isEmpty ? "Channel \(id)" : name
    }
}
