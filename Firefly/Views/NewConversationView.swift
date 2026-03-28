//
//  NewConversationView.swift
//  Firefly
//
//  Created by Jake Holland on 2/22/26.
//

import SwiftUI
import Combine

/// View for starting a new 1-1 conversation with nearby users
struct NewConversationView: View {
    @Environment(\.dismiss) private var dismiss
    @StateObject private var viewModel: NewConversationViewModel
    
    init(messagingService: MessagingServiceProtocol, persistenceService: PersistenceService, myNodeId: UInt32?) {
        _viewModel = StateObject(wrappedValue: NewConversationViewModel(
            messagingService: messagingService,
            persistenceService: persistenceService,
            myNodeId: myNodeId
        ))
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
                    searchBar
                    
                    if viewModel.isLoading {
                        loadingView
                    } else if viewModel.filteredUsers.isEmpty {
                        emptyStateView
                    } else {
                        userList
                    }
                }
            }
            .navigationTitle("New Conversation")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") {
                        dismiss()
                    }
                }
                
                ToolbarItem(placement: .primaryAction) {
                    Button {
                        viewModel.refreshUsers()
                    } label: {
                        Image(systemName: "arrow.clockwise")
                    }
                }
            }
            .onAppear {
                viewModel.loadUsers()
            }
        }
        .preferredColorScheme(.dark)
    }
    
    private var searchBar: some View {
        HStack {
            Image(systemName: "magnifyingglass")
                .foregroundStyle(.secondary)
            
            TextField("Search users...", text: $viewModel.searchText)
                .textFieldStyle(.plain)
                .foregroundStyle(.primary)
            
            if !viewModel.searchText.isEmpty {
                Button {
                    viewModel.searchText = ""
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .foregroundStyle(.secondary)
                }
            }
        }
        .padding()
        .background(Color.black.opacity(0.3))
        .clipShape(RoundedRectangle(cornerRadius: 10))
        .padding()
    }
    
    private var userList: some View {
        List {
            Section {
                ForEach(viewModel.filteredUsers) { user in
                    Button {
                        viewModel.selectUser(user)
                        dismiss()
                    } label: {
                        UserRowView(user: user)
                    }
                    .listRowBackground(Color.black.opacity(0.3))
                }
            } header: {
                Text("\(viewModel.filteredUsers.count) Nearby Users")
                    .foregroundStyle(.secondary)
            }
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
    }
    
    private var loadingView: some View {
        VStack(spacing: 20) {
            ProgressView()
                .scaleEffect(1.5)
                .tint(.cyan)
            
            Text("Looking for users...")
                .font(.title3)
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
    
    private var emptyStateView: some View {
        VStack(spacing: 20) {
            Image(systemName: "person.3.fill")
                .font(.system(size: 60))
                .foregroundStyle(.cyan.opacity(0.5))
            
            Text("No Users Found")
                .font(.title2)
                .foregroundStyle(.secondary)
            
            Text(viewModel.searchText.isEmpty ? "No users are currently visible on the mesh" : "No users match your search")
                .font(.caption)
                .foregroundStyle(.tertiary)
                .multilineTextAlignment(.center)
                .padding(.horizontal)
            
            Button {
                viewModel.refreshUsers()
            } label: {
                Label("Refresh", systemImage: "arrow.clockwise")
                    .font(.headline)
            }
            .buttonStyle(.borderedProminent)
            .tint(.cyan)
            .padding(.top)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

struct UserRowView: View {
    let user: MeshNode
    
    var body: some View {
        HStack(spacing: 16) {
            Image(systemName: "person.circle.fill")
                .font(.title2)
                .foregroundStyle(.purple)
                .frame(width: 40, height: 40)
                .background(.purple.opacity(0.2))
                .clipShape(Circle())
            
            VStack(alignment: .leading, spacing: 4) {
                Text(user.displayName)
                    .font(.headline)
                    .foregroundStyle(.primary)
                
                HStack(spacing: 8) {
                    if let hardwareModel = user.hardwareModel {
                        Text(hardwareModel)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    
                    if let lastHeard = user.lastHeard {
                        Text("•")
                            .foregroundStyle(.tertiary)
                        
                        Text(lastHeard, style: .relative)
                            .font(.caption)
                            .foregroundStyle(.tertiary)
                    }
                }
            }
            
            Spacer()
            
            Image(systemName: "chevron.right")
                .font(.caption)
                .foregroundStyle(.tertiary)
        }
        .padding(.vertical, 4)
    }
}

@MainActor
final class NewConversationViewModel: ObservableObject {
    @Published var users: [MeshNode] = []
    @Published var searchText: String = ""
    @Published var isLoading: Bool = false
    @Published var selectedUser: MeshNode?
    
    private let messagingService: MessagingServiceProtocol
    private let persistenceService: PersistenceService
    private let myNodeId: UInt32?
    
    var filteredUsers: [MeshNode] {
        if searchText.isEmpty {
            return users
        } else {
            return users.filter { user in
                user.displayName.localizedCaseInsensitiveContains(searchText) ||
                user.shortName?.localizedCaseInsensitiveContains(searchText) == true ||
                user.hardwareModel?.localizedCaseInsensitiveContains(searchText) == true
            }
        }
    }
    
    init(messagingService: MessagingServiceProtocol, persistenceService: PersistenceService, myNodeId: UInt32?) {
        self.messagingService = messagingService
        self.persistenceService = persistenceService
        self.myNodeId = myNodeId
        
        NSLog("💬 [NewConversation] Initialized with myNodeId: \(myNodeId.map { String($0, radix: 16) } ?? "nil")")
    }
    
    func loadUsers() {
        NSLog("💬 [NewConversation] Loading users from mesh...")
        isLoading = true
        
        // First, get users from the live mesh network
        var meshUsers = messagingService.nodes().filter { $0.id != myNodeId }
        NSLog("💬 [NewConversation] 📡 Found \(meshUsers.count) users from mesh")
        
        // Then, get any persisted users we might have talked to before
        let persistedUsers = persistenceService.fetchAllNodes().filter { $0.id != myNodeId }
        NSLog("💬 [NewConversation] 💾 Found \(persistedUsers.count) users from persistence")
        
        // Merge them, preferring mesh data for users that exist in both
        let meshUserIds = Set(meshUsers.map { $0.id })
        let additionalPersistedUsers = persistedUsers.filter { !meshUserIds.contains($0.id) }
        
        meshUsers.append(contentsOf: additionalPersistedUsers)
        
        // Sort by last heard (most recent first), with users never heard at the end
        users = meshUsers.sorted { user1, user2 in
            switch (user1.lastHeard, user2.lastHeard) {
            case (.some(let date1), .some(let date2)):
                return date1 > date2
            case (.some, .none):
                return true
            case (.none, .some):
                return false
            case (.none, .none):
                return user1.displayName < user2.displayName
            }
        }
        
        isLoading = false
        NSLog("💬 [NewConversation] ✅ Loaded \(users.count) total users")
    }
    
    func refreshUsers() {
        NSLog("💬 [NewConversation] 🔄 Refreshing users...")
        loadUsers()
    }
    
    func selectUser(_ user: MeshNode) {
        NSLog("💬 [NewConversation] 👤 Selected user: \(user.displayName)")
        selectedUser = user
    }
}

#Preview {
    NewConversationView(
        messagingService: MockMessagingService(),
        persistenceService: PersistenceService.shared,
        myNodeId: 0x12345678
    )
}
