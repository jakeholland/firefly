//
//  CrewHeardListView.swift — Crew -> Advanced -> "People my puck hears"
//  (`docs/specs/A02-crew-join.md` §4.7/§6.5, scoped for this slice —
//  see `CrewHeardListProviding.swift`'s header comment). Reached only
//  from `CrewAdvancedScreen`.
//
import FireflyModel
import SwiftUI

struct CrewHeardListView: View {
    let viewModel: CrewHeardListViewModel

    @State private var makeRoomTarget: UInt32?

    var body: some View {
        List {
            Section {
                Text("These are radios your puck hears on your crew's channel that aren't full " +
                     "crew members right now — someone you've hidden, or someone who scanned your " +
                     "code after your crew was already full.")
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
            }
            if let banner = viewModel.overflowBanner {
                Section {
                    Text(banner)
                        .font(.footnote.weight(.semibold))
                        .foregroundStyle(Color.ffAlert)
                }
            }
            if viewModel.isEmpty {
                Section {
                    Text("Nobody right now.").foregroundStyle(Color.ffMuted)
                }
            } else {
                Section {
                    ForEach(viewModel.rows) { row in
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(row.shortID)
                                    .font(.system(.body, design: .monospaced))
                                    .foregroundStyle(Color.ffInk)
                                Text(row.detail)
                                    .font(.caption)
                                    .foregroundStyle(Color.ffMuted)
                            }
                            Spacer()
                            switch row.kind {
                            case .hidden:
                                Button("Unhide") { viewModel.unhide(nodeID: row.id) }
                                    .buttonStyle(.bordered)
                                    .tint(Color.ffMuted)
                                    .foregroundStyle(Color.ffAmber)
                            case .overflow:
                                Button("Make room") { makeRoomTarget = row.id }
                                    .buttonStyle(.bordered)
                                    .tint(Color.ffMuted)
                                    .foregroundStyle(Color.ffAmber)
                                    .disabled(viewModel.makeRoomCandidates.isEmpty)
                            }
                        }
                    }
                }
            }
        }
        .navigationTitle("People my puck hears")
        .accessibilityIdentifier("Screen.CrewHeardList")
        .confirmationDialog("Hide who to make room?", isPresented: Binding(
            get: { makeRoomTarget != nil }, set: { if !$0 { makeRoomTarget = nil } }
        ), titleVisibility: .visible) {
            // `makeRoomTarget` (the OVERFLOW person, i.e. who this makes
            // room FOR) is never itself passed to `makeRoom(hiding:)` —
            // §4.3: hiding an EXISTING member is what frees a slot, and
            // the overflow person is admitted only by a real qualifying
            // packet (§4.1), never written to directly here. It gates
            // this dialog's visibility and nothing else.
            ForEach(viewModel.makeRoomCandidates) { member in
                Button(CrewCopy.displayName(member.displayName), role: .destructive) {
                    viewModel.makeRoom(hiding: member.id)
                    makeRoomTarget = nil
                }
            }
            Button("Cancel", role: .cancel) { makeRoomTarget = nil }
        } message: {
            Text("Hiding someone frees a slot. The person you're making room for is admitted " +
                 "automatically the next time your puck hears them.")
        }
    }
}
