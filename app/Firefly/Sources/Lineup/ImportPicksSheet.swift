//
//  ImportPicksSheet.swift — "Import picks": paste a settimes share URL
//  or a bare code list (docs/specs/A01-companion-app.md, Lineup:
//  "Import picks action that accepts a pasted URL or code").
//
import FireflyModel
import SwiftUI

struct ImportPicksSheet: View {
    @Bindable var model: LineupViewModel
    @Binding var isPresented: Bool
    @Binding var feedback: String?
    @State private var text: String = ""

    var body: some View {
        NavigationStack {
            VStack(alignment: .leading, spacing: 16) {
                Text("Paste a settimes.kandiwooks.com share link, or just the code after \"picks=\".")
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
                TextField("https://settimes.kandiwooks.com/…", text: $text, axis: .vertical)
                    .textFieldStyle(.roundedBorder)
                    .lineLimit(3...6)
                    #if os(iOS)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    #endif
                Spacer()
            }
            .padding()
            .background(Color.ffBackground)
            .navigationTitle("Import picks")
            #if os(iOS)
            .navigationBarTitleDisplayMode(.inline)
            #endif
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { isPresented = false }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Import") { importAndClose() }
                        .disabled(text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                }
            }
        }
    }

    private func importAndClose() {
        let result = model.importPicks(from: text)
        switch result {
        case .imported(let count, let dropped):
            feedback = dropped > 0
                ? "Added \(count) pick\(count == 1 ? "" : "s") — \(dropped) code\(dropped == 1 ? "" : "s") no longer match this pack."
                : "Added \(count) pick\(count == 1 ? "" : "s")."
        case .nothingFound:
            feedback = "That didn't look like a picks link or code."
        }
        isPresented = false
    }
}
