//
//  SettingsViewModel.swift — the Settings destination's state (docs/
//  specs/A01-companion-app.md, Design language > More/Settings).
//
//  Backed by ONE `SettingsStore` (FireflyModel, real UserDefaults —
//  slice C's own addition) rather than `AppDependencies.store`: that
//  seam is still `InMemorySettingsStore` under both `.stub()` and
//  `.live()` today (AppDependencies.swift, landed, not slice-owned —
//  see its own comment on why `.live() == .stub()` for now), and this
//  screen's whole point is settings that actually survive a relaunch.
//  A future integration step — pointing `AppDependencies.live()` at
//  `SettingsStore` — will make the two converge; until then, writes
//  made here are real but a Radar-screen (slice D) read of
//  `dependencies.store.bool(.locationSharingEnabled)` will not see
//  them. Flagged rather than hidden.
//
import FireflyModel
import Foundation
import Observation

@MainActor
@Observable
final class SettingsViewModel {
    private let store: SettingsStore
    /// The SAME instance the Connect screen imports channels into
    /// (`FireflyApp.swift` constructs one and hands it to both) — so
    /// this screen's "Channel" row reads exactly what Connect showed,
    /// never a second, possibly-stale copy.
    private let channelImport: ChannelImportViewModel

    var nodeLongName: String
    var nodeShortName: String
    var shareGPSWithNode: Bool
    var locationIntervalSeconds: Double
    var stayConnectedInBackground: Bool
    var colorblindPalette: Bool

    init(store: SettingsStore, channelImport: ChannelImportViewModel) {
        self.store = store
        self.channelImport = channelImport
        nodeLongName = store.nodeLongNamePreference ?? ""
        nodeShortName = store.nodeShortNamePreference ?? ""
        shareGPSWithNode = store.bool(.locationSharingEnabled)
        locationIntervalSeconds = store.double(.locationSharingIntervalSeconds) ?? 30
        stayConnectedInBackground = store.backgroundConnectEnabled
        colorblindPalette = store.colorblindPaletteEnabled
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
}
