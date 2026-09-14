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
    /// "app: festpack from fest-almanac + Lineup" — shared with the
    /// Lineup destination (`RootView`'s own `lineup` property), same
    /// "one instance, two screens" pattern `channelImport` already uses
    /// between Connect and Settings, so the "Festival data" row and the
    /// Lineup tab can never show two different sourceState/URL answers.
    @Bindable var lineup: LineupViewModel
    @State private var festpackURLDraft: String = ""
    /// M2's own state: the Crew section's rows (rename/remove), backed
    /// by the SAME `CrewPairingController` Connect's Nearby section
    /// writes through.
    @State private var crewSettings: CrewSettingsViewModel
    /// Demo-only (`-FireflyDemoScreen diagnostics`, `RootView`'s own
    /// mapping): pushes straight to Diagnostics on appear. `false` in
    /// every non-demo build.
    var autoOpenDiagnostics: Bool = false
    @State private var showDiagnostics = false
    /// Owner note (build 304, item 3): pairing itself only happens on
    /// Connect's Nearby section (`NearbyNodesViewModel.addToCrew(_:)`)
    /// — the Crew section here only has rename/remove
    /// (`CrewSettingsViewModel`). This screen's own "ADD CREW" button
    /// (`crewSection`, below) calls this to get the owner there rather
    /// than leaving them to find Connect on their own. Defaults to a
    /// no-op for previews/tests that never wire real navigation.
    var onOpenConnect: () -> Void = {}
    /// M3 — confirm-then-write sheets for the node-name and region edits
    /// (docs/specs/A01-companion-app.md M3), the same pattern Connect's
    /// channel "Apply to node" uses.
    @State private var isShowingNameConfirmation = false
    @State private var isShowingRegionConfirmation = false
    /// M3 — "Clear history" (docs/specs/A01-companion-app.md M3),
    /// behind the same confirm-then-write pattern as the two admin
    /// writes above.
    @State private var isShowingClearHistoryConfirmation = false

    init(model: SettingsViewModel, client: any MeshtasticClientProtocol, pairing: CrewPairingController,
         lineup: LineupViewModel, autoOpenDiagnostics: Bool = false, onOpenConnect: @escaping () -> Void = {},
         // A03 §3.6/§3.10 — appended, so a sibling slice's own hunk in
         // this signature lands as a pure insertion rather than a
         // collision.
         linkDiagnostics: (any BLELinkDiagnosticsProviding)? = nil,
         notifications: (any NotificationSending)? = nil) {
        self.model = model
        self.client = client
        self.lineup = lineup
        self.autoOpenDiagnostics = autoOpenDiagnostics
        self.onOpenConnect = onOpenConnect
        self.linkDiagnostics = linkDiagnostics
        self.notifications = notifications
        _crewSettings = State(initialValue: CrewSettingsViewModel(pairing: pairing))
        _festpackURLDraft = State(initialValue: model.festpackSourceURLOverride ?? "")
    }

    /// A03 §3.6/§3.10 — the transport's reconnect counters and the
    /// notification seam, handed down so Diagnostics can render what is
    /// actually true about the background connection. Both optional and
    /// defaulted: a stack with no radio (or a preview) renders UNKNOWN
    /// rather than zeros.
    var linkDiagnostics: (any BLELinkDiagnosticsProviding)?
    var notifications: (any NotificationSending)?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 28) {
                nodeIdentitySection
                channelSection
                connectivitySection
                unitsSection
                crewSection
                appearanceSection
                festivalDataSection
                historySection
                Button("DIAGNOSTICS") { showDiagnostics = true }
                    .buttonStyle(.bordered)
                    .tint(.ffAmber)
                    .frame(minHeight: 44)
            }
            .padding(20)
        }
        .background(Color.ffBackground)
        .navigationTitle("SETTINGS")
        // M3: one identifying accessibility identifier per screen — see
        // `ConnectScreen`'s own comment.
        .accessibilityIdentifier("Screen.Settings")
        // A real push, through the same `navigationDestination`
        // mechanism the button's tap uses — `autoOpenDiagnostics` just
        // sets the same `@State` a tap would (see `InboxContainerView`
        // .task's own comment for the identical reasoning).
        .navigationDestination(isPresented: $showDiagnostics) {
            DiagnosticsScreen(model: DiagnosticsViewModel(
                client: client, linkDiagnostics: linkDiagnostics, notifications: notifications,
                // Read through the SAME view model the toggle writes, so
                // the status line can never disagree with the switch
                // three rows above it.
                backgroundConnectEnabled: { model.stayConnectedInBackground }))
        }
        .task {
            if autoOpenDiagnostics { showDiagnostics = true }
        }
        // A pairing made on Connect while this screen was off-screen
        // (or a mesh name that has arrived since) must show up the
        // moment More is opened again — `crewSettings` is only ever
        // written from a rename/remove tap otherwise.
        .onAppear { crewSettings.refresh() }
        // M3's own `.onAppear`/`.onDisappear` pair that used to live
        // here — tracking `model.isConnected` for the two confirm-then-
        // write buttons below — is gone. `model.observe()` now starts in
        // `SettingsViewModel.makeObserving(store:channelImport:client:)`,
        // the composition root's own factory (`FireflyApp.init`, since
        // this type lives in the app target, not `FireflyKit`, so
        // `AppGraph` itself cannot construct it — `makeConnectViewModel()`
        // 's own doc comment has the full NavigationSplitView
        // detail-column remount story this screen is exactly as
        // vulnerable to as Connect was: it too is one of `RootView`'s own
        // `detail(for:)` destinations).
        .sheet(isPresented: $isShowingNameConfirmation) {
            // Owner decision, 2026-09-13 ("Settings' name sheet"): the
            // title names what this actually is to the person reading
            // it, and the body is the same blink-off sentence every
            // real radio write shares — no more "node restarts".
            AdminWriteConfirmationSheet(
                title: "Your name \u{00B7} what your crew sees",
                primaryText: AdminWriteCopy.radioBlinksOff,
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
                primaryText: AdminWriteCopy.radioBlinksOff,
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
        // M3 — "Clear history": no network round trip
        // (`SettingsViewModel.confirmClearHistory()`'s own doc comment),
        // so CONFIRM closes the sheet immediately rather than awaiting
        // anything. Never the radio blink-off sentence here — this
        // action never touches the puck at all.
        .sheet(isPresented: $isShowingClearHistoryConfirmation) {
            AdminWriteConfirmationSheet(
                title: "CLEAR HISTORY",
                primaryText: model.clearHistorySummary.first
                    ?? "Every saved message, in every thread, deleted from this device.",
                isBusy: false,
                errorMessage: nil,
                onConfirm: {
                    model.confirmClearHistory()
                    isShowingClearHistoryConfirmation = false
                },
                onCancel: { isShowingClearHistoryConfirmation = false })
        }
    }

    private var nodeIdentitySection: some View {
        SettingsBlock(title: "RADIO NAME") {
            // Finding 2 (first real-radio session): the write path has
            // shipped since M3 (PR #274) — the OLD "out of scope until
            // M3" copy was stale the moment that PR landed. Plain words
            // about what actually happens, not a scope note nobody
            // reading Settings cares about.
            // PR #304 review: this caption still said "radio restarts"
            // while the confirmation sheet one tap away now says the
            // puck blinks off — same event, two vocabularies, on one
            // screen. A02 §6.4's own wording for this row.
            Text("This is what your crew sees for you. Applies after confirmation.")
                .font(.caption2)
                .foregroundStyle(Color.ffCaption)
            LabeledField(label: "Long name") {
                TextField("", text: Binding(get: { model.nodeLongName }, set: { model.setNodeLongName($0) }))
                    .textFieldStyle(.roundedBorder)
            }
            // NIT (PR #282 review): same "from node" label the
            // Region/Channel rows use just below in `channelSection`,
            // shown only while THIS field is still the node's own
            // pre-filled owner name rather than a typed local draft.
            if let source = model.nodeLongNameSourceLabel {
                Text(source)
                    .font(.caption2)
                    .foregroundStyle(Color.ffCaption)
            }
            LabeledField(label: "Short name") {
                TextField("", text: Binding(get: { model.nodeShortName }, set: { model.setNodeShortName($0) }))
                    .textFieldStyle(.roundedBorder)
            }
            if let source = model.nodeShortNameSourceLabel {
                Text(source)
                    .font(.caption2)
                    .foregroundStyle(Color.ffCaption)
            }
            // M3 — the write path has landed; "APPLY NAME TO NODE"
            // reaches it behind a confirmation sheet, disabled whenever
            // there is no connected node.
            Button("APPLY NAME TO RADIO") { isShowingNameConfirmation = true }
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
            // Finding 2: both rows above now read the client's own
            // config snapshot (want_config, refreshed after an admin
            // write's read-back) — "from node" names that source
            // honestly, only once it has actually reported something.
            if let source = model.nodeConfigSourceLabel {
                Text(source)
                    .font(.caption2)
                    .foregroundStyle(Color.ffCaption)
            }
            // A region PICKED here is only a staged selection
            // (`SettingsViewModel.regionSelection`) until "APPLY REGION"
            // is confirmed; it never overrides the "Region" row above.
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
                label: "Share phone GPS with radio",
                isOn: Binding(get: { model.shareGPSWithNode }, set: { model.setShareGPSWithNode($0) }))
            if model.shareGPSWithNode {
                LabeledField(label: "Interval (seconds, minimum 5)") {
                    TextField("30", value: Binding(
                        get: { model.locationIntervalSeconds },
                        set: { model.setLocationIntervalSeconds($0) }), format: .number)
                        .textFieldStyle(.roundedBorder)
                        #if os(iOS)
                        .keyboardType(.numberPad)
                        #endif
                }
            }
            ToggleRow(
                label: "Stay connected in background",
                // A03 §3.3 — this is ON by default now
                // (`SettingsStore.backgroundConnectEnabled`), so the
                // subtitle has to say what each position DOES in plain
                // words rather than assume the reader turned it on
                // deliberately. Both halves are literally true of the
                // code: ON keeps the Bluetooth link up with the screen
                // off and lets the app alert you; OFF disconnects the
                // moment Firefly leaves the foreground, which also means
                // no messages and no flares reach you until you open it
                // again — the consequence a reader most needs and the
                // old copy did not mention.
                subtitle: "On: Firefly keeps talking to your puck while your phone is in your pocket, " +
                    "so messages and flares still reach you. " +
                    "Off: Firefly disconnects when you leave the app, and nothing reaches you until you open it.",
                isOn: Binding(get: { model.stayConnectedInBackground }, set: { model.setStayConnectedInBackground($0) }))
            // A03 §3.10 — the same honest line Diagnostics shows,
            // repeated under the toggle so somebody who just changed it
            // can see what is actually true now.
            BackgroundConnectionRow(model: model, client: client, linkDiagnostics: linkDiagnostics,
                                     notifications: notifications)
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
            Picker("Units", selection: Binding(get: { model.unitsPreference }, set: { model.setUnitsPreference($0) })) {
                ForEach(UnitsPreference.allCases, id: \.self) { preference in
                    Text(preference.settingsLabel).tag(preference)
                }
            }
            .pickerStyle(.segmented)
            .frame(minHeight: 44)
            Text(unitsCaption)
                .font(.caption2)
                .foregroundStyle(Color.ffCaption)
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
                isOn: Binding(get: { model.colorblindPalette }, set: { model.setColorblindPalette($0) }))
        }
    }

    // MARK: - History (M3)

    /// docs/specs/A01-companion-app.md, M3: "a 'Clear history' action in
    /// Settings with confirmation." A destructive-tinted button, plain
    /// text warning — the same bluntness `AdminWriteConfirmationSheet`
    /// itself uses for a write it cannot undo, applied here to a delete
    /// it cannot undo either.
    ///
    /// PR #281 review, SHOULD-FIX 2: a second line discloses the OTHER
    /// way history can disappear — an app update whose on-disk format
    /// changed, or a corrupted store file, clears it automatically, with
    /// no button ever tapped (`HistoryStore.makeContainer`'s
    /// drop-and-recreate fallback; that type's own header comment and
    /// `HistorySchema.swift`'s have the full justification). The spec's
    /// own "disclosed in three places" claim named this screen as one of
    /// them; before this fix the copy below disclosed only that the
    /// MANUAL button is irreversible, never the automatic case.
    private var historySection: some View {
        SettingsBlock(title: "HISTORY") {
            Text("Saved messages live only on this device. Clearing them cannot be undone.")
                .font(.caption2)
                .foregroundStyle(Color.ffCaption)
            Text("If the app's history format changes, old history is cleared automatically.")
                .font(.caption2)
                .foregroundStyle(Color.ffCaption)
            Button("CLEAR HISTORY") { isShowingClearHistoryConfirmation = true }
                .buttonStyle(.bordered)
                .tint(.ffAlert)
                .frame(minHeight: 44)
        }
    }

    // MARK: - Festival data ("app: festpack from fest-almanac + Lineup")

    /// "pack updated `<meta.updated>` · from fest-almanac", honestly
    /// reflecting whichever of no pack / bundled / cached (age) / fresh
    /// `lineup.sourceState` actually is — never a claim this screen
    /// cannot back up. Shares `lineup` with the Lineup tab (this file's
    /// own `lineup` doc comment), so "refresh" here and the Lineup
    /// screen's pull-to-refresh can never disagree about what pack is
    /// loaded.
    private var festivalDataSection: some View {
        SettingsBlock(title: "FESTIVAL DATA") {
            Text(festivalDataStatusText)
                .font(.footnote)
                .foregroundStyle(Color.ffCaption)
            festivalPickerList
            Text("Advanced")
                .font(.caption2.weight(.semibold))
                .foregroundStyle(Color.ffCaption)
                .padding(.top, 4)
            LabeledField(label: "Pack URL (blank = fest-almanac default)") {
                TextField(AlmanacFestpackProvider.defaultURL.absoluteString, text: $festpackURLDraft)
                    .textFieldStyle(.roundedBorder)
                    #if os(iOS)
                    .textInputAutocapitalization(.never)
                    .keyboardType(.URL)
                    #endif
                    .onSubmit { model.setFestpackSourceURLOverride(festpackURLDraft) }
            }
            if let error = model.festpackSourceURLError {
                Text(error)
                    .font(.caption2)
                    .foregroundStyle(Color.ffAlert)
            }
            HStack {
                Button("SAVE URL") { model.setFestpackSourceURLOverride(festpackURLDraft) }
                    .buttonStyle(.bordered)
                Button("REFRESH") { Task { await lineup.refresh() } }
                    .buttonStyle(.bordered)
                    .tint(.ffAmber)
                    .disabled(lineup.isRefreshing)
            }
        }
    }

    private var festivalDataStatusText: String {
        // `FestpackSourceState.statusText` — shared with Lineup's own
        // header so the two cannot drift, and so the honest
        // "cached (age unknown)" case is testable (hardening QA pass).
        let sourceText = lineup.sourceState.statusText
        guard let updated = lineup.festpack?.meta.updated else { return sourceText }
        return "pack updated \(updated) · from fest-almanac · \(sourceText)"
    }

    /// "app: automatic almanac refresh + festival picker" (owner ask
    /// #2) — one row per festival `model.festivalPicker` knows about,
    /// sorted by start date (that view model's own `load()` doc
    /// comment), current one marked, the selected one checked. Loaded
    /// on first appear so opening Settings never shows a stale list
    /// from whenever the app last happened to fetch the index.
    private var festivalPickerList: some View {
        VStack(alignment: .leading, spacing: 6) {
            ForEach(model.festivalPicker.rows) { row in
                Button {
                    Task { await model.festivalPicker.select(row.id) }
                } label: {
                    HStack {
                        VStack(alignment: .leading, spacing: 1) {
                            Text("\(row.name) \(String(row.year))")
                                .font(.footnote.weight(row.isSelected ? .semibold : .regular))
                                .foregroundStyle(Color.ffInk)
                            if row.isCurrent {
                                Text("HAPPENING NOW")
                                    .font(.caption2.weight(.bold))
                                    .foregroundStyle(Color.ffAmber)
                            }
                        }
                        Spacer()
                        if row.isSelected {
                            Image(systemName: "checkmark.circle.fill")
                                .foregroundStyle(Color.ffAmber)
                        }
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .frame(minHeight: 36)
                // Same "OUR identifier, not a localized label" rule the
                // More list's `MoreRow.<name>` rows follow — a festival
                // NAME comes from fest-almanac and can change under us,
                // so the UI smoke test addresses rows by the stable
                // "<slug>-<year>" id instead.
                .accessibilityIdentifier("FestivalRow.\(row.id)")
            }
            if model.festivalPicker.isLoading, model.festivalPicker.rows.isEmpty {
                Text("Loading festivals…")
                    .font(.caption2)
                    .foregroundStyle(Color.ffCaption)
            }
            if let error = model.festivalPicker.loadError {
                Text(error)
                    .font(.caption2)
                    .foregroundStyle(Color.ffAlert)
            }
        }
        .task { await model.festivalPicker.load() }
    }

    // MARK: - Crew (M2)

    private var crewSection: some View {
        SettingsBlock(title: "CREW") {
            if crewSettings.rows.isEmpty {
                Text("Nobody paired yet.")
                    .font(.footnote)
                    .foregroundStyle(Color.ffCaption)
            } else {
                ForEach(crewSettings.rows) { row in
                    CrewMemberRow(
                        row: row,
                        colorblind: model.colorblindPalette,
                        onRename: { crewSettings.rename(row.id, to: $0) },
                        onRemove: { crewSettings.remove(row.id) })
                }
            }
            // Owner note (build 304, item 3): this section could only
            // ever rename/remove — pairing itself lives on Connect's
            // Nearby section (`NearbyNodesViewModel.addToCrew(_:)`), and
            // nothing here used to say so with a real way to get there.
            // Honest about what happens BEFORE the tap, not just after.
            Button {
                onOpenConnect()
            } label: {
                Label("ADD CREW", systemImage: "person.badge.plus")
                    .font(.system(.footnote, design: .rounded).weight(.bold))
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.bordered)
            .tint(.ffAmber)
            .frame(minHeight: 44)
            .accessibilityIdentifier("Settings.AddCrew")
            Text("Pick a nearby radio on Connect \u{2192} Nearby to add it to your crew.")
                .font(.caption2)
                .foregroundStyle(Color.ffCaption)
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
                .foregroundStyle(Color.ffCaption)
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
                .foregroundStyle(Color.ffCaption)
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
                .foregroundStyle(Color.ffCaption)
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
            // Colour + initial, never a bare swatch (owner decision,
            // 2026-09-13: "Nameless crew rows... a colour and initial
            // '?'") — "?" is an honest "unknown" glyph, not a
            // fabricated name, the same fix `InboxAvatar.avatarGlyph`
            // makes for the Inbox row.
            ZStack {
                Circle()
                    .fill(Color(fireflyHex: RadarCrewPalette.hex(index: row.colorIndex, colorblind: colorblind)))
                Text(String(row.initial ?? "?"))
                    .font(.system(.caption2, design: .rounded).weight(.bold))
                    .foregroundStyle(Color.ffBackground)
            }
            .frame(width: 22, height: 22)
            TextField(row.meshName.isEmpty ? row.displayName : row.meshName, text: $nicknameDraft)
                .textFieldStyle(.roundedBorder)
                .onSubmit { onRename(nicknameDraft) }
            Button("REMOVE", action: onRemove)
                .buttonStyle(.bordered)
                .tint(.ffAlert)
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
                    .foregroundStyle(Color.ffCaption)
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

/// A03 §3.10 — the "Background connection" line, repeated under the
/// CONNECTIVITY toggle (it also lives on Diagnostics). One row, in plain
/// language, that states what is actually true right now.
///
/// It owns a `DiagnosticsViewModel` of its own rather than reaching for
/// a shared one: that type IS the live-value reader for exactly these
/// four inputs (link state, the transport's reconnect counters, the
/// notification authorization, the toggle), and it already carries the
/// observe/stopObserving lifecycle a row on a screen that comes and goes
/// needs. Nothing here computes the SENTENCE — that is
/// `BackgroundConnectionStatus`, in FireflyModel, with its own honesty
/// test.
private struct BackgroundConnectionRow: View {
    let model: SettingsViewModel
    let client: any MeshtasticClientProtocol
    let linkDiagnostics: (any BLELinkDiagnosticsProviding)?
    let notifications: (any NotificationSending)?
    @State private var diagnostics: DiagnosticsViewModel?

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("Background connection")
                .font(.caption)
                .foregroundStyle(Color.ffCaption)
            Text(diagnostics?.backgroundConnectionLabel ?? "\u{2014}")
                .font(.footnote)
                .foregroundStyle(Color.ffMuted)
                .fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .onAppear {
            if diagnostics == nil {
                let vm = DiagnosticsViewModel(client: client, linkDiagnostics: linkDiagnostics,
                                               notifications: notifications,
                                               backgroundConnectEnabled: { model.stayConnectedInBackground })
                vm.observe()
                diagnostics = vm
            }
        }
        .onDisappear { diagnostics?.stopObserving() }
    }
}
