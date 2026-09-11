//
//  SettingsScreen.swift — the Settings destination (docs/specs/
//  A01-companion-app.md, Design language > More/Settings).
//
import FireflyMesh
import FireflyModel
import SwiftUI

struct SettingsScreen: View {
    @Bindable var model: SettingsViewModel
    let client: any MeshtasticClientProtocol
    /// Demo-only (`-FireflyDemoScreen diagnostics`, `RootView`'s own
    /// mapping): pushes straight to Diagnostics on appear. `false` in
    /// every non-demo build.
    var autoOpenDiagnostics: Bool = false
    @State private var showDiagnostics = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 28) {
                nodeIdentitySection
                channelSection
                connectivitySection
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
        }
    }

    private var channelSection: some View {
        SettingsBlock(title: "CHANNEL") {
            LabeledRow(label: "Region", value: model.region)
            LabeledRow(label: "Channel", value: model.currentChannelName)
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
                // PR #265 review, should-fix: the toggle used to imply
                // a working feature. `AppGraph.stop()`'s own doc
                // comment has the mechanism — nothing calls it, and
                // nothing reconnects on return either — so this
                // subtitle is the honest state of M1: the setting is
                // stored (and will matter once M2 builds the actual
                // background behavior), but toggling it does not yet
                // change what the app does when it is backgrounded.
                subtitle: "Background reconnect isn't built yet (M2) \u{2014} this only saves the preference for now.",
                isOn: Binding(get: { model.stayConnectedInBackground }, set: model.setStayConnectedInBackground))
        }
    }

    private var appearanceSection: some View {
        SettingsBlock(title: "APPEARANCE") {
            ToggleRow(
                label: "Colorblind crew palette",
                isOn: Binding(get: { model.colorblindPalette }, set: model.setColorblindPalette))
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
