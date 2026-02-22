//
//  NewConversationView.swift
//  Firefly
//
//  Created by Jake Holland on 2/22/26.
//

import SwiftUI
import Combine

/// View for starting a new 1-1 conversation with nearby nodes
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
                    } else if viewModel.filteredNodes.isEmpty {
                        emptyStateView
                    } else {
                        nodeList
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
                        viewModel.refreshNodes()
                    } label: {
                        Image(systemName: "arrow.clockwise")
                    }
                }
            }
            .onAppear {
                viewModel.loadNodes()
            }
        }
        .preferredColorScheme(.dark)
    }
    
    private var searchBar: some View {
        HStack {
            Image(systemName: "magnifyingglass")
                .foregroundStyle(.secondary)
            
            TextField("Search nodes...", text: $viewModel.searchText)
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
    
    private var nodeList: some View {
        List {
            Section {
                ForEach(viewModel.filteredNodes) { node in
                    Button {
                        viewModel.selectNode(node)
                        dismiss()
                    } label: {
                        NodeRowView(node: node)
                    }
                    .listRowBackground(Color.black.opacity(0.3))
                }
            } header: {
                Text("\(viewModel.filteredNodes.count) Nearby Nodes")
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
            
            Text("Loading nodes...")
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
            
            Text("No Nodes Found")
                .font(.title2)
                .foregroundStyle(.secondary)
            
            Text(viewModel.searchText.isEmpty ? "No nodes are currently visible on the mesh" : "No nodes match your search")
                .font(.caption)
                .foregroundStyle(.tertiary)
                .multilineTextAlignment(.center)
                .padding(.horizontal)
            
            Button {
                viewModel.refreshNodes()
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

struct NodeRowView: View {
    let node: MeshNode
    
    var body: some View {
        HStack(spacing: 16) {
            Image(systemName: "person.circle.fill")
                .font(.title2)
                .foregroundStyle(.purple)
                .frame(width: 40, height: 40)
                .background(.purple.opacity(0.2))
                .clipShape(Circle())
            
            VStack(alignment: .leading, spacing: 4) {
                Text(node.displayName)
                    .font(.headline)
                    .foregroundStyle(.primary)
                
                HStack(spacing: 8) {
                    if let hardwareModel = node.hardwareModel {
                        Text(hardwareModel)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    
                    if let lastHeard = node.lastHeard {
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
    @Published var nodes: [MeshNode] = []
    @Published var searchText: String = ""
    @Published var isLoading: Bool = false
    @Published var selectedNode: MeshNode?
    
    private let messagingService: MessagingServiceProtocol
    private let persistenceService: PersistenceService
    private let myNodeId: UInt32?
    
    var filteredNodes: [MeshNode] {
        if searchText.isEmpty {
            return nodes
        } else {
            return nodes.filter { node in
                node.displayName.localizedCaseInsensitiveContains(searchText) ||
                node.shortName?.localizedCaseInsensitiveContains(searchText) == true ||
                node.hardwareModel?.localizedCaseInsensitiveContains(searchText) == true
            }
        }
    }
    
    init(messagingService: MessagingServiceProtocol, persistenceService: PersistenceService, myNodeId: UInt32?) {
        self.messagingService = messagingService
        self.persistenceService = persistenceService
        self.myNodeId = myNodeId
        
        NSLog("💬 [NewConversation] Initialized with myNodeId: \(myNodeId.map { String($0, radix: 16) } ?? "nil")")
    }
    
    func loadNodes() {
        NSLog("💬 [NewConversation] Loading nodes...")
        isLoading = true
        
        // Get nodes from persistence and filter out self
        let allNodes = persistenceService.fetchAllNodes()
        nodes = allNodes.filter { $0.id != myNodeId }
        
        isLoading = false
        NSLog("💬 [NewConversation] ✅ Loaded \(nodes.count) nodes (filtered out self)")
    }
    
    func refreshNodes() {
        NSLog("💬 [NewConversation] 🔄 Refreshing nodes...")
        loadNodes()
    }
    
    func selectNode(_ node: MeshNode) {
        NSLog("💬 [NewConversation] 👤 Selected node: \(node.displayName)")
        selectedNode = node
    }
}

#Preview {
    NewConversationView(
        messagingService: MockMessagingService(),
        persistenceService: PersistenceService.shared,
        myNodeId: 0x12345678
    )
}
