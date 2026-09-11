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

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 28) {
                nodeIdentitySection
                channelSection
                connectivitySection
                appearanceSection
                NavigationLink("DIAGNOSTICS") {
                    DiagnosticsScreen(model: DiagnosticsViewModel(client: client))
                }
                .buttonStyle(.bordered)
                .tint(.ffAmber)
                .frame(minHeight: 44)
            }
            .padding(20)
        }
        .background(Color.ffBackground)
        .navigationTitle("SETTINGS")
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
    @Binding var isOn: Bool

    var body: some View {
        Toggle(label, isOn: $isOn)
            .tint(.ffAmber)
            .foregroundStyle(Color.ffInk)
            .frame(minHeight: 44)
    }
}
