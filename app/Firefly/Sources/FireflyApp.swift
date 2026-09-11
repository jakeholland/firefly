//
//  FireflyApp.swift — the multiplatform shell.
//
//  One target, two platforms (iOS 17+, macOS 14+). The Mac build is not
//  a courtesy port: the iOS Simulator has no Bluetooth at all, so the
//  Mac — with its own CoreBluetooth radio and a USB-serial transport —
//  is where this app is tested against a real Heltec.
//  See docs/specs/A01-companion-app.md.
//
import FireflyMesh
import FireflyModel
import SwiftUI

@main
struct FireflyApp: App {
    /// THE composition root, constructed exactly once for the process.
    ///
    /// `AppGraph` owns the one `AppDependencies` (`.current()`:
    /// `.stub()` in the iOS Simulator, which has no Bluetooth at all;
    /// `.live()` — real client over `BLETransport`, real CoreLocation
    /// providers, real `SettingsStore` — everywhere else), the one set
    /// of `ff_*` C contexts, and every view model built on top of them.
    ///
    /// Nothing below this line may call `AppDependencies.current()` for
    /// itself: doing so builds a SECOND client over a SECOND transport,
    /// which is exactly the bug `RadarView`'s old no-argument `init()`
    /// shipped with (see `AppGraph`'s own header comment).
    @State private var graph: AppGraph
    @State private var connect: ConnectViewModel
    @State private var inbox: InboxViewModel
    @State private var radar: RadarViewModel
    /// Shared between the Connect and Settings destinations (see
    /// `SettingsViewModel`'s own comment) so both read the same imported
    /// channel rather than two disconnected copies.
    @State private var channelImport: ChannelImportViewModel
    @State private var settings: SettingsViewModel

    init() {
        let graph = AppGraph()
        _graph = State(initialValue: graph)
        _connect = State(initialValue: graph.makeConnectViewModel())
        let importVM = ChannelImportViewModel()
        _channelImport = State(initialValue: importVM)
        // Slice C's INTEGRATION TASK, now done: this used to construct
        // its own `SettingsStore()` because `AppDependencies.store` was
        // still `InMemorySettingsStore` under both `.stub()` and
        // `.live()`. `.live()` is pointed at the real `SettingsStore`
        // now, so Settings and every other reader of
        // `dependencies.store.bool(.locationSharingEnabled)` — the
        // phone-GPS uplink above all — share ONE instance, instead of
        // agreeing only by `UserDefaults.standard` coincidence.
        _settings = State(initialValue: SettingsViewModel(store: graph.dependencies.store,
                                                           channelImport: importVM))
        _inbox = State(initialValue: graph.makeInboxViewModel())
        #if os(iOS)
        let haptics: any HapticSignaling = UIKitHapticSignaling()
        #else
        // No Taptic Engine on a Mac — the honest answer, not a gap.
        let haptics: any HapticSignaling = NoHapticSignaling()
        #endif
        _radar = State(initialValue: graph.makeRadarViewModel(haptics: haptics))
    }

    var body: some Scene {
        WindowGroup {
            // One argument per line (SHOULD-FIX 4): every slice that adds
            // a `RootView` dependency appends its own line here instead
            // of editing this call's single line, so sibling slices'
            // hunks land as pure insertions and never collide.
            RootView(
                connect: connect,
                settings: settings,
                channelImport: channelImport,
                client: graph.dependencies.client,
                inbox: inbox,
                radar: radar,
                scanner: graph.dependencies.scanner
            )
            .preferredColorScheme(.dark)
            // The graph's own subscriptions (CoreStore over the client's
            // streams, the portnum-269 reader, the phone-GPS uplink and
            // the ack-timeout tick) start with the window and live as
            // long as it does — NOT per screen. A screen's `observe()`
            // is its own, independent subscription (S1); this is the one
            // that has to keep running when no screen is on top of it,
            // because an ack that arrives while Settings is showing is
            // still an ack.
            .task { await graph.start() }
        }
        #if os(macOS)
        .defaultSize(width: 420, height: 720)
        #endif
    }
}
