//
//  PersistedMessage.swift
//  Firefly
//
//  Created by Jake Holland on 2/22/26.
//

import Foundation
import SwiftData

/// SwiftData model for persisting mesh messages locally
@Model
final class PersistedMessage {
    @Attribute(.unique) var messageId: UInt32
    var from: UInt32
    var to: UInt32
    var channel: UInt32
    var text: String
    var timestamp: Date
    var isFromMe: Bool
    
    init(messageId: UInt32, from: UInt32, to: UInt32, channel: UInt32, text: String, timestamp: Date, isFromMe: Bool) {
        self.messageId = messageId
        self.from = from
        self.to = to
        self.channel = channel
        self.text = text
        self.timestamp = timestamp
        self.isFromMe = isFromMe
    }
    
    convenience init(from message: MeshMessage) {
        self.init(
            messageId: message.id,
            from: message.from,
            to: message.to,
            channel: message.channel,
            text: message.text,
            timestamp: message.timestamp,
            isFromMe: message.isFromMe
        )
    }
    
    func toMeshMessage() -> MeshMessage {
        MeshMessage(
            id: messageId,
            from: from,
            to: to,
            channel: channel,
            text: text,
            timestamp: timestamp,
            isFromMe: isFromMe
        )
    }
}
