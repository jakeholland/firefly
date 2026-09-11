//
//  ConnectScreen.swift — the Connect destination (docs/specs/
//  A01-companion-app.md, M1: "node picker (BLE on both platforms),
//  connection state including a distinct HANDSHAKING, channel import").
//
//  Sections, top to bottom: connection state + connect/disconnect,
//  the BLE node picker (honestly empty until slice A's scanner exists
//  — see PeripheralDiscovery.swift), Nearby heard nodes ranked by
//  signal tier with Add to Crew, and channel import (paste everywhere,
//  scan on iOS).
//
import FireflyMesh
import FireflyModel
import SwiftUI

struct ConnectScreen: View {
    let connect: ConnectViewModel
    let client: any MeshtasticClientProtocol
    /// Shared with the Settings screen (`FireflyApp.swift` constructs
    /// one instance and hands it to both), so a channel imported here
    /// is the same one Settings' "Channel" row reads — never a second,
    /// disconnected copy that could disagree with what this screen
    /// shows.
    let channelImport: ChannelImportViewModel

    @State private var nearby: NearbyNodesViewModel
    @State private var discovery: any PeripheralDiscovering = StubPeripheralDiscovery()
    @State private var peripherals: [DiscoveredPeripheral] = []
    @State private var channelURLText = ""
    @State private var isShowingScanner = false

