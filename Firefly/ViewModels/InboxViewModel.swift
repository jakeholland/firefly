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
    @Published var discoveredDevices: [PeripheralDevice] = []
    @Published var showingDeviceSelection: Bool = false

    let messagingService: MessagingServiceProtocol
    private var cancellables = Set<AnyCancellable>()
    /// Prevents concurrent auto-reconnect attempts
    private var isAutoReconnecting = false
    
    var myNodeId: UInt32? {
        messagingService.myNodeId
    }
    
    var isConnected: Bool {
        if case .connected = connectionState { return true }
        return false
    }
    
    var isScanning: Bool {
        if case .scanning = connectionState { return true }
        return false
    }
    
    var connectionStatusText: String {
        switch connectionState {
        case .disconnected: return "Disconnected"
        case .scanning: return "Scanning..."
        case .connecting: return "Connecting..."
        case .connected: return "Connected"
        case .failed(let error): return "Error: \(error.localizedDescription)"
        case .resetting: return "Resetting..."
        case .unknown: return "Unknown"
        }
    }
    
    init(messagingService: MessagingServiceProtocol) {
        self.messagingService = messagingService
        
        messagingService.connectionState
            .receive(on: RunLoop.main)
            .sink { [weak self] state in
                guard let self else { return }
                self.connectionState = state
                self.isConnecting = false

                // Auto-reconnect: when we see .disconnected and have a saved device,
                // attempt to reconnect (handles both startup and unexpected drops).
                if case .disconnected = state {
                    self.attemptAutoReconnect()
                }
            }
            .store(in: &cancellables)
        
        messagingService.newMessagePublisher
            .receive(on: RunLoop.main)
            .sink { [weak self] _ in
                self?.refreshConversations()
            }
            .store(in: &cancellables)
        
        messagingService.discoveredDevicesPublisher
            .receive(on: RunLoop.main)
            .sink { [weak self] devices in
                self?.discoveredDevices = devices
            }
            .store(in: &cancellables)
        
        refreshConversations()
    }
    
    func connect() {
        showingDeviceSelection = true
    }
    
    func startScanning() {
        messagingService.startScanning()
    }
    
    func stopScanning() {
        messagingService.stopScanning()
    }
    
    func connectToDevice(_ device: PeripheralDevice) {
        isConnecting = true
        errorMessage = nil
        
        Task {
            do {
                try await messagingService.connect(to: device.id)
                isConnecting = false
                refreshConversations()
            } catch {
                isConnecting = false
                errorMessage = error.localizedDescription
            }
        }
    }
    
    func disconnect() {
        messagingService.disconnect()
        refreshConversations()
    }
    
    func refreshConversations() {
        conversations = messagingService.recentConversations()
    }

    func deleteConversation(_ conversation: Conversation) {
        messagingService.deleteConversation(conversation)
        refreshConversations()
    }

    // MARK: - Auto-reconnect

    private func attemptAutoReconnect() {
        guard !isAutoReconnecting,
              !isConnecting,
              let savedString = UserDefaults.standard.string(forKey: "lastConnectedDeviceId"),
              let deviceId = UUID(uuidString: savedString) else { return }

        NSLog("💬 [Inbox] 🔄 Auto-reconnecting to last device: \(savedString)")
        isAutoReconnecting = true
        isConnecting = true

        Task {
            do {
                try await messagingService.connect(to: deviceId)
                refreshConversations()
                NSLog("💬 [Inbox] ✅ Auto-reconnect succeeded")
            } catch {
                NSLog("💬 [Inbox] ⚠️ Auto-reconnect failed: \(error.localizedDescription)")
                // Don't surface this error — user can manually connect
            }
            isAutoReconnecting = false
        }
    }
}
