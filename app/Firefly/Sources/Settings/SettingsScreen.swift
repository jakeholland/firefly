//
//  SettingsScreen.swift — the Settings destination (docs/specs/
//  A01-companion-app.md, Design language > More/Settings).
//
import FireflyMesh
import FireflyModel
import MeshtasticProto
import SwiftUI

struct SettingsScreen: View {
    @Bindable var model: SettingsViewModel
    let client: any MeshtasticClientProtocol
    /// M2's own state: the Crew section's rows (rename/remove), backed
    /// by the SAME `CrewPairingController` Connect's Nearby section
    /// writes through.
    @State private var crewSettings: CrewSettingsViewModel
    /// Demo-only (`-FireflyDemoScreen diagnostics`, `RootView`'s own
    /// mapping): pushes straight to Diagnostics on appear. `false` in
    /// every non-demo build.
    var autoOpenDiagnostics: Bool = false
    @State private var showDiagnostics = false
    /// M3 — confirm-then-write sheets for the node-name and region edits
    /// (docs/specs/A01-companion-app.md M3), the same pattern Connect's
    /// channel "Apply to node" uses.
    @State private var isShowingNameConfirmation = false
    @State private var isShowingRegionConfirmation = false

    init(model: SettingsViewModel, client: any MeshtasticClientProtocol, pairing: CrewPairingController,
         autoOpenDiagnostics: Bool = false) {
        self.model = model
        self.client = client
        self.autoOpenDiagnostics = autoOpenDiagnostics
        _crewSettings = State(initialValue: CrewSettingsViewModel(pairing: pairing))
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 28) {
                nodeIdentitySection
                channelSection
                connectivitySection
                unitsSection
                crewSection
                appearanceSection
                Button("DIAGNOSTICS") { showDiagnostics = true }
                    .buttonStyle(.bordered)
                    .tint(.ffAmber)
                    .frame(minHeight: 44)
            }
            .padding(20)
        }
        .background(Color.ffBackground)
        .navigationTitle("SETTINGS")
        // A real push, through the same `navigationDestination`
        // mechanism the button's tap uses — `autoOpenDiagnostics` just
        // sets the same `@State` a tap would (see `InboxContainerView`
        // .task's own comment for the identical reasoning).
        .navigationDestination(isPresented: $showDiagnostics) {
            DiagnosticsScreen(model: DiagnosticsViewModel(client: client))
        }
        .task {
            if autoOpenDiagnostics { showDiagnostics = true }
        }
        // A pairing made on Connect while this screen was off-screen
        // (or a mesh name that has arrived since) must show up the
        // moment More is opened again — `crewSettings` is only ever
        // written from a rename/remove tap otherwise.
        .onAppear { crewSettings.refresh() }
        // M3 — a SEPARATE `.onAppear`/`.onDisappear` pair from the one
        // above, tracking `model.isConnected` for the two confirm-then-
        // write buttons below. SwiftUI runs every `.onAppear`/
        // `.onDisappear` attached to a view, not just the first.
        .onAppear { model.observe() }
        .onDisappear { model.stopObserving() }
        .sheet(isPresented: $isShowingNameConfirmation) {
            AdminWriteConfirmationSheet(
                title: "APPLY NAME",
                changes: model.nameApplySummary,
                isBusy: model.isApplyingName,
                errorMessage: model.nameApplyError,
                onConfirm: {
                    Task {
                        if await model.applyNodeName() { isShowingNameConfirmation = false }
                    }
                },
                onCancel: { isShowingNameConfirmation = false })
        }
        .sheet(isPresented: $isShowingRegionConfirmation) {
            AdminWriteConfirmationSheet(
                title: "APPLY REGION",
                changes: model.regionApplySummary,
                isBusy: model.isApplyingRegion,
                errorMessage: model.regionApplyError,
                onConfirm: {
                    Task {
                        if await model.applyRegion() { isShowingRegionConfirmation = false }
                    }
                },
                onCancel: { isShowingRegionConfirmation = false })
        }
    }

    private var nodeIdentitySection: some View {
        SettingsBlock(title: "NODE NAME") {
            Text("A LOCAL DRAFT — not yet sent to the node. Renaming the radio itself is an " +
                 "admin message, out of scope until M3.")
                .font(.caption2)
                .foregroundStyle(Color.ffMuted)
            LabeledField(label: "Long name") {
                TextField("", text: Binding(get: { model.nodeLongName }, set: model.setNodeLongName))
                    .textFieldStyle(.roundedBorder)
            }
            LabeledField(label: "Short name") {
                TextField("", text: Binding(get: { model.nodeShortName }, set: model.setNodeShortName))
                    .textFieldStyle(.roundedBorder)
            }
            // M3 — the write path has landed; "APPLY NAME TO NODE"
            // reaches it behind a confirmation sheet, disabled whenever
            // there is no connected node.
            Button("APPLY NAME TO NODE") { isShowingNameConfirmation = true }
                .buttonStyle(.borderedProminent)
                .tint(.ffAmber)
                .foregroundStyle(Color.ffBackground)
                .disabled(!model.isConnected)
                .frame(minHeight: 44)
        }
    }

    private var channelSection: some View {
        SettingsBlock(title: "CHANNEL") {
            LabeledRow(label: "Region", value: model.region)
            LabeledRow(label: "Channel", value: model.currentChannelName)
            // M3 — a region PICKED here is only a staged selection
            // (`SettingsViewModel.regionSelection`) until "APPLY REGION"
            // is confirmed; it never overrides the "Region" row above,
            // which stays UNKNOWN per that row's own doc comment (no
            // passive read-back seam exists yet).
            Picker("Set region", selection: $model.regionSelection) {
                ForEach(Config.LoRaConfig.RegionCode.allCases, id: \.self) { region in
                    Text(String(describing: region).uppercased()).tag(region)
                }
            }
            .pickerStyle(.menu)
            .frame(minHeight: 44)
            Button("APPLY REGION") { isShowingRegionConfirmation = true }
                .buttonStyle(.borderedProminent)
                .tint(.ffAmber)
                .foregroundStyle(Color.ffBackground)
                .disabled(!model.isConnected || model.regionSelection == .unset)
                .frame(minHeight: 44)
        }
    }

    private var connectivitySection: some View {
        SettingsBlock(title: "CONNECTIVITY") {
            ToggleRow(
                label: "Share phone GPS with node",
                isOn: Binding(get: { model.shareGPSWithNode }, set: model.setShareGPSWithNode))
            if model.shareGPSWithNode {
                LabeledField(label: "Interval (seconds, floor 5)") {
                    TextField("30", value: Binding(
                        get: { model.locationIntervalSeconds },
                        set: model.setLocationIntervalSeconds), format: .number)
                        .textFieldStyle(.roundedBorder)
                        #if os(iOS)
                        .keyboardType(.numberPad)
                        #endif
                }
            }
            ToggleRow(
                label: "Stay connected in background",
                // M2: built. ON keeps the Bluetooth link open — and
                // reconnecting on its own after a pocket loss or a node
                // power cycle — with the screen off
                // (`AppGraph.handleScenePhaseChange`, `BLETransport`'s
                // reconnect-on-loss and CoreBluetooth state
                // restoration). OFF disconnects the moment Firefly
                // leaves the foreground, honestly, rather than quietly
                // keeping a radio open the setting says is off.
                subtitle: "On: the link stays up and reconnects on its own while your phone is in your pocket. " +
                    "Off: Firefly disconnects the moment it leaves the foreground.",
                isOn: Binding(get: { model.stayConnectedInBackground }, set: model.setStayConnectedInBackground))
        }
    }

    /// M2: replaces the M1 gap PR #265's own review flagged — a units
    /// row that actually writes something (`AppGraph
    /// .makeRadarViewModel`'s own comment had nowhere honest to read
    /// from before this existed). `.system` — the default — is spelled
    /// out in the segment label rather than left implicit, since
    /// picking it is itself a meaningful choice ("follow my phone"),
    /// not merely "no choice made yet".
    private var unitsSection: some View {
        SettingsBlock(title: "UNITS") {
            Picker("Units", selection: Binding(get: { model.unitsPreference }, set: model.setUnitsPreference)) {
                ForEach(UnitsPreference.allCases, id: \.self) { preference in
                    Text(preference.settingsLabel).tag(preference)
                }
            }
            .pickerStyle(.segmented)
            .frame(minHeight: 44)
            Text(unitsCaption)
                .font(.caption2)
                .foregroundStyle(Color.ffMuted)
        }
    }

    private var unitsCaption: String {
        switch model.unitsPreference {
        case .system:
            return "Follows this phone's own region for distance — metric or imperial, whichever it uses."
        case .metric:
            return "Distances always show in meters/kilometers, regardless of region."
        case .imperial:
            return "Distances always show in feet/miles, regardless of region."
        }
    }

    private var appearanceSection: some View {
        SettingsBlock(title: "APPEARANCE") {
            ToggleRow(
                label: "Colorblind crew palette",
                isOn: Binding(get: { model.colorblindPalette }, set: model.setColorblindPalette))
        }
    }

    // MARK: - Crew (M2)

    private var crewSection: some View {
        SettingsBlock(title: "CREW") {
            if crewSettings.rows.isEmpty {
                Text("Nobody paired yet. Add crew from Connect \u{2192} Nearby.")
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
            } else {
                ForEach(crewSettings.rows) { row in
                    CrewMemberRow(
                        row: row,
                        colorblind: model.colorblindPalette,
                        onRename: { crewSettings.rename(row.id, to: $0) },
                        onRemove: { crewSettings.remove(row.id) })
                }
            }
        }
    }
}

