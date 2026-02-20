//
//  Chat.swift
//  Firefly
//
//  Created by Jake Holland on 2/19/26.
//

import Foundation

struct Chat: Identifiable, Hashable {
    let id: String
    let name: String
    let users: [User]
    let messages: [Message]
}

struct User: Identifiable, Hashable {
    let id: String
    let name: String
    let isMe: Bool
}

struct Message: Identifiable, Hashable {
    let id: String
    let date: Date
    let text: String
    let sender: User
}
