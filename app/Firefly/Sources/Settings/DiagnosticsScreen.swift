//
//  DiagnosticsScreen.swift — the Diagnostics sub-screen (docs/specs/
//  A01-companion-app.md, Design language > Diagnostics).
//
import SwiftUI

struct DiagnosticsScreen: View {
    @State var model: DiagnosticsViewModel

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                row(label: "Link state", value: model.linkStateLabel, isLive: true)
                row(label: "Link uptime", value: model.uptimeLabel, isLive: true)
                row(label: "Heard in last 10 min", value: model.heardInLast10MinCount, isLive: false)
                row(label: "Packets in", value: model.packetsIn, isLive: false)
                row(label: "Packets out", value: model.packetsOut, isLive: false)
                row(label: "Ack rate", value: model.ackRate, isLive: false)
                row(label: "Node battery", value: model.nodeBatteryPercent, isLive: false)
                row(label: "Node voltage", value: model.nodeVoltage, isLive: false)
                row(label: "Firmware version", value: model.firmwareVersion, isLive: false)

                Text("Nothing here is inferred. Every value above either came from the node " +
                     "directly (link state) or is UNKNOWN because the app doesn't have it yet.")
                    .font(.caption)
                    .foregroundStyle(Color.ffMuted)
                    .padding(.top, 12)
            }
            .padding(20)
        }
        .background(Color.ffBackground)
        .navigationTitle("DIAGNOSTICS")
        .onAppear { model.observe() }
        .onDisappear { model.stopObserving() }
    }

    private func row(label: String, value: String, isLive: Bool) -> some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(label)
                    .foregroundStyle(Color.ffMuted)
                Text(isLive ? "source: connected radio" : "source: not available yet")
                    .font(.caption2)
                    .foregroundStyle(Color.ffMuted.opacity(0.7))
            }
            Spacer()
            Text(value)
                .font(.system(.body, design: .monospaced))
                .foregroundStyle(isLive ? Color.ffLiveGreen : Color.ffMuted)
        }
        .frame(minHeight: 44)
    }
}
