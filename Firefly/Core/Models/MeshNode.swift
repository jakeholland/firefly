//
//  MeshNode.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import CoreLocation

/// Represents a Meshtastic node in the mesh network
struct MeshNode: Identifiable, Equatable, Codable, Hashable {
    let id: UInt32 // Node number
    var shortName: String?
    var longName: String?
    var hardwareModel: String?
    var macAddress: String?
    var lastHeard: Date?
    var location: NodeLocation?
    var isFriend: Bool = false
    
    var displayName: String {
        longName ?? shortName ?? "Node \(String(format: "%08X", id))"
    }
}
