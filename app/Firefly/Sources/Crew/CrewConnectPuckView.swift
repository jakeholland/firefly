//
//  CrewConnectPuckView.swift — "Connect your puck", the step A02 §6.1's
//  first-launch flow gains between Welcome and Start/Join (owner report,
//  2026-09-14, build 328: "we also need better handling on that screen
//  for connecting to a puck/meshtastic node first or making sure we are
//  connected").
//
//  It is NOT a second Connect screen. Everything here drives the same
//  two pieces the real Connect screen drives — `ConnectViewModel` (link
//  state, remembered peripheral, connect/disconnect) and
//  `RadioListBuilder` (`NearbyNodesViewModel.swift`, the row model with
//  its per-row action) — through the same `PeripheralDiscovering` seam,
//  so there is exactly one implementation of "which radio, and what is
//  it doing" in this app. What differs is only what it SAYS and what it
//  leaves out: no channel card, no NEARBY crew list, no node ids, no
//  dBm — A02 §6's "on the main path the product has pucks, crews and
//  people".
//
//  Two behaviours this screen has that `ConnectScreen` deliberately does
//  not:
//
//  1. It scans on appear. `ConnectScreen`'s own `.onAppear` comment
//     explains why IT must not (merely showing a screen is not the
//     moment to make macOS put up its Bluetooth dialog, and it is the
//     screen a test runner lands on). Here the user has just tapped a
//     button that says they want to connect their puck, which is
//     precisely the moment to ask.
//  2. It auto-connects to the REMEMBERED radio, once, with no tap —
//     §2.1 step 1's "runs the existing discovery/connect path
//     headlessly". A person who already paired once should not have to
//     pick their puck out of a list again.
//
import FireflyMesh
import FireflyModel
import SwiftUI

struct CrewConnectPuckView: View {
    let connect: ConnectViewModel
    /// The real BLE scan when there is a radio behind it, and
    /// `StubPeripheralDiscovery` — which discovers nothing — when there
    /// is not, exactly as `ConnectScreen` composes it.
    let discovery: any PeripheralDiscovering
    /// Called once the link actually reaches a state a crew write can
    /// use. Not merely `.ready`: `CrewController.hasConnectedRadio` is
    /// the precondition that matters, and this view is handed it rather
    /// than re-deriving a second opinion about the same fact.
    let isRadioUsable: () -> Bool
    let onConnected: () -> Void
    /// "Do this later" — never removed, never disabled. A person with no
    /// puck in reach must still be able to look around the app.
    let onSkip: () -> Void

    @State private var peripherals: [DiscoveredPeripheral] = []
    @State private var isScanning = false
    @State private var scanDidTimeOut = false
    @State private var scanTimeoutTask: Task<Void, Never>?
    @State private var didAutoConnect = false
    @State private var didHandOff = false
    @State private var showNoPuckHelp = false