private struct SettingsBlock<Content: View>: View {
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

private struct LabeledField<Content: View>: View {
    let label: String
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(label)
                .font(.caption)
                .foregroundStyle(Color.ffMuted)
            content
                .frame(minHeight: 44)
        }
    }
}

private struct LabeledRow: View {
    let label: String
    let value: String

    var body: some View {
        HStack {
            Text(label)
                .foregroundStyle(Color.ffMuted)
            Spacer()
            Text(value)
                .font(.system(.body, design: .monospaced))
                .foregroundStyle(Color.ffInk)
        }
        .frame(minHeight: 44)
    }
}

/// One Crew-section row: colour swatch, rename field (a LOCAL DRAFT —
/// see `CrewSettingsViewModel.Row.nickname`'s own doc comment), and
/// REMOVE, which unpairs through the same `CrewPairingController` every
/// other crew action uses.
private struct CrewMemberRow: View {
    let row: CrewSettingsViewModel.Row
    let colorblind: Bool
    let onRename: (String) -> Void
    let onRemove: () -> Void

    // Seeded once from `row.nickname` in `init`, NOT re-synced on every
    // `.onAppear` (PR #270 review NIT): `.onAppear` can fire again for
    // an already-created row (e.g. a `refresh()` that re-renders this
    // list while a rename is mid-edit), and resetting the draft there
    // would discard whatever the user was typing. `@State`'s own
    // initial-value semantics already give the right behavior for
    // free — it's read once per view identity (`ForEach` keys rows by
    // `nodeID`) — so this only has to stop overriding it a second time.
    @State private var nicknameDraft: String

