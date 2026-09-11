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
import FireflyModel
import Foundation
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

    init(store: any FireflyExtraSettingsStoring, channelImport: ChannelImportViewModel) {
        self.store = store
        self.channelImport = channelImport
        nodeLongName = store.nodeLongNamePreference ?? ""
        nodeShortName = store.nodeShortNamePreference ?? ""
        shareGPSWithNode = store.bool(.locationSharingEnabled)
        locationIntervalSeconds = store.double(.locationSharingIntervalSeconds) ?? 30
        stayConnectedInBackground = store.backgroundConnectEnabled
        colorblindPalette = store.colorblindPaletteEnabled
        unitsPreference = store.unitsPreference()
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
}