    /// Same budget `ConnectScreen.scanEmptyTimeout` uses, and for the
    /// same reason — how long this screen waits before the honest
    /// answer becomes "nothing is here".
    private static let scanEmptyTimeout: Duration = .seconds(8)

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                heading
                statusCard
                radioList
                noPuckYet
                Button(action: onSkip) {
                    Text("Do this later")
                        .font(.footnote)
                        .foregroundStyle(Color.ffAmber)
                        .frame(maxWidth: .infinity, minHeight: 44)
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("CrewConnect.Later")
            }
            .padding(24)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color.ffBackground)
        .accessibilityIdentifier("Screen.CrewConnectPuck")
        .task {
            for await found in discovery.peripherals() {
                peripherals = found
            }
        }
        .task { await beginIfNeeded() }
        // The hand-off is driven by the link reaching a usable state,
        // not by the tap that started it: an auto-connect, a manual row
        // tap and a reconnect all arrive here the same way, and a radio
        // that was ALREADY connected when this screen opened never gets
        // here at all (the container skips the step — §6.1).
        .onChange(of: connect.link) { _, _ in handOffIfConnected() }
        .onDisappear {
            discovery.stopScanning()
            scanTimeoutTask?.cancel()
        }
        .sheet(isPresented: $showNoPuckHelp) { noPuckSheet }
    }

    // MARK: - Pieces

    private var heading: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Connect your puck")
                .font(.system(.title, design: .rounded).weight(.heavy))
                .foregroundStyle(Color.ffInk)
                .accessibilityIdentifier("CrewConnect.Title")
            Text("Turn your puck on and hold it near your phone. Firefly needs it connected " +
                 "before you can start or join a crew.")
                .font(.body)
                .foregroundStyle(Color.ffMuted)
        }
    }

    private var statusCard: some View {
        HStack(alignment: .top, spacing: 10) {
            if isWorking {
                ProgressView().controlSize(.small)
            } else {
                Circle().fill(statusColor).frame(width: 10, height: 10).padding(.top, 5)
            }
            VStack(alignment: .leading, spacing: 4) {
                // `puckStatusText` is the view model's own sentence
                // (MVVM convention 5) — NOT CONNECTED / Connecting to X…
                // / Setting up X… / Connected to X, plus the plain
                // Bluetooth-off and not-allowed messages.
                Text(connect.puckStatusText)
                    .font(.headline)
                    .foregroundStyle(isFailed ? Color.ffAlert : Color.ffInk)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityIdentifier("CrewConnect.Status")
                if let trouble = connect.lastTrouble, trouble.isRetryable {
                    Text("Tap RESCAN once you've fixed it.")
                        .font(.footnote)
                        .foregroundStyle(Color.ffMuted)
                }
            }
            Spacer(minLength: 0)
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.ffSurface)
        .clipShape(RoundedRectangle(cornerRadius: 12))
    }

    private var radioList: some View {
        VStack(alignment: .leading, spacing: 12) {
            let rows = RadioListBuilder.rows(discovered: peripherals, connect: connect)
            if rows.isEmpty {
                emptyState
            } else {
                ForEach(rows) { row in
                    CrewRadioRow(row: row, onTap: { tap(row) })
                }
            }
            Button("RESCAN") { startScan() }
                .buttonStyle(.bordered)
                .tint(.ffAmber)
                .frame(minHeight: 44)
                .accessibilityIdentifier("CrewConnect.Rescan")
        }
    }

    @ViewBuilder
    private var emptyState: some View {
        if isScanning && !scanDidTimeOut {
            HStack(spacing: 8) {
                ProgressView().controlSize(.small)
                Text("Looking for your puck…")
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
            }
            .frame(minHeight: 44, alignment: .leading)
        } else if scanDidTimeOut {
            // Review of PR #319: this must not blame the puck for
            // something the app has not checked. `NodeScanning.scan()`
            // yields nothing when Bluetooth is off or not allowed
            // ("a radio that never powers on … simply yields nothing"),
            // and with no REMEMBERED radio this screen never calls
            // `connect()`, so `lastTrouble` is nil and the three plain
            // Bluetooth sentences are unreachable on a genuine first
            // launch. Until the scan seam can report why it found
            // nothing, an empty scan means one of two things and this
            // says both rather than asserting the one it cannot know.
            Text("Nothing found yet. Check your puck is powered on and nearby, " +
                 "and that Bluetooth is on for Firefly.")
                .font(.footnote)
                .foregroundStyle(Color.ffMuted)
                .fixedSize(horizontal: false, vertical: true)
        } else {
            Text("Tap RESCAN to find your puck.")
                .font(.footnote)
                .foregroundStyle(Color.ffMuted)
        }
    }

    private var noPuckYet: some View {
        Button("Don't have a puck yet?") { showNoPuckHelp = true }
            .font(.footnote)
            .foregroundStyle(Color.ffAmber)
            .frame(minHeight: 44)
            .accessibilityIdentifier("CrewConnect.NoPuck")
    }

    private var noPuckSheet: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("You need a puck").font(.headline).foregroundStyle(Color.ffInk)
            Text("A puck is the little radio that does the actual finding. It talks straight to " +
                 "your crew's pucks — no phone signal, no wifi, no festival network.")
                .font(.body)
                .foregroundStyle(Color.ffMuted)
            Text("Without one, Firefly can still show you the lineup and everything you've " +
                 "already picked — it just can't see your crew.")
                .font(.body)
                .foregroundStyle(Color.ffMuted)
            Spacer()
            Button("Got it") { showNoPuckHelp = false }
                .buttonStyle(.borderedProminent)
                .tint(Color.ffAmber)
                .foregroundStyle(Color.ffBackground)
                .frame(maxWidth: .infinity, minHeight: 44)
        }
        .padding(24)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(Color.ffBackground)
        #if os(iOS)
        .presentationDetents([.fraction(0.45)])
        #endif
    }

    // MARK: - State

    private var isWorking: Bool {
        switch connect.link {
        case .connecting, .handshaking, .reconnecting: return true
        case .disconnected, .ready, .failed: return false
        }
    }

    private var isFailed: Bool {
        if case .failed = connect.link { return true }
        return false
    }

    private var statusColor: Color {
        switch connect.link {
        case .ready: return .ffLiveGreen
        case .connecting, .handshaking, .reconnecting: return .ffAmber
        case .failed: return .ffAlert
        case .disconnected: return .ffMuted
        }
    }

    // MARK: - Actions

    /// Auto-connect to the remembered radio if there is one, otherwise
    /// scan. Runs once — `didAutoConnect` — so a redraw cannot start a
    /// second connect racing the first
    /// (`MeshtasticClientError.alreadyConnecting`).
    private func beginIfNeeded() async {
        guard !didAutoConnect else { return }
        didAutoConnect = true
        handOffIfConnected()
        guard !didHandOff else { return }
        startScan()
        if connect.rememberedPeripheralID != nil, !connect.isBusyOrConnected {
            await connect.connect()
        }
    }

    private func startScan() {
        isScanning = true
        scanDidTimeOut = false
        discovery.startScanning()
        scanTimeoutTask?.cancel()
        scanTimeoutTask = Task {
            try? await Task.sleep(for: Self.scanEmptyTimeout)
            guard !Task.isCancelled else { return }
            scanDidTimeOut = true
        }
    }

    private func tap(_ row: RadioListRow) {
        switch row.action {
        case .connect:
            // The same pair a Connect-screen row performs
            // (`ConnectScreen.performPrimaryAction(for:)`): point the
            // transport at THIS peripheral, tell the view model the name
            // it already knows so the status line can name the puck
            // through the whole connect window, then connect.
            let match = peripherals.first(where: { $0.id == row.id })
            discovery.select(row.id)
            connect.noteSelectedPeripheral(name: match?.name, rssiDbm: match?.rssiDbm)
            Task { await connect.connect() }
        case .disconnect:
            Task { await connect.disconnect() }
        case .unavailable:
            break
        }
    }

    private func handOffIfConnected() {
        guard !didHandOff, isRadioUsable() else { return }
        didHandOff = true
        discovery.stopScanning()
        scanTimeoutTask?.cancel()
        onConnected()
    }
}

