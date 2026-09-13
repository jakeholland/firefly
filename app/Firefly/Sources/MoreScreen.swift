//
//  MoreScreen.swift — the "More" destination (docs/specs/
//  A01-companion-app.md, "Navigation"). iOS caps a `TabView` at five
//  visible tabs before it starts auto-generating its own "More" list
//  for the rest; the approved design (RadarSignal.dc.html/MapLive.dc
//  .html/Settings.dc.html mocks) originally put exactly five tabs on
//  the bar — Radar, Map, Inbox, Lineup, More — now four (owner
//  decision, 2026-09-13: Radar and Map became segments of one Find
//  tab) — Find, Inbox, Lineup, More — so Connect and Settings, which
//  do not fit as their own tabs either way, live one tap under this
//  screen instead of iOS's own unstyled overflow list.
//
//  A plain list, not a re-implementation of Connect/Settings/System:
//  each row PUSHES the existing screen (`ConnectScreen`, `SettingsScreen`,
//  `DiagnosticsScreen`) rather than forking its state into a second copy
//  that could disagree with the tab it used to be.
//
//  Owner note (build 304) — "tapping More while Connect is already
//  pushed pushes another Connect": the OLD `PushRequest`/fresh-`UUID`
//  wrapper (kept below only in this comment, for the record) combined
//  badly with `autoOpen` never being consumed: `autoOpen` stayed
//  `.connect` forever after the very first launch-time push, so ANY
//  later reappearance of this screen's root list (in particular the
//  system's own "tap the already-selected tab pops its NavigationStack
//  to root" gesture re-revealing the root list out from under a pushed
//  Connect) re-ran this file's `.task`/`.onChange` and silently pushed
//  Connect right back — from the owner's seat, retapping More while on
//  Connect looked like it "pushed another Connect" instead of popping.
//  Fixed two ways, together: (1) `path` is now an explicit `[Row]`
//  owned by `RootView` (`RootView`'s own `morePath`), so a genuine
//  tab-reselect can reset it to `[]` from OUTSIDE this view — see
//  `RootView.selectionBinding` — without depending on that system
//  gesture actually firing; (2) `autoOpen` is consumed exactly once:
//  `onAutoOpenHandled` tells `RootView` to clear it the instant this
//  screen acts on it, so the SAME request can never replay itself just
//  because this screen reappears.
//
import FireflyMesh
import FireflyModel
import SwiftUI

struct MoreScreen: View {
    let connect: ConnectViewModel
    let settings: SettingsViewModel
    let channelImport: ChannelImportViewModel
    let client: any MeshtasticClientProtocol
    let lineup: LineupViewModel
    let scanner: (any NodeScanning)?
    let pairing: CrewPairingController
    let colorblind: Bool
    /// Demo-only (`-FireflyDemoScreen diagnostics`, `RootView`'s own
    /// mapping) — forwarded to `SettingsScreen` exactly as it was when
    /// Settings was its own top-level tab; `false` on every non-demo
    /// build.
    var autoOpenDiagnosticsInSettings: Bool = false
    /// One-shot: which row (if any) `RootView` wants pushed the next
    /// time this screen processes it — `RootView`'s own "no bonded
    /// radio yet -> land on Connect with zero taps" first-launch rule,
    /// `-FireflyDemoScreen connect`/`settings`/`diagnostics`, and — on
    /// macOS — a sidebar click on one of this screen's own rows.
    /// `nil` opens on the plain list, same as a manual tap on the More
    /// tab always did. NEVER a sticky flag: this screen calls
    /// `onAutoOpenHandled()` the instant it acts on a non-`nil` value,
    /// which is `RootView`'s cue to set its own copy back to `nil` —
    /// see this file's header comment for the bug that shape replaces.
    var autoOpen: Row?
    /// Tells `RootView` the just-seen `autoOpen` value has been
    /// consumed (whether that meant a fresh push or recognizing the
    /// requested row was already on top) — `RootView` clears its own
    /// `moreAutoOpen` in response. Defaults to a no-op so previews/tests
    /// that never set `autoOpen` need not supply this.
    var onAutoOpenHandled: () -> Void = {}
    /// This tab's own push stack — owned by `RootView` (`@State
    /// private var morePath`), not locally, precisely so a tab
    /// reselect can reset it to `[]` from outside this view (see this
    /// file's header comment). At most one element deep today: Connect/
    /// Settings/System are leaf destinations with nothing further to
    /// push from here.
    @Binding var path: [Row]

