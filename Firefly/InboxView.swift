//
//  ContentView.swift
//  Firefly
//
//  Created by Jake Holland on 2/19/26.
//

import SwiftUI

struct InboxView: View {
    let chats: [Chat] = MockData.allChats
    
    var body: some View {
        NavigationStack {
            List(chats) { chat in
                NavigationLink(destination: ChatView(chat: chat)) {
                    HStack(alignment: .top) {
                        VStack(alignment: .leading) {
                            Text(chat.name)
                            Text(chat.messages.first?.text ?? "")
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                        Text(chat.messages.first?.date.formatted(date: .abbreviated, time: .shortened) ?? "")
                    }
                }
            }.navigationTitle("Inbox")
        }
        .navigationTitle("Inbox")
        
    }
}

#Preview {
    InboxView()
}
