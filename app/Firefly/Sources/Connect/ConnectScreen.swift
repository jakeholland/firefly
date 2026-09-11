//
//  ConnectScreen.swift — the Connect destination (docs/specs/
//  A01-companion-app.md, M1: "node picker (BLE on both platforms),
//  connection state including a distinct HANDSHAKING, channel import").
//
//  Sections, top to bottom: connection state (naming WHICH radio, not
//  just whether one is up), the NEARBY RADIOS picker with a per-row
//  CONNECT/DISCONNECT/FORGET, Nearby heard nodes ranked by signal tier
//  with Add to Crew, and channel import (paste everywhere, scan on
//  iOS).
//
//  Redesigned on owner feedback from the first real-radio run on the
//  iPhone (verbatim): "iPhone shows 'connected' but I'm not sure to
//  which radio, the UX is confusing, the connect button needs to be on
//  the line item or something, screen needs a little work." Two
//  changes answer that: `ConnectViewModel.headerStatusText` names the
//  radio (BLE name · node long name · node id · RSSI) instead of a bare
//  CONNECTED, and every NEARBY RADIOS row (`RadioListBuilder`,
//  `NearbyNodesViewModel.swift`) carries its own CONNECT/DISCONNECT
//  rather than three top-level buttons nobody could tie to a row.
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
    @State private var channelURLText = ""
    @State private var isShowingScanner = false
    /// NEARBY RADIOS empty/scanning copy (owner feedback item 3):
    /// "Scanning…" with a spinner while a scan is running and nothing
    /// has turned up yet, "No Meshtastic radios found…" once it has run
    /// long enough that the honest answer is "nothing's here", and a
    /// plain "tap RESCAN" prompt before the user has ever asked at all
    /// — RESCAN stays the trigger (`onAppear`'s own comment, below),
    /// this only changes what the empty state SAYS while waiting.
    @State private var isScanning = false
    @State private var scanDidTimeOut = false
    @State private var scanTimeoutTask: Task<Void, Never>?
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

    /// How long a RESCAN is given before an empty NEARBY RADIOS list
    /// switches from "Scanning…" to the honest "nothing found" copy.
    /// Not a protocol timeout of any kind — purely how long this screen
    /// waits before saying so.
    static let scanEmptyTimeout: Duration = .seconds(8)

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
            // M3: one identifying accessibility identifier per screen —
            // `FireflyUITests`' smoke test asserts each destination it
            // navigates to actually rendered, rather than merely not
            // crashing.
            .accessibilityIdentifier("Screen.Connect")
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
            scanTimeoutTask?.cancel()
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
    //
    // Owner feedback item 1: the state line now NAMES the radio —
    // `connect.headerStatusText` (built in the view model, MVVM
    // convention 5) rather than a bare CONNECTED this view would have
    // to re-derive. No CONNECT/DISCONNECT/FORGET here any more — those
    // moved onto their radio's own row in NEARBY RADIOS, below, per
    // item 2 ("the connect button needs to be on the line item").

    private var connectionSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .top, spacing: 8) {
                Circle()
                    .fill(statusColor)
                    .frame(width: 10, height: 10)
                    .padding(.top, 5)
                Text(connect.headerStatusText)
                    .font(.system(.headline, design: .monospaced))
                    .foregroundStyle(statusColor)
                    .fixedSize(horizontal: false, vertical: true)
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
    //
    // Owner feedback item 2: every row carries its own primary action —
    // CONNECT on a row that isn't the active radio, DISCONNECT on the
    // one that is — built by `RadioListBuilder.rows(discovered:connect:)`
    // (`NearbyNodesViewModel.swift`) so the gating itself lives in a
    // view model, not here. Only RESCAN stays a top-level button.

    private var nodePickerSection: some View {
        SectionBlock(title: "NEARBY RADIOS") {
            let rows = RadioListBuilder.rows(discovered: peripherals, connect: connect)
            if rows.isEmpty {
                radioListEmptyState
            } else {
                ForEach(rows) { row in
                    RadioRow(row: row, primaryLabel: primaryLabel(for: row),
                             onPrimary: { performPrimaryAction(for: row) },
                             onForget: { Task { await connect.forgetNode() } })
                }
            }

            Button("RESCAN") { startScan() }
                .buttonStyle(.bordered)
                .tint(.ffMuted)
                .frame(minHeight: 44)

            Text("DISCONNECT keeps a radio remembered for next launch. FORGET clears it.")
                .font(.caption2)
                .foregroundStyle(Color.ffMuted)
        }
    }

    @ViewBuilder
    private var radioListEmptyState: some View {
        if isScanning && !scanDidTimeOut {
            HStack(spacing: 8) {
                ProgressView()
                Text("Scanning…")
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
            }
            .frame(minHeight: 44, alignment: .leading)
        } else if scanDidTimeOut {
            Text("No Meshtastic radios found — is the node powered on and within range?")
                .font(.footnote)
                .foregroundStyle(Color.ffMuted)
        } else {
            Text("Tap RESCAN to look for nearby Meshtastic radios.")
                .font(.footnote)
                .foregroundStyle(Color.ffMuted)
        }
    }

    private func startScan() {
        isScanning = true
        scanDidTimeOut = false
        discovery.startScanning()
        scanTimeoutTask?.cancel()
        scanTimeoutTask = Task {
            try? await Task.sleep(for: Self.scanEmptyTimeout)
            guard !Task.isCancelled else { return }
            scanDidTimeOut = true
        }
    }

    /// RETRY only on the radio that just failed — every other row with
    /// a `.connect` action (a stranger in the scan, or the active row
    /// before any attempt at all) reads plain CONNECT.
    /// `ConnectViewModel.connectButtonLabel`'s own RETRY rule, applied
    /// per row instead of to one shared button.
    private func primaryLabel(for row: RadioListRow) -> String {
        switch row.action {
        case .connect:
            if row.isRemembered, connect.connectButtonLabel == "RETRY" { return "RETRY" }
            return "CONNECT"
        case .disconnect: return "DISCONNECT"
        case .unavailable: return "CONNECT"
        }
    }

    private func performPrimaryAction(for row: RadioListRow) {
        switch row.action {
        case .connect:
            // `discovery.select` points the transport at this specific
            // peripheral BEFORE `connect()` — the whole reason the
            // picker exists (`discovery.select`'s own doc comment): a
            // bare `connect()` takes whatever the transport's internal
            // scan sees first. `noteSelectedPeripheral` tells the view
            // model the BLE name/RSSI this row already knows, so the
            // header can name the radio through CONNECTING/HANDSHAKING
            // rather than only once `.ready` (owner feedback item 1).
            let match = peripherals.first(where: { $0.id == row.id })
            discovery.select(row.id)
            connect.noteSelectedPeripheral(name: match?.name, rssiDbm: match?.rssiDbm)
            Task { await connect.connect() }
        case .disconnect:
            Task { await connect.disconnect() }
        case .unavailable:
            break // dead chrome on purpose — see `ConnectViewModel.rowAction`'s doc comment
        }
    }

    /// `-FireflyAutoConnect <name>` (`FireflyAutoConnectLaunch`'s own doc
    /// comment) — performs EXACTLY the manual UI path a person taking
    /// this screen would: RESCAN, wait for a peripheral whose advertised
    /// name matches, tap it (`discovery.select(_:)` +
    /// `connect.noteSelectedPeripheral(name:rssiDbm:)` — the same pair
    /// a row's own CONNECT action performs, `performPrimaryAction(for:)`
    /// above), tap CONNECT (`connect.connect()`). Never touches
    /// `MeshtasticClient`/`BLETransport` directly — the whole point is
    /// to reproduce the live graph path a real tap takes, not a
    /// shortcut around it. A no-op on every ordinary launch
    /// (`requestedPeripheralName()` reads nil) and on any build with no
    /// scanner (`discovery` is `StubPeripheralDiscovery`, which
    /// discovers nothing — this loop then simply waits forever off its
    /// own `.task`, harmlessly, same as an ungranted permission would).
    private func runAutoConnectIfRequested() async {
        guard let targetName = FireflyAutoConnectLaunch.requestedPeripheralName() else { return }
        discovery.startScanning()
        for await found in discovery.peripherals() {
            guard let match = found.first(where: { $0.name == targetName }) else { continue }
            discovery.select(match.id)
            connect.noteSelectedPeripheral(name: match.name, rssiDbm: match.rssiDbm)
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

/// One NEARBY RADIOS row — `RadioListRow`'s (`NearbyNodesViewModel.swift`)
/// display state rendered verbatim, with its own CONNECT/DISCONNECT and,
/// for the remembered radio, FORGET (owner feedback item 2: "the connect
/// button needs to be on the line item"). The connected row is visually
/// distinct — a theme-amber border plus a CONNECTED chip — so "which
/// radio am I on" reads at a glance, matching the design canvas's node
/// cards (docs/specs/A01-companion-app.md, design language).
private struct RadioRow: View {
    let row: RadioListRow
    let primaryLabel: String
    let onPrimary: () -> Void
    let onForget: () -> Void

    // A two-line card, not one crowded HStack: title/chip/RSSI on top,
    // actions on their own row underneath. A single row ran out of
    // width on a phone the moment a chip AND a 44pt DISCONNECT (and,
    // for the remembered radio, FORGET too) all needed to fit beside
    // the name — SwiftUI's answer to that was wrapping the chip's own
    // text mid-word ("CONNECT-ED", caught in the demo-mode screenshot).
    // Giving actions their own row is honest about how much a phone
    // screen actually has, not a squeeze that only ever looked right on
    // a Mac.
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Text(row.title)
                    .font(.system(.body, design: .monospaced))
                    .foregroundStyle(Color.ffInk)
                    .lineLimit(1)
                    .layoutPriority(1)
                if let chipText = row.status.chipText {
                    RadioChip(text: chipText, color: row.status == .connected ? .ffAmber : .ffMuted)
                        .fixedSize()
                }
                Spacer(minLength: 4)
                if let rssi = row.rssiDbm {
                    Text("\(rssi) dBm")
                        .font(.system(.footnote, design: .monospaced))
                        .foregroundStyle(Color.ffMuted)
                }
            }
            if let subtitle = row.subtitle {
                Text(subtitle)
                    .font(.caption2)
                    .foregroundStyle(Color.ffMuted)
                    .lineLimit(1)
            }
            HStack(spacing: 12) {
                Spacer(minLength: 0)
                if row.isRemembered {
                    Button("FORGET", action: onForget)
                        .buttonStyle(.bordered)
                        .tint(.ffAlert)
                        .font(.caption)
                        .frame(minHeight: 44)
                }
                primaryButton
            }
        }
        .padding(row.status.isHighlighted ? 10 : 0)
        .background {
            if row.status.isHighlighted {
                RoundedRectangle(cornerRadius: 10)
                    .stroke(Color.ffAmber, lineWidth: 2)
            }
        }
    }

    @ViewBuilder
    private var primaryButton: some View {
        switch row.action {
        case .connect:
            Button(primaryLabel, action: onPrimary)
                .buttonStyle(.borderedProminent)
                .tint(.ffAmber)
                .foregroundStyle(Color.ffBackground)
                .frame(minHeight: 44)
        case .disconnect:
            Button(primaryLabel, action: onPrimary)
                .buttonStyle(.bordered)
                .tint(.ffMuted)
                .frame(minHeight: 44)
        case .unavailable:
            Button(primaryLabel) {}
                .buttonStyle(.bordered)
                .tint(.ffMuted)
                .disabled(true)
                .frame(minHeight: 44)
        }
    }
}

/// A small pill label — CONNECTED/REMEMBERED — matching `DemoBadge`'s
/// own monospaced-caption2-bold vocabulary rather than a platform
/// default badge.
private struct RadioChip: View {
    let text: String
    let color: Color

    var body: some View {
        Text(text)
            .font(.system(.caption2, design: .monospaced).weight(.bold))
            .tracking(0.5)
            .foregroundStyle(color)
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .overlay(RoundedRectangle(cornerRadius: 4).stroke(color, lineWidth: 1))
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
                // Paired: the HEARD-presence tag (`ff_crew`'s own
                // freshness buckets). Stranger: the honest "how long
                // ago" text (finding 1) — never the tier label alone,
                // which for a want_config-replayed stranger is almost
                // always NONE and would say nothing.
                if let presence = node.presence {
                    Text(presence.rawValue)
                        .font(.caption2)
                        .foregroundStyle(Color.ffMuted)
                } else if let heardAgo = node.heardAgo {
                    Text(heardAgo)
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
