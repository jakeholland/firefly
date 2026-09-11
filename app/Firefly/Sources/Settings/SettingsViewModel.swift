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
    private var linkObservation: Task<Void, Never>?

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
    init(store: any FireflyExtraSettingsStoring, channelImport: ChannelImportViewModel,
         client: any MeshtasticClientProtocol = StubMeshtasticClient()) {
        self.store = store
        self.channelImport = channelImport
        self.client = client
        nodeLongName = store.nodeLongNamePreference ?? ""
        nodeShortName = store.nodeShortNamePreference ?? ""
        shareGPSWithNode = store.bool(.locationSharingEnabled)
        locationIntervalSeconds = store.double(.locationSharingIntervalSeconds) ?? 30
        stayConnectedInBackground = store.backgroundConnectEnabled
        colorblindPalette = store.colorblindPaletteEnabled
        unitsPreference = store.unitsPreference()
        isConnected = client.connectedNodeNum != nil
    }

    /// No seam exposes the connected node's actual region yet —
    /// `MeshtasticClientProtocol` carries no config/region field in M1.
    /// UNKNOWN is the honest rendering, not a placeholder.
    var region: String { "UNKNOWN" }

    /// UNKNOWN until a channel has actually been imported on the
    /// Connect screen this session — never a guess, and never the
    /// primary channel's name if more than one was in the link.
    var currentChannelName: String {
        guard let first = channelImport.result?.channelSet.settings.first else { return "UNKNOWN" }
        return first.name.isEmpty ? "(default channel)" : first.name
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
    }

    func stopObserving() {
        linkObservation?.cancel()
        linkObservation = nil
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
}
