//
//  SettingsViewModel.swift — the Settings destination's state (docs/
//  specs/A01-companion-app.md, Design language > More/Settings).
//
//  Backed by `AppDependencies.store` — the ONE store the whole app
//  shares. That integration step is done: `.live()` now builds a real
//  `SettingsStore` (`UserDefaults`-backed, so settings survive a
//  relaunch) and hands the same instance to this screen and to the
//  phone-GPS uplink, so `shareGPSWithNode` written here is the exact
//  `SettingsKey.locationSharingEnabled` `PhoneGPSUplinkPolicy.shouldPush`
//  reads on the next fix — not a second store that agrees by
//  `UserDefaults.standard` coincidence.
//
//  The type is `any FireflyExtraSettingsStoring` rather than the
//  concrete `SettingsStore` (MVVM convention #2: "takes its dependencies
//  as protocol existentials in init, and stores no concrete service
//  type"): this screen needs the six shared keys AND its own four from
//  one instance, which is exactly what that refining protocol is.
//
//  M3 update: node-name and region edits get the same confirm-then-write
//  path the Connect screen's channel import does (docs/specs/
//  A01-companion-app.md M3). `nodeLongName`/`nodeShortName` stay exactly
//  what they were in M1 — a LOCAL DRAFT, persisted to `store` on every
//  keystroke, never sent anywhere on their own. `applyNodeName()`/
//  `applyRegion()` are the only things that ever call
//  `client.setOwner`/`client.setRegion`, and only when the Settings
//  screen's confirmation sheet calls them.
//
import FireflyMesh
import FireflyModel
import Foundation
import MeshtasticProto
import Observation

@MainActor
@Observable
final class SettingsViewModel {
    private let store: any FireflyExtraSettingsStoring
    /// The SAME instance the Connect screen imports channels into
    /// (`FireflyApp.swift` constructs one and hands it to both) — so
    /// this screen's "Channel" row reads exactly what Connect showed,
    /// never a second, possibly-stale copy.
    private let channelImport: ChannelImportViewModel
    /// M3 — the seam `applyNodeName()`/`applyRegion()` write through,
    /// and what `isConnected` tracks so the confirm-then-write buttons
    /// are honestly disabled rather than optimistically enabled with no
    /// node to write to.
    private let client: any MeshtasticClientProtocol
    /// M3 — reaches `AppGraph.inboxProvider.clearAll()` (both the live
    /// ring AND `HistoryStore` together — `PersistingInboxProvider
    /// .clearAll()`'s own doc comment) without this view model needing
    /// to depend on `FireflyModel`'s `InboxProviding`/`AppGraph` at all;
    /// same closure-seam convention `ThreadViewModel.currentFix`/
    /// `AppGraph`'s own `destinationNodeNum` already use. Defaulted to a
    /// no-op so every pre-M3 call site (`SettingsViewModel(store:
    /// channelImport:)` in tests) keeps compiling unchanged.
    private let clearHistory: () -> Void
    /// Finding 3 (first real-radio session, macOS): the seam
    /// `setShareGPSWithNode(true)` requests authorization through — the
    /// SAME instance `RadarViewModel`/`PhoneGPSUplink` hold
    /// (`AppDependencies.location`), never a second `CLLocationManager`.
    private let location: any LocationProviding
    private var linkObservation: Task<Void, Never>?
    /// Finding 2 (first real-radio session): the passive read seam —
    /// `client.nodeConfigUpdates()`'s own `CurrentValueEventHub` replay
    /// means this observation sees the current values immediately, even
    /// if want_config finished before this screen was ever opened.
    private var nodeConfigObservation: Task<Void, Never>?
    private(set) var nodeConfig: NodeConfigSnapshot?

    var nodeLongName: String
    var nodeShortName: String
    var shareGPSWithNode: Bool
    var locationIntervalSeconds: Double
    var stayConnectedInBackground: Bool
    var colorblindPalette: Bool
    /// M2: `.system` (the default) follows the phone's own locale —
    /// see `UnitsPreference.resolvedImperial(locale:)` — until this row
    /// overrides it. Read/write through `SettingsStoring`'s own tri-state
    /// helpers, not `.bool(.unitsMetric)` (that key's bool shape is the
    /// exact bug this replaces; see `SettingsStoring.swift`).
    var unitsPreference: UnitsPreference

