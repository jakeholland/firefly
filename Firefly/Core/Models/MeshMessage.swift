//
//  MeshMessage.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation

/// The broadcast destination address used by the Meshtastic firmware.
/// Any packet addressed to this value is delivered to all nodes on the mesh.
let meshBroadcastAddress: UInt32 = 0xFFFFFFFF

/// Represents a text message sent or received via the mesh network
struct MeshMessage: Identifiable, Equatable, Codable, Hashable {
    let id: UInt32 // Message ID
    let from: UInt32 // Sender node number
    let to: UInt32 // Recipient node number (meshBroadcastAddress for broadcast)
    let channel: UInt32 // Channel index
    let text: String
    let timestamp: Date
    let isFromMe: Bool

    var isBroadcast: Bool {
        to == meshBroadcastAddress
    }

    var isDirectMessage: Bool {
        !isBroadcast && channel == 0
    }
}