    init(connect: ConnectViewModel, client: any MeshtasticClientProtocol, channelImport: ChannelImportViewModel) {
        self.connect = connect
        self.client = client
        self.channelImport = channelImport
        _nearby = State(initialValue: NearbyNodesViewModel(client: client))
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 28) {
                header
                connectionSection
                nodePickerSection
                nearbySection
                channelImportSection
            }
            .padding(20)
        }
        .background(Color.ffBackground)
        .onAppear {
            connect.observe()
            nearby.observe()
        }
        .onDisappear {
            connect.stopObserving()
            nearby.stopObserving()
        }
        .task {
            for await found in discovery.peripherals() {
                peripherals = found
            }
        }
        #if os(iOS)
        .sheet(isPresented: $isShowingScanner) {
            QRScannerSheet { payload in
                channelURLText = payload
                channelImport.importURL(payload)
            }
        }
        #endif
    }

    // MARK: - Header

    private var header: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("FIREFLY")
                .font(.system(.largeTitle, design: .rounded).weight(.heavy))
                .foregroundStyle(Color.ffAmber)
            Text("One crew. One channel. Nothing on this screen is invented.")
                .font(.caption)
                .foregroundStyle(Color.ffMuted)
        }
    }

    // MARK: - Connection state

    private var connectionSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 8) {
                Circle()
                    .fill(statusColor)
                    .frame(width: 10, height: 10)
                Text(connect.statusLabel)
                    .font(.system(.headline, design: .monospaced))
                    .foregroundStyle(statusColor)
            }

            if let error = connect.lastError {
                Text(error)
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
            }

            HStack(spacing: 12) {
                Button("CONNECT") { Task { await connect.connect() } }
                    .buttonStyle(.borderedProminent)
                    .tint(.ffAmber)
                    .foregroundStyle(Color.ffBackground)
                    .disabled(connect.link == .ready || connect.link == .connecting || connect.link == .handshaking)

                Button("DISCONNECT") { Task { await connect.disconnect() } }
                    .buttonStyle(.bordered)
                    .tint(.ffMuted)
                    .disabled(connect.link != .ready)
            }
            .frame(minHeight: 44)
        }
    }

    private var statusColor: Color {
        switch connect.link {
        case .ready: return .ffLiveGreen
        case .handshaking, .connecting: return .ffAmber
        case .failed: return .red
        case .disconnected: return .ffMuted
        }
    }

    // MARK: - Node picker (BLE peripherals)

    private var nodePickerSection: some View {
        SectionBlock(title: "NEARBY RADIOS") {
            if peripherals.isEmpty {
                Text("No Meshtastic radios found yet. This build has no BLE scanner wired in " +
                     "(that lands with the real transport) — an empty list here is the honest " +
                     "answer, not a stalled scan.")
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
            } else {
                ForEach(peripherals) { peripheral in
                    HStack {
                        Text(peripheral.name ?? peripheral.id)
                            .foregroundStyle(Color.ffInk)
                        Spacer()
                        Text("\(peripheral.rssiDbm) dBm")
                            .font(.system(.footnote, design: .monospaced))
                            .foregroundStyle(Color.ffMuted)
                    }
                    .frame(minHeight: 44)
                }
            }

            Button("RESCAN") { discovery.startScanning() }
                .buttonStyle(.bordered)
                .tint(.ffMuted)
                .frame(minHeight: 44)
        }
    }

    // MARK: - Nearby heard nodes (Add to crew)

    private var nearbySection: some View {
        SectionBlock(title: "NEARBY") {
            if nearby.nodes.isEmpty {
                Text("Nobody heard yet. This fills in once the client reports mesh traffic — " +
                     "never before.")
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
            } else {
                ForEach(nearby.nodes) { node in
                    HStack(spacing: 12) {
                        SignalBars(tier: node.tier)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(node.displayName)
                                .foregroundStyle(Color.ffInk)
                            Text(node.tier.label)
                                .font(.caption2)
                                .foregroundStyle(Color.ffMuted)
                        }
                        Spacer()
                        Button(node.isCrew ? "IN CREW" : "ADD TO CREW") {
                            nearby.toggleCrew(node.id)
                        }
                        .buttonStyle(.bordered)
                        .tint(node.isCrew ? .ffLiveGreen : .ffAmber)
                        .font(.caption)
                    }
                    .frame(minHeight: 44)
                }
            }
        }
    }

    // MARK: - Channel import

    private var channelImportSection: some View {
        SectionBlock(title: "CHANNEL") {
            TextField("Paste a meshtastic.org/e/#… link", text: $channelURLText)
                .textFieldStyle(.roundedBorder)
                .frame(minHeight: 44)
                #if os(iOS)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                #endif

            HStack(spacing: 12) {
                Button("IMPORT") { channelImport.importURL(channelURLText) }
                    .buttonStyle(.borderedProminent)
                    .tint(.ffAmber)
                    .foregroundStyle(Color.ffBackground)
                    .disabled(channelURLText.isEmpty)

                Button("PASTE") { pasteFromClipboard() }
                    .buttonStyle(.bordered)
                    .tint(.ffMuted)

                #if os(iOS)
                Button("SCAN") { isShowingScanner = true }
                    .buttonStyle(.bordered)
                    .tint(.ffMuted)
                #endif
            }
            .frame(minHeight: 44)

            if let error = channelImport.errorMessage {
                Text(error)
                    .font(.footnote)
                    .foregroundStyle(Color.red)
            }

            if let result = channelImport.result {
                VStack(alignment: .leading, spacing: 6) {
                    ForEach(Array(result.channelSet.settings.enumerated()), id: \.offset) { _, settings in
                        Text(settings.name.isEmpty ? "(default channel)" : settings.name)
                            .font(.system(.body, design: .monospaced))
                            .foregroundStyle(Color.ffLiveGreen)
                    }
                    if result.addMode {
                        Text("Adds to existing channels (add=true).")
                            .font(.caption)
                            .foregroundStyle(Color.ffMuted)
                    }
                    ForEach(channelImport.precisionWarnings, id: \.self) { warning in
                        Text(warning)
                            .font(.caption)
                            .foregroundStyle(Color.ffAmber)
                    }
                    Text("Shown only — not sent to the node. Writing a channel is admin-message " +
                         "territory, out of scope until M3.")
                        .font(.caption2)
                        .foregroundStyle(Color.ffMuted)
                }
            }
        }
    }

    private func pasteFromClipboard() {
        #if os(iOS)
        if let text = UIPasteboard.general.string { channelURLText = text }
        #elseif os(macOS)
        if let text = NSPasteboard.general.string(forType: .string) { channelURLText = text }
        #endif
    }
}

/// A boxed section, matching the puck's SURFACE-on-BG card language
/// rather than the platform's native grouped-list chrome.
private struct SectionBlock<Content: View>: View {
    let title: String
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(title)
                .font(.system(.caption, design: .monospaced).weight(.bold))
                .foregroundStyle(Color.ffMuted)
            content
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.ffSurface, in: RoundedRectangle(cornerRadius: 12))
    }
}

/// A strength glyph, never a scale bar with a unit on it —
/// `SignalTierPresentation.barFill` already enforces that the value
/// behind it cannot be read as a distance.
private struct SignalBars: View {
    let tier: SignalTierPresentation

    var body: some View {
        HStack(alignment: .bottom, spacing: 2) {
            ForEach(0..<4, id: \.self) { index in
                RoundedRectangle(cornerRadius: 1)
                    .fill(barColor(for: index))
                    .frame(width: 4, height: CGFloat(6 + index * 4))
            }
        }
        .frame(width: 44, height: 44)
    }

    private func barColor(for index: Int) -> Color {
        let filled = Double(index + 1) / 4.0 <= tier.barFill + 0.001
        return filled ? Color.ffAmber : Color.ffMuted.opacity(0.3)
    }
}