    /// M3 — `true` once the link has reached `.ready` at least once
    /// since `observe()` started (or, before `observe()` is ever
    /// called, if the client already reports a connected node). Gates
    /// the two confirm-then-write buttons in Settings.
    private(set) var isConnected: Bool
    private(set) var isApplyingName = false
    private(set) var nameApplyError: String?
    /// The region picker's OWN staged selection — never applied until
    /// `applyRegion()` runs, and defaulting to `.unset` rather than
    /// guessing `.us`: this project never asserts a fact about the
    /// node it has not actually read (the same rule `region` above,
    /// "UNKNOWN, never a placeholder", already follows).
    var regionSelection: Config.LoRaConfig.RegionCode = .unset
    private(set) var isApplyingRegion = false
    private(set) var regionApplyError: String?

    /// Defaulted so every existing call site (`SettingsViewModel(store:
    /// channelImport:)` in tests predating M3) keeps compiling — same
    /// convention `ChannelImportViewModel.init`'s own comment cites.
    /// `location` defaults to `UnavailableLocationProvider()` for the
    /// same reason: every pre-finding-3 test call site that never
    /// mentioned location gets the honest "permanently unavailable"
    /// double, never a real `CLLocationManager` it didn't ask for.
    init(store: any FireflyExtraSettingsStoring, channelImport: ChannelImportViewModel,
         client: any MeshtasticClientProtocol = StubMeshtasticClient(), clearHistory: @escaping () -> Void = {},
         location: any LocationProviding = UnavailableLocationProvider()) {
        self.store = store
        self.channelImport = channelImport
        self.client = client
        self.clearHistory = clearHistory
        self.location = location
        nodeLongName = store.nodeLongNamePreference ?? ""
        nodeShortName = store.nodeShortNamePreference ?? ""
        shareGPSWithNode = store.bool(.locationSharingEnabled)
        locationIntervalSeconds = store.double(.locationSharingIntervalSeconds) ?? 30
        stayConnectedInBackground = store.backgroundConnectEnabled
        colorblindPalette = store.colorblindPaletteEnabled
        unitsPreference = store.unitsPreference()
        isConnected = client.connectedNodeNum != nil
        // Finding 2: a synchronous initial read, same convention as
        // `isConnected` just above (`client.connectedNodeNum` — not a
        // stream wait) — a Settings screen opened AFTER want_config
        // already completed must not sit on a placeholder until the
        // NEXT config change, which might never come.
        nodeConfig = client.connectedNodeConfig
        if nodeLongName.isEmpty, let ownerLongName = nodeConfig?.ownerLongName {
            nodeLongName = ownerLongName
        }
        if nodeShortName.isEmpty, let ownerShortName = nodeConfig?.ownerShortName {
            nodeShortName = ownerShortName
        }
    }

    /// The composition root's own constructor — `FireflyApp.init()` calls
    /// this instead of `SettingsViewModel(store:channelImport:client:)`
    /// directly. Mirrors `AppGraph.makeConnectViewModel()`'s fix for the
    /// exact same NavigationSplitView detail-column remount hazard
    /// (`SettingsScreen` is one of `RootView`'s own `detail(for:)`
    /// destinations, same as Connect was): `observe()` starts HERE,
    /// once, for the life of the process, rather than being left to
    /// `SettingsScreen`'s own `.onAppear`/`.onDisappear` to establish or
    /// tear down. This type cannot live on `AppGraph` itself —
    /// `ChannelImportViewModel` is app-target Swift, not `FireflyKit`,
    /// and `AppGraph` cannot depend on the app target — so this static
    /// factory is the composition root for this one singleton instead,
    /// the same role `AppGraph.make*ViewModel()` plays for everything
    /// `FireflyKit` can construct on its own.
    static func makeObserving(store: any FireflyExtraSettingsStoring, channelImport: ChannelImportViewModel,
                               client: any MeshtasticClientProtocol,
                               clearHistory: @escaping () -> Void = {},
                               location: any LocationProviding = UnavailableLocationProvider()) -> SettingsViewModel {
        let model = SettingsViewModel(store: store, channelImport: channelImport, client: client,
                                       clearHistory: clearHistory, location: location)
        model.observe()
        return model
    }