    init(row: CrewSettingsViewModel.Row, colorblind: Bool, onRename: @escaping (String) -> Void, onRemove: @escaping () -> Void) {
        self.row = row
        self.colorblind = colorblind
        self.onRename = onRename
        self.onRemove = onRemove
        self._nicknameDraft = State(initialValue: row.nickname ?? "")
    }

    var body: some View {
        HStack(spacing: 12) {
            Circle()
                .fill(Color(fireflyHex: RadarCrewPalette.hex(index: row.colorIndex, colorblind: colorblind)))
                .frame(width: 16, height: 16)
            TextField(row.meshName.isEmpty ? row.displayName : row.meshName, text: $nicknameDraft)
                .textFieldStyle(.roundedBorder)
                .onSubmit { onRename(nicknameDraft) }
            Button("REMOVE", action: onRemove)
                .buttonStyle(.bordered)
                .tint(.ffMuted)
                .font(.caption)
        }
        .frame(minHeight: 44)
    }
}

private struct ToggleRow: View {
    let label: String
    /// Optional — most toggles are self-explanatory; this exists for
    /// the ones that aren't, so the honest caveat lives next to the
    /// control it qualifies rather than in a doc comment nobody
    /// browsing Settings will ever read (PR #265 review, should-fix:
    /// "stay connected in background").
    var subtitle: String?
    @Binding var isOn: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Toggle(label, isOn: $isOn)
                .tint(.ffAmber)
                .foregroundStyle(Color.ffInk)
                .frame(minHeight: 44)
            if let subtitle {
                Text(subtitle)
                    .font(.caption2)
                    .foregroundStyle(Color.ffMuted)
            }
        }
    }
}

/// UI-only labels — kept here rather than on `UnitsPreference` itself
/// (`FireflyModel`, no SwiftUI/display-string concerns of its own) so
/// the model stays a plain, presentation-agnostic tri-state.
private extension UnitsPreference {
    var settingsLabel: String {
        switch self {
        case .system: return "System"
        case .metric: return "Metric"
        case .imperial: return "Imperial"
        }
    }
}
