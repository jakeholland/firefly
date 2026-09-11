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
    /// The real BLE scan when there is a radio behind it
    /// (`AppDependencies.live()`), and `StubPeripheralDiscovery` — which
    /// discovers nothing — when there is not (the iOS Simulator). Built
    /// here rather than injected from `AppGraph` because the picker's
    /// list is this screen's own state: nothing else in the app reads a
    /// scan result.
    @State private var discovery: any PeripheralDiscovering
    @State private var peripherals: [DiscoveredPeripheral] = []
    @State private var selectedPeripheralID: String?
    @State private var channelURLText = ""
    @State private var isShowingScanner = false
    /// M3 — "Apply to node" confirmation sheet for the imported channel
    /// (docs/specs/A01-companion-app.md M3). Separate from
    /// `isShowingScanner`'s `.sheet` — SwiftUI supports more than one
    /// `.sheet(isPresented:)` on the same view, each on its own Bool.
    @State private var isShowingApplyConfirmation = false
    /// `SettingsViewModel.colorblindPalette`, read fresh from `RootView`
    /// on every redraw (M2) — the SAME flag Radar's ring and the Inbox
    /// read, so a paired member's swatch here never disagrees with
    /// theirs. Never stored: this only selects WHICH palette a
    /// `colorIndex` resolves against at render time.
    let colorblind: Bool

    init(connect: ConnectViewModel, client: any MeshtasticClientProtocol,
         channelImport: ChannelImportViewModel, scanner: (any NodeScanning)?,
         pairing: CrewPairingController, colorblind: Bool) {
        self.connect = connect
        self.client = client
        self.channelImport = channelImport
        self.colorblind = colorblind
        _nearby = State(initialValue: NearbyNodesViewModel(client: client, pairing: pairing))
        _discovery = State(initialValue: scanner.map { MeshPeripheralDiscovery(scanner: $0) }
                            ?? StubPeripheralDiscovery())
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
            // `connect.observe()` is deliberately NOT called here any
            // more — `AppGraph.makeConnectViewModel()` starts it once,
            // for the life of the graph, and that file's own doc comment
            // is where the "app: fix live connect path never reaching
            // CONNECTED on macOS" root cause and its fix are written up.
            // Tying it to THIS screen's own appear/disappear is exactly
            // what broke: a `NavigationSplitView` detail-column remount
            // at launch (bench-reproduced, no user action at all) fires
            // `.onDisappear` once with no matching `.onAppear` ever
            // following it, permanently orphaning the subscription for
            // the rest of the process — "NOT CONNECTED" forever, no
            // matter how many times CONNECT is tapped afterward, even
            // though `MeshtasticClient` itself reaches `.ready` cleanly.
            nearby.observe()
            // Deliberately NOT auto-started. Starting a scan builds the
            // `CBCentralManager`, which is what makes macOS put up its
            // one-time Bluetooth permission dialog — and merely SHOWING
            // this screen is not the moment to ask. It is also the
            // moment `FireflyHardwareTests` launches its host app, so an
            // auto-scan here pops a modal in front of a test runner and
            // hangs it ("The test runner hung before establishing
            // connection", observed). RESCAN is the trigger; an empty
            // picker until the user asks is the honest state anyway.
        }
        .onDisappear {
            // `connect.stopObserving()` is deliberately NOT called here
            // any more — see `.onAppear`'s own comment just above. Link
            // state is process-lifetime state now, not per-screen state;
            // `nearby`'s own (genuinely screen-scoped: a node/inbox feed
            // IS meaningless while its screen is off-screen) subscription
            // is unaffected by this and still stops here as before.
            nearby.stopObserving()
            discovery.stopScanning()
        }
        .task {
            for await found in discovery.peripherals() {
                peripherals = found
            }
        }
        .task { await runAutoConnectIfRequested() }
        #if os(iOS)
        .sheet(isPresented: $isShowingScanner) {
            QRScannerSheet { payload in
                channelURLText = payload
                channelImport.importURL(payload)
            }
        }
        #endif
        // M3 — "Apply to node" behind an explicit confirmation sheet
        // (docs/specs/A01-companion-app.md M3). A SEPARATE `.sheet`
        // modifier from the scanner's above, on its own `@State` Bool —
        // SwiftUI allows more than one on the same view.
        .sheet(isPresented: $isShowingApplyConfirmation) {
            if let summary = channelImport.applySummary {
                AdminWriteConfirmationSheet(
                    title: "APPLY CHANNEL",
                    // BLOCKING 2 (PR #274 review): every slot's fate, not
                    // just the ones being written — WRITTEN, then
                    // DISABLED (replace only), then untouched (add
                    // only), then the region/preset line if this import
                    // carried a LoRa config.
                    changes: summary.channelLines + summary.disabledLines + summary.untouchedLines +
                             (summary.regionLine.map { [$0] } ?? []),
                    isBusy: channelImport.isApplying,
                    errorMessage: channelImport.applyErrorMessage,
                    onConfirm: {
                        Task {
                            if await channelImport.confirmApply() {
                                isShowingApplyConfirmation = false
                            }
                        }
                    },
                    onCancel: { isShowingApplyConfirmation = false })
            }
        }
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

            if let lastConnected = connect.lastConnectedLabel {
                Text(lastConnected)
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
            }

            if let error = connect.lastError {
                Text(error)
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
            }

            HStack(spacing: 12) {
                // NIT (PR #272 review): `connect.connectButtonLabel` reads
                // "RETRY" once the bounded handshake-retry loop has given
                // up (`.failed`) — a visible terminal-state action,
                // rather than a silent re-enable of a button still
                // labeled for a first-time connect.
                Button(connect.connectButtonLabel) { Task { await connect.connect() } }
                    .buttonStyle(.borderedProminent)
                    .tint(.ffAmber)
                    .foregroundStyle(Color.ffBackground)
                    .disabled(connect.isBusyOrConnected)

                // SHOULD-FIX 3 (PR #272 review): gated on
                // `ConnectViewModel.isDisconnectable`, not `link == .ready`
                // — see that property's own doc comment for why a
                // `.connecting`/`.handshaking`/`.reconnecting` link must
                // stay abortable.
                Button("DISCONNECT") { Task { await connect.disconnect() } }
                    .buttonStyle(.bordered)
                    .tint(.ffMuted)
                    .disabled(!connect.isDisconnectable)

                Button("FORGET") { Task { await connect.forgetNode() } }
                    .buttonStyle(.bordered)
                    .tint(.ffAlert)
                    .disabled(!connect.canForgetNode)
            }
            .frame(minHeight: 44)

            // SHOULD-FIX 5 (PR #272 review): plain DISCONNECT deliberately
            // never clears the remembered node — matches Meshtastic-Apple's
            // own `AccessoryManager.disconnect()`, which also never
            // touches `UserDefaults.preferredPeripheralId`. FORGET, above,
            // is the only action that does.
            Text("DISCONNECT keeps this radio remembered for next launch. FORGET clears it.")
                .font(.caption2)
                .foregroundStyle(Color.ffMuted)
        }
    }

    private var statusColor: Color {
        switch connect.link {
        case .ready: return .ffLiveGreen
        case .handshaking, .connecting, .reconnecting: return .ffAmber
        case .failed: return .ffAlert
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
                    Button {
                        // Tapping a row does ONE thing: tell the
                        // transport which peripheral a subsequent
                        // CONNECT should prefer. It deliberately does
                        // not connect — connecting is the CONNECT
                        // button's job, and a picker that silently
                        // starts a connection is a picker that can
                        // start one you did not mean.
                        discovery.select(peripheral.id)
                        selectedPeripheralID = peripheral.id
                    } label: {
                        HStack {
                            Text(peripheral.name ?? peripheral.id)
                                .foregroundStyle(Color.ffInk)
                            if peripheral.id == selectedPeripheralID {
                                Text("SELECTED")
                                    .font(.system(.caption2, design: .monospaced))
                                    .foregroundStyle(Color.ffAmber)
                            }
                            Spacer()
                            Text("\(peripheral.rssiDbm) dBm")
                                .font(.system(.footnote, design: .monospaced))
                                .foregroundStyle(Color.ffMuted)
                        }
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .frame(minHeight: 44)
                }
            }

            Button("RESCAN") { discovery.startScanning() }
                .buttonStyle(.bordered)
                .tint(.ffMuted)
                .frame(minHeight: 44)
        }
    }

    /// `-FireflyAutoConnect <name>` (`FireflyAutoConnectLaunch`'s own doc
    /// comment) — performs EXACTLY the manual UI path a person taking
    /// this screen would: RESCAN, wait for a peripheral whose advertised
    /// name matches, tap it (`discovery.select(_:)`), tap CONNECT
    /// (`connect.connect()`). Never touches `MeshtasticClient`/
    /// `BLETransport` directly — the whole point is to reproduce the
    /// live graph path a real tap takes, not a shortcut around it. A
    /// no-op on every ordinary launch (`requestedPeripheralName()`
    /// reads nil) and on any build with no scanner (`discovery` is
    /// `StubPeripheralDiscovery`, which discovers nothing — this loop
    /// then simply waits forever off its own `.task`, harmlessly, same
    /// as an ungranted permission would).
    private func runAutoConnectIfRequested() async {
        guard let targetName = FireflyAutoConnectLaunch.requestedPeripheralName() else { return }
        discovery.startScanning()
        for await found in discovery.peripherals() {
            guard let match = found.first(where: { $0.name == targetName }) else { continue }
            discovery.select(match.id)
            selectedPeripheralID = match.id
            await connect.connect()
            return
        }
    }

    // MARK: - Nearby heard nodes (Add / Remove from crew)

    private var nearbySection: some View {
        SectionBlock(title: "NEARBY") {
            if nearby.nodes.isEmpty {
                Text("Nobody heard yet. This fills in once the client reports mesh traffic — " +
                     "never before.")
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
            } else {
                // Paired crew first (their own colour + presence), then
                // strangers by signal tier — `NearbyNodesViewModel
                // .rebuild()`'s own ordering, rendered verbatim.
                ForEach(nearby.nodes) { node in
                    NearbyRow(node: node, colorblind: colorblind) {
                        if node.isCrew {
                            nearby.removeFromCrew(node.id)
                        } else {
                            nearby.addToCrew(node.id)
                        }
                    }
                }
            }
            // The honest 8-limit message (M2's own acceptance
            // criterion) — never a silently-ignored tap.
            if let limitMessage = nearby.limitMessage {
                Text(limitMessage)
                    .font(.footnote)
                    .foregroundStyle(Color.ffAlert)
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
                    .foregroundStyle(Color.ffAlert)
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
                    if let planError = channelImport.planErrorMessage {
                        Text(planError)
                            .font(.footnote)
                            .foregroundStyle(Color.ffAlert)
                    }
                    Text("Shown only — not sent to the node until you confirm exactly which " +
                         "slots will be written, disabled, or left untouched.")
                        .font(.caption2)
                        .foregroundStyle(Color.ffMuted)
                    // M3 — the write path has landed. BLOCKING 1 & 2 (PR
                    // #274 review): tapping this first reads the node's
                    // CURRENT channel table and builds the exact write
                    // plan (`preparePlan()`) — the confirmation sheet
                    // only opens once that plan exists, so it can never
                    // show a placeholder for a write that might not be
                    // possible (e.g. an "add" import with no free slot).
                    Button(channelImport.isPreparingPlan ? "CHECKING NODE…" : "APPLY TO NODE") {
                        Task {
                            if await channelImport.preparePlan() {
                                isShowingApplyConfirmation = true
                            }
                        }
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.ffAmber)
                    .foregroundStyle(Color.ffBackground)
                    .disabled(connect.link != .ready || channelImport.isPreparingPlan)
                    .frame(minHeight: 44)
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

/// One Nearby row: a paired member (colour swatch + presence tag) or a
/// stranger (signal bars only) — `NearbyNodesViewModel.NearbyNode`'s own
/// two shapes, rendered verbatim rather than re-derived here.
private struct NearbyRow: View {
    let node: NearbyNodesViewModel.NearbyNode
    let colorblind: Bool
    let onToggle: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            if let colorIndex = node.colorIndex {
                Circle()
                    .fill(Color(fireflyHex: RadarCrewPalette.hex(index: colorIndex, colorblind: colorblind)))
                    .frame(width: 14, height: 14)
            } else {
                SignalBars(tier: node.tier)
            }
            VStack(alignment: .leading, spacing: 2) {
                Text(node.displayName)
                    .foregroundStyle(Color.ffInk)
                if let presence = node.presence {
                    Text(presence.rawValue)
                        .font(.caption2)
                        .foregroundStyle(Color.ffMuted)
                } else {
                    Text(node.tier.label)
                        .font(.caption2)
                        .foregroundStyle(Color.ffMuted)
                }
            }
            Spacer()
            Button(node.isCrew ? "REMOVE" : "ADD TO CREW", action: onToggle)
                .buttonStyle(.bordered)
                .tint(node.isCrew ? .ffLiveGreen : .ffAmber)
                .font(.caption)
        }
        .frame(minHeight: 44)
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
