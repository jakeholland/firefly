//
//  InboxView.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import SwiftUI

/// Main inbox view showing channels and direct messages
struct InboxView: View {
    @StateObject private var viewModel: InboxViewModel
    @State private var showingNewConversation = false
    
    init(container: DependencyContainer = .production()) {
        _viewModel = StateObject(wrappedValue: InboxViewModel(messagingService: container.messagingService))
    }
    
    var body: some View {
        NavigationStack {
            ZStack {
                LinearGradient(
                    colors: [.black, Color(red: 0.1, green: 0, blue: 0.2)],
                    startPoint: .top,
                    endPoint: .bottom
                )
                .ignoresSafeArea()
                
                VStack(spacing: 0) {
                    connectionStatusBar
                    
                    if viewModel.conversations.isEmpty {
                        emptyStateView
                    } else {
                        conversationList
                    }
                }
            }
            .navigationTitle("Inbox")
            .navigationBarTitleDisplayMode(.large)
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    Button {
                        showingNewConversation = true
                    } label: {
                        Image(systemName: "plus.circle.fill")
                            .font(.title2)
                            .foregroundStyle(.cyan)
                    }
                    .disabled(!viewModel.isConnected)
                }
            }
            .sheet(isPresented: $viewModel.showingDeviceSelection) {
                DeviceSelectionView(viewModel: viewModel)
            }
            .sheet(isPresented: $showingNewConversation) {
                NewConversationView(
                    messagingService: viewModel.messagingService,
                    persistenceService: PersistenceService.shared,
                    myNodeId: viewModel.myNodeId
                )
                .onDisappear {
                    // Refresh conversations when sheet dismisses
                    viewModel.refreshConversations()
                }
            }
        }
        .preferredColorScheme(.dark)
    }
    
    private var connectionStatusBar: some View {
        HStack {
            Circle()
                .fill(viewModel.isConnected ? Color.green : Color.red)
                .frame(width: 8, height: 8)
            
            Text(viewModel.connectionStatusText)
                .font(.caption)
                .foregroundStyle(.secondary)
            
            Spacer()
            
            if viewModel.isConnecting {
                ProgressView()
                    .scaleEffect(0.8)
            } else if !viewModel.isConnected {
                Button("Connect") {
                    viewModel.connect()
                }
                .font(.caption)
                .buttonStyle(.borderedProminent)
                .tint(.cyan)
            } else {
                Button("Disconnect") {
                    viewModel.disconnect()
                }
                .font(.caption)
                .buttonStyle(.bordered)
                .tint(.red)
            }
        }
        .padding()
        .background(Color.black.opacity(0.3))
    }
    
    private var conversationList: some View {
        List {
            ForEach(viewModel.conversations) { conversation in
                NavigationLink(value: conversation) {
                    ConversationRowView(conversation: conversation)
                }
                .listRowBackground(Color.black.opacity(0.3))
            }
        }
        .listStyle(.plain)
        .scrollContentBackground(.hidden)
        .navigationDestination(for: Conversation.self) { conversation in
            ConversationView(conversation: conversation)
        }
    }
    
    private var emptyStateView: some View {
        VStack(spacing: 16) {
            Image(systemName: "antenna.radiowaves.left.and.right")
                .font(.system(size: 60))
                .foregroundStyle(.cyan.opacity(0.5))
            
            Text("No Conversations")
                .font(.title2)
                .foregroundStyle(.secondary)
            
            Text("Connect to a Meshtastic device to start chatting")
                .font(.caption)
                .foregroundStyle(.tertiary)
                .multilineTextAlignment(.center)
                .padding(.horizontal)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

struct ConversationRowView: View {
    let conversation: Conversation
    
    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: icon)
                .font(.title2)
                .foregroundStyle(iconColor)
                .frame(width: 40, height: 40)
                .background(iconColor.opacity(0.2))
                .clipShape(Circle())
            
            VStack(alignment: .leading, spacing: 4) {
                Text(conversation.displayName)
                    .font(.headline)
                    .foregroundStyle(.primary)
                
                if let lastMessage = conversation.lastMessage {
                    Text(lastMessage.text)
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }
            
            Spacer()
            
            if let lastMessage = conversation.lastMessage {
                Text(lastMessage.timestamp, style: .relative)
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }
        }
        .padding(.vertical, 4)
    }
    
    private var icon: String {
        switch conversation {
        case .channel: return "number"
        case .directMessage: return "person.fill"
        }
    }
    
    private var iconColor: Color {
        switch conversation {
        case .channel: return .cyan
        case .directMessage: return .purple
        }
    }
}

#Preview {
    InboxView()
}
