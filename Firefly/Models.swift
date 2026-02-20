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
    let users: [FireflyUser]
    let messages: [Message]
}

struct FireflyUser: Identifiable, Hashable {
    let id: String
    let name: String
    let isMe: Bool
}

extension FireflyUser {
    var initials: String {
        return name.split(separator: " ").compactMap { $0.first }.map { String($0).uppercased() }.joined()
    }
}

struct Message: Identifiable, Hashable {
    let id: String
    let date: Date
    let text: String
    let sender: FireflyUser
}
