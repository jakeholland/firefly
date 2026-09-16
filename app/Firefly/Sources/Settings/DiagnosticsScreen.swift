//
//  DiagnosticsScreen.swift — the Diagnostics sub-screen (docs/specs/
//  A01-companion-app.md, Design language > Diagnostics).
//
//  A04 (docs/specs/A04-telemetry.md) adds two rows: "Share diagnostics"
//  (a toggle) and "Export diagnostics" (a share-sheet button) — both
//  described in that spec's §4 and both live here, not on the main
//  Settings screen, because Diagnostics is already where this app
//  explains what it does and does not know about itself.
//
import FireflyTelemetry
import SwiftUI

struct DiagnosticsScreen: View {
    @State var model: DiagnosticsViewModel
    /// A04 — the files `exportDiagnosticsFiles()` most recently
    /// produced, staged here because `ShareLink`'s `item:` needs a value
    /// ready before the sheet presents, not an `async` call it can
    /// await itself.
    @State private var exportFiles: [URL] = []
    @State private var isPreparingExport = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                shareDiagnosticsSection
                row(label: "Link state", value: model.linkStateLabel, isLive: true)
                row(label: "Link uptime", value: model.uptimeLabel, isLive: true)
                // A03 §3.10 — one line that says what is actually true
                // about the background connection, in plain words. Its
                // own row shape, because it is a sentence rather than a
                // value.
                statusLine(model.backgroundConnectionLabel)
                // The three counters are this PHONE's own observations of
                // its Bluetooth link — not the radio's. Their source line
                // says so, because a row that mislabels where a number
                // came from is a real finding in this repo, not a nit
                // (AGENTS.md: "honesty rules bind debug surfaces too").
                row(label: "Reconnected on its own", value: model.reconnectsLabel,
                     isLive: model.hasLinkDiagnostics, source: linkSource)
                row(label: "Last came back", value: model.lastReconnectLabel,
                     isLive: model.hasLinkDiagnostics, source: linkSource)
                row(label: "Rediscovery scans started", value: model.scanStartsLabel,
                     isLive: model.hasLinkDiagnostics, source: linkSource)
                // A03 §3.1 (S1b) — the restoration counters. §6's P3 is
                // measured here: a relaunch after a jettison shows a
                // restore this process actually adopted, with the state
                // it restored into.
                row(label: "Restored sessions", value: model.restoredSessionsLabel,
                     isLive: model.hasLinkDiagnostics, source: linkSource)
                row(label: "Last restore", value: model.lastRestoreLabel,
                     isLive: model.hasLinkDiagnostics, source: linkSource)
                row(label: "Notifications", value: model.notificationsLabel, isLive: true,
                     source: "source: this phone's notification settings")
                row(label: "Heard in last 10 min", value: model.heardInLast10MinCount, isLive: false)
                row(label: "Packets in", value: model.packetsIn, isLive: false)
                row(label: "Packets out", value: model.packetsOut, isLive: false)
                row(label: "Ack rate", value: model.ackRate, isLive: false)
                row(label: "Radio battery", value: model.nodeBatteryPercent, isLive: false)
                row(label: "Radio voltage", value: model.nodeVoltage, isLive: false)
                row(label: "Firmware version", value: model.firmwareVersion, isLive: false)

                Text("Every value above comes from the connected radio, or reads UNKNOWN until the app has it.")
                    .font(.caption)
                    .foregroundStyle(Color.ffCaption)
                    .padding(.top, 12)
            }
            .padding(20)
        }
        .background(Color.ffBackground)
        .navigationTitle("DIAGNOSTICS")
        .onAppear { model.observe() }
        .onDisappear { model.stopObserving() }
    }

    /// A04 — "Share diagnostics" (a toggle) and "Export diagnostics" (a
    /// share sheet over whatever the local recorder currently holds).
    /// Its own block, ahead of every live value, since it is a setting
    /// and an action rather than a reading.
    private var shareDiagnosticsSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Toggle("Share diagnostics", isOn: Binding(
                    get: { model.shareDiagnosticsEnabled },
                    set: { model.setShareDiagnostics($0) }))
                Text("Sends connection diagnostics when the phone has signal. " +
                     "Never your messages or exact location.")
                    .font(.caption2)
                    .foregroundStyle(Color.ffMuted.opacity(0.7))
            }
            .frame(minHeight: 44)

            if model.canExportDiagnostics {
                Button {
                    isPreparingExport = true
                    Task {
                        exportFiles = await model.exportDiagnosticsFiles()
                        isPreparingExport = false
                    }
                } label: {
                    HStack {
                        Text("Export diagnostics")
                        if isPreparingExport { ProgressView().controlSize(.small) }
                    }
                }
                .buttonStyle(.bordered)
                .tint(.ffAmber)
                .frame(minHeight: 44)
                // Populated by the button tap above — `ShareLink` needs
                // the file list ready before presenting, not an `async`
                // call of its own, so this button always fires the
                // fetch first and lets a non-empty `exportFiles`
                // trigger the share sheet.
                if !exportFiles.isEmpty {
                    ShareLink(items: exportFiles) {
                        Text("Share \(exportFiles.count) file\(exportFiles.count == 1 ? "" : "s")")
                    }
                    .buttonStyle(.bordered)
                    .frame(minHeight: 44)
                }
            }
        }
    }

    /// The §3.10 status line. Full width and unmonospaced — it is a
    /// sentence, not a reading, and squeezing it into the value column
    /// would truncate exactly the part that carries the meaning.
    private func statusLine(_ text: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("Background connection")
                .foregroundStyle(Color.ffCaption)
            Text(text)
                .foregroundStyle(Color.ffLiveGreen)
                .fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .frame(minHeight: 44)
    }

    /// Where the three A03 counters come from — or honestly, that there
    /// is nothing here to count them.
    private var linkSource: String {
        model.hasLinkDiagnostics ? "source: this phone's Bluetooth link"
                                 : "source: no Bluetooth link on this build"
    }

    private func row(label: String, value: String, isLive: Bool, source: String? = nil) -> some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(label)
                    .foregroundStyle(Color.ffCaption)
                Text(source ?? (isLive ? "source: connected radio" : "source: not available yet"))
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
