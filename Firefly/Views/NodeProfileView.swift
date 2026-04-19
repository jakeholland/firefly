//
//  NodeProfileView.swift
//  Firefly
//
//  Created by Jake Holland on 4/19/26.
//

import SwiftUI

/// Sheet for viewing and editing the local node's display name.
struct NodeProfileView: View {
    @Environment(\.dismiss) private var dismiss
    let messagingService: MessagingServiceProtocol

    @State private var longName: String = ""
    @State private var isSaving = false
    @State private var errorMessage: String?
    @State private var saved = false

    private var currentNode: MeshNode? { messagingService.myNodeInfo() }

    var body: some View {
#if os(macOS)
        macOSLayout
#else
        iOSLayout
#endif
    }

    // MARK: - iOS

#if !os(macOS)
    private var iOSLayout: some View {
        NavigationStack {
            formContent
                .navigationTitle("My Profile")
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) {
                        Button("Cancel") { dismiss() }
                    }
                    ToolbarItem(placement: .confirmationAction) {
                        saveButton
                    }
                }
        }
        .preferredColorScheme(.dark)
        .onAppear(perform: prefill)
    }
#endif

    // MARK: - macOS

#if os(macOS)
    private var macOSLayout: some View {
        ZStack {
            LinearGradient(
                colors: [.black, Color(red: 0.1, green: 0, blue: 0.2)],
                startPoint: .top,
                endPoint: .bottom
            )
            .ignoresSafeArea()

            VStack(spacing: 0) {
                HStack {
                    Button("Cancel") { dismiss() }
                        .buttonStyle(.plain)
                        .foregroundStyle(.secondary)

                    Spacer()

                    Text("My Profile")
                        .font(.headline)

                    Spacer()

                    saveButton
                }
                .padding(.horizontal, 20)
                .padding(.vertical, 14)
                .background(Color.black.opacity(0.4))

                Divider()
                    .overlay(Color.white.opacity(0.1))

                formContent
            }
        }
        .frame(minWidth: 380, minHeight: 300)
        .preferredColorScheme(.dark)
        .onAppear(perform: prefill)
    }
#endif

    // MARK: - Shared form

    private var formContent: some View {
        ZStack {
            LinearGradient(
                colors: [.black, Color(red: 0.1, green: 0, blue: 0.2)],
                startPoint: .top,
                endPoint: .bottom
            )
            .ignoresSafeArea()

            VStack(spacing: 24) {
                // Avatar
                ZStack {
                    Circle()
                        .fill(LinearGradient(
                            colors: [.purple, .cyan],
                            startPoint: .topLeading,
                            endPoint: .bottomTrailing
                        ))
                        .frame(width: 80, height: 80)

                    Image(systemName: "person.fill")
                        .font(.title)
                        .foregroundStyle(.white)
                }
                .padding(.top, 16)

                if let node = currentNode {
                    Text(String(format: "Node ID: !%08x", node.id))
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                }

                VStack(spacing: 16) {
                    fieldRow(
                        label: "Name",
                        hint: "Shown to others on the mesh",
                        placeholder: "e.g. Jake Holland",
                        text: $longName
                    )
                }
                .padding(.horizontal, 20)

                if let error = errorMessage {
                    Text(error)
                        .font(.caption)
                        .foregroundStyle(.red)
                        .padding(.horizontal, 20)
                }

                if saved {
                    Label("Saved!", systemImage: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                        .font(.subheadline)
                        .transition(.opacity)
                }

                Spacer()
            }
        }
    }

    private func fieldRow(label: String, hint: String, placeholder: String, text: Binding<String>) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(label)
                .font(.caption)
                .foregroundStyle(.secondary)

            TextField(placeholder, text: text)
                .textFieldStyle(.plain)
                .padding(12)
                .background(Color.white.opacity(0.08))
                .clipShape(RoundedRectangle(cornerRadius: 10))

            Text(hint)
                .font(.caption2)
                .foregroundStyle(.tertiary)
        }
    }

    @ViewBuilder
    private var saveButton: some View {
        if isSaving {
            ProgressView()
                .scaleEffect(0.8)
        } else {
            Button("Save") { save() }
                .disabled(longName.trimmingCharacters(in: .whitespaces).isEmpty)
        }
    }

    // MARK: - Actions

    private func prefill() {
        longName = currentNode?.longName ?? ""
    }

    private func save() {
        let long = longName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !long.isEmpty else { return }

        isSaving = true
        errorMessage = nil

        Task { @MainActor in
            do {
                try await messagingService.renameNode(longName: long)
                isSaving = false
                withAnimation { saved = true }
                try? await Task.sleep(for: .milliseconds(800))
                dismiss()
            } catch {
                isSaving = false
                errorMessage = error.localizedDescription
            }
        }
    }
}

#Preview {
    NodeProfileView(messagingService: MockMessagingService())
}
