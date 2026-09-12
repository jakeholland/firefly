//
//  MoreScreen.swift — the "More" destination (docs/specs/
//  A01-companion-app.md, "Navigation"). iOS caps a `TabView` at five
//  visible tabs before it starts auto-generating its own "More" list
//  for the rest; the approved design (RadarSignal.dc.html/MapLive.dc
//  .html/Settings.dc.html mocks) puts exactly five tabs on the bar —
//  Radar, Map, Inbox, Lineup, More — so Connect and Settings, which no
//  longer fit as their own tabs, live one tap under this screen
//  instead of iOS's own unstyled overflow list.
//
//  A plain list, not a re-implementation of Connect/Settings/System:
//  each row PUSHES the existing screen (`ConnectScreen`, `SettingsScreen`,
//  `DiagnosticsScreen`) rather than forking its state into a second copy
//  that could disagree with the tab it used to be.
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
    /// Which row (if any) to push into the moment this screen first
    /// appears: `RootView`'s own "no bonded radio yet -> land on
    /// Connect with zero taps" first-launch rule, `-FireflyDemoScreen
    /// connect`/`settings`/`diagnostics`, and — on macOS — a sidebar
    /// click on one of this screen's own rows while `More` is already
    /// selected (`RootView`'s `content` doc comment on why this needs
    /// to react to a CHANGE, not just a one-shot value). `nil` opens on
    /// the plain list, same as a manual tap on the More tab always did.
    var autoOpen: Row?

    enum Row: Hashable {
        case connect, settings, system
    }

    /// A single `navigationDestination(item:)` push, identified by a
    /// fresh `UUID` on every request rather than by `Row` alone. `Row`
    /// carries no identity beyond its own case, and TWO earlier
    /// attempts each broke on that in a different way: keying
    /// `navigationDestination(item:)` directly on `Row?` pushed
    /// `.connect` fine the first time, but pushing `.connect` again
    /// later (same, already-seen value) silently no-opped — confirmed
    /// empirically, reproduced 3/3 runs of the UI test's final
    /// "back to Connect" step, while `.settings`, pushed for the first
    /// time in the same run, never showed it. Splitting into three
    /// independent `navigationDestination(isPresented:)` flags (one per
    /// row, `SettingsScreen.showDiagnostics`'s own shape) traded that
    /// bug for a worse one: three such modifiers stacked on the same
    /// view intermittently left the stack unable to push OR pop at all.
    /// Wrapping the row in a per-request `UUID` keeps ONE
    /// `navigationDestination(item:)` (the reliable shape) while making
    /// every push a genuinely new value, even a same-row repeat.
    private struct PushRequest: Hashable {
        let id = UUID()
        let row: Row
    }

    @State private var pushed: PushRequest?

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
        .navigationDestination(item: $pushed) { request in
            destination(for: request.row)
        }
        .task {
            if let autoOpen { open(autoOpen) }
        }
        // macOS only, in practice: a sidebar click on Connect/Settings/
        // System while `More` is ALREADY the selected sidebar item
        // changes `autoOpen` without ever tearing this view down (no
        // `selection` change to remount it), so the one-shot `.task`
        // above would never see the new value. iOS never changes
        // `autoOpen` after this screen's first appearance, so this is a
        // no-op there.
        .onChange(of: autoOpen) { _, newValue in
            if let newValue { open(newValue) }
        }
    }

    private func open(_ row: Row) {
        pushed = PushRequest(row: row)
    }

    @ViewBuilder
    private func destination(for row: Row) -> some View {
        switch row {
        case .connect:
            ConnectScreen(connect: connect, client: client, channelImport: channelImport,
                          scanner: scanner, pairing: pairing, colorblind: colorblind)
        case .settings:
            SettingsScreen(model: settings, client: client, pairing: pairing, lineup: lineup,
                            autoOpenDiagnostics: autoOpenDiagnosticsInSettings)
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
