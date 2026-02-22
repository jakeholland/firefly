//
//  InboxViewModel.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import Combine

@MainActor
final class InboxViewModel: ObservableObject {
    @Published var conversations: [Conversation] = []
    @Published var connectionState: BluetoothConnectionState = .disconnected
    @Published var isConnecting: Bool = false
    @Published var errorMessage: String?
    
    private let messagingService: MessagingServiceProtocol
    private var cancellables = Set<AnyCancellable>()
    
    var isConnected: Bool {
        if case .connected = connectionState { return true }
        return false
    }
    
    var connectionStatusText: String {
        switch connectionState {
        case .disconnected: return "Disconnected"
        case .scanning: return "Scanning..."
        case .connecting: return "Connecting..."
        case .connected: return "Connected"
        case .failed(let error): return "Error: \(error.localizedDescription)"
        }
    }
    
    init(messagingService: MessagingServiceProtocol) {
        self.messagingService = messagingService
        
        messagingService.connectionState
            .receive(on: DispatchQueue.main)
            .sink { [weak self] state in
                self?.connectionState = state
                self?.isConnecting = false
            }
            .store(in: &cancellables)
        
        messagingService.newMessagePublisher
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in
                self?.refreshConversations()
            }
            .store(in: &cancellables)
        
        refreshConversations()
    }
    
    func connect() {
        isConnecting = true
        errorMessage = nil
    }
    
    func disconnect() { }
    
    func refreshConversations() {
        conversations = messagingService.recentConversations()
    }
}
