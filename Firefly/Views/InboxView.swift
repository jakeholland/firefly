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
    @State private var showingProfile = false
    @State private var navigationPath = NavigationPath()
    /// Holds the DM conversation selected in `NewConversationView` so we can push
    /// it onto the NavigationStack after the sheet finishes dismissing.
    @State private var pendingDMConversation: Conversation?

    /// Kept to forward into `NewConversationView` rather than accessing `PersistenceService.shared`.
    private let persistenceService: PersistenceService?

    init(container: DependencyContainer = .production()) {
        _viewModel = StateObject(wrappedValue: InboxViewModel(messagingService: container.messagingService))
        persistenceService = container.persistenceService
    }

    var body: some View {
        NavigationStack(path: $navigationPath) {
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
            #if os(iOS)
            .navigationBarTitleDisplayMode(.large)
            #endif
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
                ToolbarItem(placement: .navigation) {
                    Button {
                        showingProfile = true
                    } label: {
                        Image(systemName: "person.circle")
                            .font(.title2)
                    }
                    .disabled(!viewModel.isConnected)
                }
            }
            .sheet(isPresented: $showingProfile) {
                NodeProfileView(messagingService: viewModel.messagingService)
            }
            .sheet(isPresented: $viewModel.showingDeviceSelection) {
                DeviceSelectionView(viewModel: viewModel)
            }
            .sheet(isPresented: $showingNewConversation) {
                NewConversationView(
                    messagingService: viewModel.messagingService,
                    persistenceService: persistenceService,
                    myNodeId: viewModel.myNodeId,
                    onSelectUser: { node in
                        pendingDMConversation = .directMessage(node: node, lastMessage: nil)
                    }
                )
                .onDisappear {
                    viewModel.refreshConversations()
                    // Push the selected DM conversation onto the nav stack once the
                    // sheet has fully dismissed (pushing during dismissal glitches).
                    if let dm = pendingDMConversation {
                        navigationPath.append(dm)
                        pendingDMConversation = nil
                    }
                }
            }
        }
        .preferredColorScheme(.dark)
    }
    
    private var connectionStatusBar: some View {
        HStack {
            Circle()
                .fill(statusDotColor)
                .frame(width: 8, height: 8)

            Text(viewModel.connectionStatusText)
                .font(.caption)
                .foregroundStyle(.secondary)

            Spacer()

            if viewModel.isConnecting || viewModel.isScanning {
                ProgressView()
                    .scaleEffect(0.8)
            }

            if !viewModel.isConnected && !viewModel.isConnecting && !viewModel.isScanning {
                Button("Connect") {
                    viewModel.connect()
                }
                .font(.caption)
                .buttonStyle(.borderedProminent)
                .tint(.cyan)
            } else if viewModel.isConnected {
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

    private var statusDotColor: Color {
        switch viewModel.connectionState {
        case .connected:              return .green
        case .scanning, .connecting:  return .yellow
        case .resetting, .unknown:    return .orange
        case .disconnected, .failed:  return .red
        }
    }
    
    private var conversationList: some View {
        List {
            ForEach(viewModel.conversations) { conversation in
                NavigationLink(value: conversation) {
                    ConversationRowView(conversation: conversation)
                }
                .listRowBackground(Color.black.opacity(0.3))
                .swipeActions(edge: .trailing, allowsFullSwipe: true) {
                    if case .directMessage = conversation {
                        Button(role: .destructive) {
                            viewModel.deleteConversation(conversation)
                        } label: {
                            Label("Delete", systemImage: "trash")
                        }
                    }
                }
            }
        }
        .listStyle(.plain)
        .scrollContentBackground(.hidden)
        .navigationDestination(for: Conversation.self) { conversation in
            ConversationView(conversation: conversation, messagingService: viewModel.messagingService)
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
                Text(lastMessage.timestamp.conversationTimestamp)
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

// MARK: - Timestamp formatting

private extension Date {
    /// "2:34 PM" for today, "Mon" for this week, "Apr 15" for older.
    var conversationTimestamp: String {
        let cal = Calendar.current
        if cal.isDateInToday(self) {
            return Self.timeFormatter.string(from: self)
        } else if let daysAgo = cal.dateComponents([.day], from: self, to: .now).day, daysAgo < 7 {
            return Self.weekdayFormatter.string(from: self)
        } else {
            return Self.dateFormatter.string(from: self)
        }
    }

    private static let timeFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateStyle = .none
        f.timeStyle = .short
        return f
    }()

    private static let weekdayFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "EEE"
        return f
    }()

    private static let dateFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "MMM d"
        return f
    }()
}
