//
//  RootView.swift — the milestone-1 navigation skeleton (Slice C owns
//  this file and its destination registry; slices D and E each change
//  exactly one line in the `switch` below — see docs/specs/
//  A01-companion-app.md, "Slices", the shared-file table).
//
//  Four destinations, matching the screens A01 scopes for M1: Connect,
//  Radar, Inbox, Settings. Radar and Inbox stay placeholders here —
//  they state what they will show and, deliberately, show NOTHING
//  ELSE; a screen that invents data is exactly the failure this whole
//  product is designed against (docs/ARCHITECTURE.md, "Honest state").
//
import FireflyMesh
import FireflyModel
import SwiftUI

enum Destination: String, CaseIterable, Identifiable {
    case connect = "Connect"
    case radar = "Radar"
    case inbox = "Inbox"
    case settings = "Settings"

    var id: String { rawValue }

    var systemImage: String {
        switch self {
        case .connect: return "antenna.radiowaves.left.and.right"
        case .radar: return "location.north.line"
        case .inbox: return "tray"
        case .settings: return "slider.horizontal.3"
        }
    }
}

struct RootView: View {
    let connect: ConnectViewModel
    let settings: SettingsViewModel
    let channelImport: ChannelImportViewModel
    let client: any MeshtasticClientProtocol
    // Slice E's hunk (A01's Slices table: "Slices C, D, E each change
    // exactly one of those [switch] lines" — constructing that line's
    // view needs this view model reference alongside `connect`'s).
    let inbox: InboxViewModel
    // Every view model is constructed ONCE, in `FireflyApp.init`, from
    // the one `AppGraph` — never inside a destination's own `init`,
    // which would build a second client the moment the destination was
    // shown (`AppGraph`'s header comment).
    let radar: RadarViewModel
    /// The BLE node picker's scan seam — nil wherever there is no radio
    /// (the stub stack, the iOS Simulator), which the Connect screen
    /// renders as an honestly empty picker.
    let scanner: (any NodeScanning)?
    @State private var selection: Destination = .connect

    var body: some View {
        #if os(macOS)
        NavigationSplitView {
            List(Destination.allCases, selection: $selection) { destination in
                Label(destination.rawValue, systemImage: destination.systemImage)
                    .tag(destination)
            }
            .navigationSplitViewColumnWidth(min: 160, ideal: 180)
        } detail: {
            NavigationStack {
                detail(for: selection)
            }
        }
        #else
        TabView(selection: $selection) {
            ForEach(Destination.allCases) { destination in
                NavigationStack {
                    detail(for: destination)
                }
                .tabItem { Label(destination.rawValue, systemImage: destination.systemImage) }
                .tag(destination)
            }
        }
        .tint(.ffAmber)
        #endif
    }

    @ViewBuilder
    private func detail(for destination: Destination) -> some View {
        switch destination {
        case .connect: ConnectScreen(connect: connect, client: client, channelImport: channelImport,
                                      scanner: scanner)
        case .radar: RadarView(model: radar)
        case .inbox: InboxContainerView(model: inbox)
        case .settings: SettingsScreen(model: settings, client: client)
        }
    }
}

struct PlaceholderView: View {
    let title: String
    let note: String

    var body: some View {
        VStack(spacing: 16) {
            Text(title.uppercased())
                .font(.system(.title2, design: .rounded).weight(.bold))
                .foregroundStyle(Color.ffInk)
            Text(note)
                .font(.callout)
                .foregroundStyle(Color.ffMuted)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 32)
            Text("NOT BUILT YET")
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(Color.ffAmber)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color.ffBackground)
    }
}