    /// Finding 2 (first real-radio session): read from the client's own
    /// passive config snapshot — `nodeConfig.region`, filled in by
    /// want_config and refreshed after `applyRegion()`'s own read-back —
    /// not a second source of truth. UNKNOWN only for as long as no
    /// handshake has reported one yet, never a permanent placeholder.
    var region: String {
        guard let region = nodeConfig?.region else { return "UNKNOWN" }
        return String(describing: region).uppercased()
    }

    /// Finding 2: the node's own PRIMARY channel, read passively off
    /// `nodeConfig` — falls back to a channel actually imported on the
    /// Connect screen this session (the OLD-and-still-valid M1 seam,
    /// for a node this client has never been connected to), and only
    /// then to UNKNOWN. Never a guess either way.
    var currentChannelName: String {
        if let primary = nodeConfig?.primaryChannelName {
            return primary.isEmpty ? "(default channel)" : primary
        }
        guard let first = channelImport.result?.channelSet.settings.first else { return "UNKNOWN" }
        return first.name.isEmpty ? "(default channel)" : first.name
    }

    /// Finding 2: "from node" is true once `nodeConfig` has reported
    /// ANY field — the Settings screen uses this to label the CHANNEL
    /// block's source honestly rather than implying every row it shows
    /// came from the same place.
    var nodeConfigSourceLabel: String? {
        nodeConfig != nil ? "from node" : nil
    }

    /// NIT (PR #282 review): the NODE NAME block's own version of
    /// `nodeConfigSourceLabel` just above — the Long/Short name fields
    /// pre-fill from the node's own owner name (`applyNodeConfig`'s own
    /// doc comment) exactly the same way the Region/Channel rows
    /// pre-fill from `nodeConfig`, but had no matching label, leaving a
    /// user who typed nothing with no way to tell a pre-filled name
    /// came from the radio rather than a stored local draft. Each field
    /// reads its OWN prefill condition — the exact one
    /// `applyNodeConfig`/`init` gate the prefill itself on — since a
    /// user may have drafted one field but not the other.
    var nodeLongNameSourceLabel: String? {
        store.nodeLongNamePreference == nil && nodeConfig?.ownerLongName != nil ? "from node" : nil
    }
    var nodeShortNameSourceLabel: String? {
        store.nodeShortNamePreference == nil && nodeConfig?.ownerShortName != nil ? "from node" : nil
    }

    func setNodeLongName(_ value: String) {
        nodeLongName = value
        store.nodeLongNamePreference = value.isEmpty ? nil : value
    }

    func setNodeShortName(_ value: String) {
        nodeShortName = value
        store.nodeShortNamePreference = value.isEmpty ? nil : value
    }

    func setShareGPSWithNode(_ value: Bool) {
        shareGPSWithNode = value
        store.setBool(value, .locationSharingEnabled)
        // Finding 3 (first real-radio session, macOS): turning this ON
        // is exactly the moment the user expects a location-permission
        // prompt — nothing in this app ever asked before this finding.
        // Only while genuinely undecided, same guard `RadarViewModel
        // .observe()` uses: a user who already denied or granted it is
        // never re-prompted just for toggling this again.
        if value, location.authorization == .notDetermined {
            let location = self.location
            Task { await location.requestWhenInUseAuthorization() }
        }
    }

    /// Floored at 5 s per the spec's "Phone GPS -> node" cadence rule.
    func setLocationIntervalSeconds(_ value: Double) {
        let clamped = max(5, value)
        locationIntervalSeconds = clamped
        store.setDouble(clamped, .locationSharingIntervalSeconds)
    }

    func setStayConnectedInBackground(_ value: Bool) {
        stayConnectedInBackground = value
        store.backgroundConnectEnabled = value
    }

    func setColorblindPalette(_ value: Bool) {
        colorblindPalette = value
        store.colorblindPaletteEnabled = value
    }

    func setUnitsPreference(_ value: UnitsPreference) {
        unitsPreference = value
        store.setUnitsPreference(value)
    }

    // MARK: - M3: confirm-then-write

