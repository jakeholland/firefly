//
//  ConversationViewModel.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import Combine

@MainActor
final class ConversationViewModel: ObservableObject {
    @Published var messages: [MeshMessage] = []
    @Published var messageText: String = ""
    @Published var isSending: Bool = false
    @Published var errorMessage: String?
    
    let conversation: Conversation
    private let messagingService: MessagingServiceProtocol
    private var cancellables = Set<AnyCancellable>()
    
    var title: String {
        conversation.displayName
    }
    
    init(conversation: Conversation, messagingService: MessagingServiceProtocol) {
        self.conversation = conversation
        self.messagingService = messagingService
        
        messagingService.newMessagePublisher
            .receive(on: RunLoop.main)
            .sink { [weak self] _ in
                self?.refreshMessages()
            }
            .store(in: &cancellables)
        
        refreshMessages()
    }
    
    func sendMessage() {
        guard !messageText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
        
        let text = messageText
        messageText = ""
        isSending = true
        errorMessage = nil
        
        Task {
            do {
                switch conversation {
                case .channel(let channel, _):
                    try await messagingService.sendMessage(text: text, to: channel.id)
                case .directMessage(let node, _):
                    try await messagingService.sendDirectMessage(text: text, to: node.id)
                }
                isSending = false
                refreshMessages()
            } catch {
                isSending = false
                errorMessage = error.localizedDescription
            }
        }
    }
    
    func refreshMessages() {
        switch conversation {
        case .channel(let channel, _):
            messages = messagingService.messages(for: channel.id)
        case .directMessage(let node, _):
            messages = messagingService.directMessages(with: node.id)
        }
    }
}
