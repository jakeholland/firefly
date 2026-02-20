//
//  Mocks.swift
//  Firefly
//
//  Created by Jake Holland on 2/19/26.
//

import Foundation
import SwiftUI


extension FireflyUser {
    static func mock(id: String = UUID().uuidString, name: String = "Test User", isMe: Bool = false) -> FireflyUser {
        .init(id: id, name: name, isMe: isMe)
    }
}

extension Message {
    static func mock(
        id: String = UUID().uuidString,
        date: Date = .now,
        text: String = "Test message",
        sender: FireflyUser
    ) -> Message {
        .init(id: id, date: date, text: text, sender: sender)
    }
}
extension Chat {
    static func mock(
        id: String = UUID().uuidString,
        name: String = "Test Chat",
        users: [FireflyUser],
        messages: [Message]
    ) -> Chat {
        .init(id: id, name: name, users: users, messages: messages)
    }
}

enum MockData {
    static let me = FireflyUser.mock(id: "u-me", name: "Jake Holland", isMe: true)
    static let alice = FireflyUser.mock(id: "u-alice", name: "Alice Turtle")
    static let bob = FireflyUser.mock(id: "u-bob", name: "Bob Lake")
    static let carol = FireflyUser.mock(id: "u-carol", name: "Carol Baskin")

    // Messages for Chat 1 (Group chat)
    static let messagesChat1: [Message] = {
        let base = Date()
        return [
            Message.mock(id: "m-1", date: base.addingTimeInterval(-60 * 60 * 6), text: "Hey crew, doors at 7!", sender: alice),
            Message.mock(id: "m-2", date: base.addingTimeInterval(-60 * 60 * 5.5), text: "Rolling up around 6:30.", sender: me),
            Message.mock(id: "m-3", date: base.addingTimeInterval(-60 * 60 * 5), text: "Meet at the main gate?", sender: bob),
            Message.mock(id: "m-4", date: base.addingTimeInterval(-60 * 60 * 4.5), text: "Yep!", sender: carol),
            Message.mock(id: "m-5", date: base.addingTimeInterval(-60 * 60 * 1), text: "Where are you? I tried to find you but I could not. What set are you going to next? I want to RAGE!!! 😈", sender: alice),
            Message.mock(id: "m-6", date: base.addingTimeInterval(-60 * 60 * 0.8), text: "BassPod right side!", sender: me)
        ]
    }()

    // Messages for Chat 2 (One-on-one)
    static let messagesChat2: [Message] = {
        let base = Date()
        return [
            Message.mock(id: "m-7", date: base.addingTimeInterval(-60 * 60 * 24 * 2), text: "Did you get the tickets?", sender: me),
            Message.mock(id: "m-8", date: base.addingTimeInterval(-60 * 60 * 24 * 2 - 1200), text: "Yep, forwarded to your email.", sender: bob),
            Message.mock(id: "m-9", date: base.addingTimeInterval(-60 * 60 * 24), text: "Legend!", sender: me)
        ]
    }()

    // Messages for Chat 3 (Small group)
    static let messagesChat3: [Message] = {
        let base = Date()
        return [
            Message.mock(id: "m-10", date: base.addingTimeInterval(-60 * 30), text: "Food run?", sender: carol),
            Message.mock(id: "m-11", date: base.addingTimeInterval(-60 * 25), text: "Tacos sound amazing.", sender: alice),
            Message.mock(id: "m-12", date: base.addingTimeInterval(-60 * 20), text: "I'm in.", sender: me)
        ]
    }()

    static let chat1 = Chat.mock(
        id: "c-1",
        name: "EDC 2026 Crew",
        users: [me, alice, bob, carol],
        messages: messagesChat1
    )

    static let chat2 = Chat.mock(
        id: "c-2",
        name: "Bob",
        users: [me, bob],
        messages: messagesChat2
    )

    static let chat3 = Chat.mock(
        id: "c-3",
        name: "Dinner Plan",
        users: [me, alice, carol],
        messages: messagesChat3
    )

    static let allChats: [Chat] = [chat1, chat2, chat3]
    static let allUsers: [FireflyUser] = [me, alice, bob, carol]
}

