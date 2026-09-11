//
//  RootView.swift — the milestone-1 navigation skeleton.
//
//  Four destinations, matching the screens A01 scopes for M1: Connect,
//  Radar, Inbox, Settings. Each is a placeholder that states what it
//  will show and, deliberately, shows NOTHING ELSE. There is no sample
//  crew, no demo node, no placeholder distance — a screen that invents
//  data here is exactly the failure the whole product is designed
//  against (docs/ARCHITECTURE.md, "Honest state").
//
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
            detail(for: selection)
        }
        #else
        TabView(selection: $selection) {
            ForEach(Destination.allCases) { destination in
                detail(for: destination)
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
        case .connect: ConnectView(model: connect)
        case .radar: PlaceholderView(
            title: "Radar",
            note: "Live bearings when a node reports a position, the no-GPS signal view when none does, and FIND. "
                + "Nothing is drawn until a radio says something.")
        case .inbox: PlaceholderView(
            title: "Inbox",
            note: "Conversations and threads, with WAITING / SENT / DELIVERED / NO ACK on every outbound message.")
        case .settings: PlaceholderView(
            title: "Settings",
            note: "Name, channel, units, and a diagnostics page showing link state, frame counters and the "
                + "firmware version actually in front of you.")
        }
    }
}

struct ConnectView: View {
    let model: ConnectViewModel

    var body: some View {
        VStack(spacing: 24) {
            Text("FIREFLY")
                .font(.system(.largeTitle, design: .rounded).weight(.heavy))
                .foregroundStyle(Color.ffAmber)

            Text(model.statusLabel)
                .font(.system(.headline, design: .monospaced))
                .foregroundStyle(model.link == .ready ? Color.ffLiveGreen : Color.ffMuted)

            if let error = model.lastError {
                Text(error)
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
                    .multilineTextAlignment(.center)
            }

            Button("CONNECT") {
                Task { await model.connect() }
            }
            .buttonStyle(.borderedProminent)
            .tint(.ffAmber)
            .foregroundStyle(Color.ffBackground)

            Text("No node picker yet. This build talks to a stub client, "
                 + "so it will never claim to have found a radio it has not.")
                .font(.caption)
                .foregroundStyle(Color.ffMuted)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 32)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color.ffBackground)
        .onAppear { model.observe() }
        .onDisappear { model.stopObserving() }
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
