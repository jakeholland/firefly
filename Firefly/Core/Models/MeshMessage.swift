//
//  MeshMessage.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation

/// Represents a text message sent or received via the mesh network
struct MeshMessage: Identifiable, Equatable, Codable, Hashable {
    let id: UInt32 // Message ID
    let from: UInt32 // Sender node number
    let to: UInt32 // Recipient node number (0xFFFFFFFF for broadcast)
    let channel: UInt32 // Channel index
    let text: String
    let timestamp: Date
    let isFromMe: Bool
    
    var isBroadcast: Bool {
        to == 0xFFFFFFFF
    }
    
    var isDirectMessage: Bool {
        !isBroadcast && channel == 0
    }
}