/// One radio row, in this screen's vocabulary: the puck's own name, a
/// short id to tell two apart, and its state as a word. No dBm, no node
/// id — A02 §6.5 keeps those under Advanced.
private struct CrewRadioRow: View {
    let row: RadioListRow
    let onTap: () -> Void

    var body: some View {
        Button(action: onTap) {
            HStack(spacing: 12) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(row.title)
                        .font(.headline)
                        .foregroundStyle(Color.ffInk)
                    if row.shortID != row.title {
                        Text(row.shortID)
                            .font(.system(.caption, design: .monospaced))
                            .foregroundStyle(Color.ffMuted)
                    }
                }
                Spacer(minLength: 0)
                Text(actionLabel)
                    .font(.system(.subheadline, design: .rounded).weight(.bold))
                    .foregroundStyle(row.action == .unavailable ? Color.ffMuted : Color.ffAmber)
            }
            .frame(minHeight: 48)
            .padding(.horizontal, 16)
            .background(Color.ffSurface)
            .clipShape(RoundedRectangle(cornerRadius: 12))
            .overlay(
                RoundedRectangle(cornerRadius: 12)
                    .strokeBorder(row.status.isHighlighted ? Color.ffAmber : .clear, lineWidth: 1))
        }
        .buttonStyle(.plain)
        .disabled(row.action == .unavailable)
        .accessibilityIdentifier("CrewConnect.Radio.\(row.shortID)")
    }

    private var actionLabel: String {
        switch row.action {
        case .connect: return "CONNECT"
        case .disconnect: return connectingOrConnected
        case .unavailable: return "—"
        }
    }

    /// While the link is doing something with THIS row, the row's
    /// trailing word is the state, not a command — the action is still
    /// "tap to stop", which the status word plus the highlight already
    /// says. `RadioListRow.Status.chipText`'s own words, reused rather
    /// than respelled.
    private var connectingOrConnected: String {
        row.status == .connected ? "CONNECTED" : (row.status.chipText ?? "DISCONNECT")
    }
}