    /// `MoreScreenRow`, under the name every call site already uses
    /// (`MoreScreen.Row`) — the real declaration and its `pushed(_:
    /// onto:)` push rule live in `MoreScreenNavigation.swift` (no
    /// SwiftUI import), not here. See that file's own header comment
    /// for why: it is the seam `FireflyAppTests` exercises directly.
    typealias Row = MoreScreenRow

    private static func pushed(_ row: Row, onto path: [Row]) -> [Row] {
        MoreScreenNavigation.pushed(row, onto: path)
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 28) {
                MoreBlock {
                    MoreRow(title: "CONNECT", subtitle: connect.headerStatusText,
                            systemImage: "antenna.radiowaves.left.and.right",
                            identifier: "MoreRow.Connect") { open(.connect) }
                }
                MoreBlock {
                    MoreRow(title: "SETTINGS", subtitle: "Node, channel, units, crew",
                            systemImage: "slider.horizontal.3",
                            identifier: "MoreRow.Settings") { open(.settings) }
                    Divider().overlay(Color.ffMuted.opacity(0.2))
                    MoreRow(title: "SYSTEM", subtitle: "Diagnostics — link, packets, battery",
                            systemImage: "waveform.path.ecg",
                            identifier: "MoreRow.System") { open(.system) }
                }
            }
            .padding(20)
        }
        .background(Color.ffBackground)
        .navigationTitle("MORE")
        // One identifying accessibility identifier per screen, same
        // convention every other destination in this app follows (M3,
        // `ConnectScreen`'s own comment) — also this test suite's own
        // signal that a visit to More has landed back on the plain
        // list rather than still being pushed into one of its rows.
        .accessibilityIdentifier("Screen.More")
        .navigationDestination(for: Row.self) { row in
            destination(for: row)
        }
        .task {
            if let autoOpen { open(autoOpen) }
        }
        // macOS only, in practice: a sidebar click on Connect/Settings/
        // System while `More` is ALREADY the selected sidebar item
        // changes `autoOpen` without ever tearing this view down (no
        // `selection` change to remount it), so the one-shot `.task`
        // above would never see the new value. iOS never changes
        // `autoOpen` after this screen's first appearance (it is
        // cleared back to `nil` the instant it is consumed), so this is
        // a no-op there.
        .onChange(of: autoOpen) { _, newValue in
            if let newValue { open(newValue) }
        }
    }

    private func open(_ row: Row) {
        path = Self.pushed(row, onto: path)
        onAutoOpenHandled()
    }

    @ViewBuilder
    private func destination(for row: Row) -> some View {
        switch row {
        case .connect:
            ConnectScreen(connect: connect, client: client, channelImport: channelImport,
                          scanner: scanner, pairing: pairing, colorblind: colorblind)
        case .settings:
            // Owner note (build 304, item 3): Settings' own Crew section
            // has rename/remove but no way to PAIR — that only exists on
            // Connect's Nearby section (`NearbyNodesViewModel.addToCrew
            // (_:)`). `onOpenConnect` gives its "ADD CREW" button a real
            // destination instead of leaving the owner to find Connect
            // on their own. Reuses this same `open(_:)` — a tap here
            // REPLACES Settings with Connect in `path` (this tab never
            // nests more than one level deep), never a second, parallel
            // push mechanism.
            SettingsScreen(model: settings, client: client, pairing: pairing, lineup: lineup,
                            autoOpenDiagnostics: autoOpenDiagnosticsInSettings,
                            onOpenConnect: { open(.connect) })
        case .system:
            DiagnosticsScreen(model: DiagnosticsViewModel(client: client))
        }
    }
}

private struct MoreBlock<Content: View>: View {
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            content
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.ffSurface, in: RoundedRectangle(cornerRadius: 16))
    }
}

/// One More-screen row: icon, title + subtitle, chevron — the mock's
/// own row shape (`Settings.dc.html`/`Main.dc.html`: a label on the
/// left, a value/chevron on the right), rebuilt here with this app's
/// existing theme tokens rather than the mock's raw hex values.
private struct MoreRow: View {
    let title: String
    let subtitle: String
    let systemImage: String
    let identifier: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 12) {
                Image(systemName: systemImage)
                    .foregroundStyle(Color.ffAmber)
                    .frame(width: 22)
                VStack(alignment: .leading, spacing: 2) {
                    Text(title)
                        .font(.system(.body, design: .rounded).weight(.semibold))
                        .foregroundStyle(Color.ffInk)
                    Text(subtitle)
                        .font(.caption)
                        .foregroundStyle(Color.ffMuted)
                        .lineLimit(1)
                }
                Spacer()
                Image(systemName: "chevron.right")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(Color.ffMuted)
            }
            .padding(16)
            .frame(minHeight: 44)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier(identifier)
    }
}
