//
//  PersistedNode.swift
//  Firefly
//
//  Created by Jake Holland on 2/22/26.
//

import Foundation
import SwiftData

/// SwiftData model for persisting mesh nodes locally
@Model
final class PersistedNode {
    @Attribute(.unique) var nodeId: UInt32
    var shortName: String?
    var longName: String?
    var hardwareModel: String?
    var macAddress: String?
    var lastHeard: Date?
    var latitude: Double?
    var longitude: Double?
    var altitude: Int32?
    var isFriend: Bool
    
    init(nodeId: UInt32, shortName: String? = nil, longName: String? = nil, hardwareModel: String? = nil, macAddress: String? = nil, lastHeard: Date? = nil, latitude: Double? = nil, longitude: Double? = nil, altitude: Int32? = nil, isFriend: Bool = false) {
        self.nodeId = nodeId
        self.shortName = shortName
        self.longName = longName
        self.hardwareModel = hardwareModel
        self.macAddress = macAddress
        self.lastHeard = lastHeard
        self.latitude = latitude
        self.longitude = longitude
        self.altitude = altitude
        self.isFriend = isFriend
    }
    
    convenience init(from node: MeshNode) {
        self.init(
            nodeId: node.id,
            shortName: node.shortName,
            longName: node.longName,
            hardwareModel: node.hardwareModel,
            macAddress: node.macAddress,
            lastHeard: node.lastHeard,
            latitude: node.location?.latitude,
            longitude: node.location?.longitude,
            altitude: node.location?.altitude,
            isFriend: node.isFriend
        )
    }
    
    func toMeshNode() -> MeshNode {
        let location: NodeLocation?
        if let lat = latitude, let lon = longitude {
            // Pass altitude as-is (nil means unknown, not zero)
            location = NodeLocation(latitude: lat, longitude: lon, altitude: altitude)
        } else {
            location = nil
        }
        
        return MeshNode(
            id: nodeId,
            shortName: shortName,
            longName: longName,
            hardwareModel: hardwareModel,
            macAddress: macAddress,
            lastHeard: lastHeard,
            location: location,
            isFriend: isFriend
        )
    }
}