    /// Idempotent, same convention every other `observe()` in this app
    /// follows (`ConnectViewModel`, `DiagnosticsViewModel`).
    func observe() {
        guard linkObservation == nil else { return }
        let stream = client.linkState()
        linkObservation = Task { [weak self] in
            for await state in stream {
                self?.isConnected = (state == .ready)
            }
        }
        // Finding 2 — the passive read seam. `nodeConfigUpdates()`'s
        // `CurrentValueEventHub` replay means this sees whatever
        // want_config already reported, immediately, even if it
        // finished before `observe()` was ever called (same ordering
        // rule every other `observe()` in this app follows: subscribe
        // BEFORE anything can be missed, S1).
        let configs = client.nodeConfigUpdates()
        nodeConfigObservation = Task { [weak self] in
            for await snapshot in configs {
                self?.applyNodeConfig(snapshot)
            }
        }
    }

    func stopObserving() {
        linkObservation?.cancel()
        linkObservation = nil
        nodeConfigObservation?.cancel()
        nodeConfigObservation = nil
    }

    /// Merges in a fresh config snapshot and, only while the user has
    /// never typed a LOCAL draft of their own (`store.nodeLongNamePreference`/
    /// `nodeShortNamePreference` still nil), pre-fills the name fields
    /// from the node's own owner — finding 2's "the name fields pre-fill
    /// from the node's owner." The moment the user types anything,
    /// `setNodeLongName`/`setNodeShortName` persist a real local draft
    /// and this stops overriding it, on any later refresh.
    private func applyNodeConfig(_ snapshot: NodeConfigSnapshot) {
        nodeConfig = snapshot
        if store.nodeLongNamePreference == nil, let ownerLongName = snapshot.ownerLongName {
            nodeLongName = ownerLongName
        }
        if store.nodeShortNamePreference == nil, let ownerShortName = snapshot.ownerShortName {
            nodeShortName = ownerShortName
        }
    }

    /// Everything the name-change confirmation sheet shows.
    var nameApplySummary: [String] {
        ["Long name -> \"\(nodeLongName)\"", "Short name -> \"\(nodeShortName)\""]
    }

    /// Behind explicit confirmation only. `nodeLongName`/`nodeShortName`
    /// are already the staged draft this writes — nothing else about
    /// this call reaches further than what the Settings screen already
    /// shows.
    @discardableResult
    func applyNodeName() async -> Bool {
        isApplyingName = true
        nameApplyError = nil
        defer { isApplyingName = false }
        do {
            let report = try await client.setOwner(longName: nodeLongName, shortName: nodeShortName)
            nodeLongName = report.longName
            nodeShortName = report.shortName
            return true
        } catch {
            nameApplyError = ChannelImportViewModel.writeMessage(for: error)
            return false
        }
    }

    /// Everything the region-change confirmation sheet shows.
    var regionApplySummary: [String] {
        ["Region -> \(String(describing: regionSelection).uppercased())"]
    }

    /// Behind explicit confirmation only, and only reachable while
    /// `regionSelection != .unset` (the Settings screen disables the
    /// APPLY button otherwise — an unset region is Meshtastic's own
    /// "radio disabled" state, never something to write on purpose).
    @discardableResult
    func applyRegion() async -> Bool {
        isApplyingRegion = true
        regionApplyError = nil
        defer { isApplyingRegion = false }
        do {
            let report = try await client.setRegion(regionSelection)
            regionSelection = report.region
            return true
        } catch {
            regionApplyError = ChannelImportViewModel.writeMessage(for: error)
            return false
        }
    }

    // MARK: - M3: Clear history

    /// What the "Clear history" confirmation sheet shows — one line,
    /// deliberately blunt: this is the one action in this whole app that
    /// silently, irreversibly discards user data on purpose (`HistoryStore
    /// .makeContainer`'s own doc comment is the OTHER disclosed place
    /// that happens; this is the deliberate one).
    var clearHistorySummary: [String] {
        ["Every saved message, in every thread, deleted from this device."]
    }

    /// No network round trip, no `isBusy`/error state the way the two
    /// admin writes above need — `AppGraph.inboxProvider.clearAll()` is
    /// synchronous and local, so the confirmation sheet's CONFIRM tap
    /// can close immediately.
    func confirmClearHistory() {
        clearHistory()
    }
}
