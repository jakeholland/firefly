//
//  ConversationView.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import SwiftUI

struct ConversationView: View {
    @StateObject private var viewModel: ConversationViewModel
    @Environment(\.dismiss) private var dismiss
    
    init(conversation: Conversation, container: DependencyContainer = .production()) {
        _viewModel = StateObject(
            wrappedValue: ConversationViewModel(
                conversation: conversation,
                messagingService: container.messagingService
            )
        )
    }
    
    var body: some View {
        ZStack {
            LinearGradient(
                colors: [.black, Color(red: 0.1, green: 0, blue: 0.2)],
                startPoint: .top,
                endPoint: .bottom
            )
            .ignoresSafeArea()
            
            VStack(spacing: 0) {
                ScrollViewReader { proxy in
                    ScrollView {
                        LazyVStack(spacing: 12) {
                            ForEach(viewModel.messages) { message in
                                MessageBubbleView(message: message)
                                    .id(message.id)
                            }
                        }
                        .padding()
                    }
                    .onChange(of: viewModel.messages.count) { _, _ in
                        if let lastMessage = viewModel.messages.last {
                            withAnimation {
                                proxy.scrollTo(lastMessage.id, anchor: .bottom)
                            }
                        }
                    }
                }
                
                messageInputBar
            }
        }
        .navigationTitle(viewModel.title)
        .navigationBarTitleDisplayMode(.inline)
        .preferredColorScheme(.dark)
    }
    
    private var messageInputBar: some View {
        HStack(spacing: 12) {
            TextField("Message", text: $viewModel.messageText, axis: .vertical)
                .textFieldStyle(.plain)
                .padding(12)
                .background(Color.white.opacity(0.1))
                .clipShape(RoundedRectangle(cornerRadius: 20))
                .lineLimit(1...5)
            
            Button {
                viewModel.sendMessage()
            } label: {
                Image(systemName: viewModel.isSending ? "hourglass" : "arrow.up.circle.fill")
                    .font(.title2)
                    .foregroundStyle(.cyan)
            }
            .disabled(viewModel.messageText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || viewModel.isSending)
        }
        .padding()
        .background(Color.black.opacity(0.5))
    }
}

struct MessageBubbleView: View {
    let message: MeshMessage
    
    var body: some View {
        HStack {
            if message.isFromMe {
                Spacer(minLength: 60)
            }
            
            VStack(alignment: message.isFromMe ? .trailing : .leading, spacing: 4) {
                Text(message.text)
                    .padding(12)
                    .background(
                        message.isFromMe
                            ? Color.cyan.opacity(0.8)
                            : Color.white.opacity(0.15)
                    )
                    .foregroundStyle(.primary)
                    .clipShape(RoundedRectangle(cornerRadius: 16))
                
                Text(message.timestamp, style: .time)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
            
            if !message.isFromMe {
                Spacer(minLength: 60)
            }
        }
    }
}

#Preview {
    let channel = MeshChannel(id: 0, name: "Primary", role: .primary)
    let conversation = Conversation.channel(channel, lastMessage: nil)
    
    NavigationStack {
        ConversationView(conversation: conversation)
    }
}
