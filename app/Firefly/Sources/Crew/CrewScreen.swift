//
//  CrewScreen.swift — the Crew page (`docs/specs/A02-crew-join.md`, §5,
//  artboard `CrewPage.dc.html`). Replaces Settings' Crew + Channel
//  sections and Connect's status line as the one place a crew member
//  looks: your puck, the People list with presence words, hide, Show
//  code, Advanced (the existing Connect radio picker), Leave crew.
//
import FireflyMesh
import FireflyModel
import SwiftUI
#if os(iOS)
import UIKit
#elseif os(macOS)
import AppKit
#endif

struct CrewScreen: View {
    let controller: CrewController
    let membership: any CrewMembershipProviding
    let pairing: CrewPairingController
    let connect: ConnectViewModel
    let client: any MeshtasticClientProtocol
    let channelImport: ChannelImportViewModel
    let scanner: (any NodeScanning)?
    let colorblind: Bool

    @State private var showCode = false
    @State private var showLeaveConfirm = false
    @State private var leaveError: String?

    var body: some View {
        List {
            if let profile = controller.profile {
                Section {
                    Text(profile.humanName)
                        .font(.title2.weight(.bold))
                        .foregroundStyle(Color.ffInk)
                    Text("code \(profile.code)")
                        .font(.system(.footnote, design: .monospaced))
                        .foregroundStyle(Color.ffMuted)
                }

                Section {
                    HStack {
                        VStack(alignment: .leading, spacing: 2) {
                            Text("Your puck · \(connect.connectedRadio?.longName ?? connect.connectedRadio?.bleName ?? "unknown")")
                                .foregroundStyle(Color.ffInk)
                            Text(puckStatusLine)
                                .font(.caption)
                                .foregroundStyle(Color.ffMuted)
                        }
                        Spacer()
                        Text(connect.statusLabel)
                            .font(.caption.weight(.bold))
                            .foregroundStyle(connect.link == .ready ? Color.ffLiveGreen : Color.ffMuted)
                    }
                }

                Section {
                    HStack {
                        Text("People · \(members.count)").font(.headline).foregroundStyle(Color.ffInk)
                        Spacer()
                        Button("Show code") { showCode = true }
                            .font(.footnote)
                    }
                    ForEach(members) { member in
                        memberRow(member)
                            .swipeActions {
                                Button("Hide", role: .destructive) {
                                    controller.hide(nodeID: member.id, pairing: pairing)
                                }
                            }
                    }
                    Text("Anyone with the code is in. Swipe a person to hide them from your radar.")
                        .font(.footnote)
                        .foregroundStyle(Color.ffMuted)
                }

                if !hiddenMembers.isEmpty {
                    Section("Hidden (\(hiddenMembers.count))") {
                        ForEach(hiddenMembers, id: \.self) { nodeID in
                            HStack {
                                Text(String(format: "!%08x", nodeID)).foregroundStyle(Color.ffMuted)
                                Spacer()
                                Button("Unhide") { controller.unhide(nodeID: nodeID) }
                            }
                        }
                    }
                }

                Section {
                    NavigationLink("Advanced — radio, frequency, invite link") {
                        CrewAdvancedScreen(
                            controller: controller, connect: connect, client: client,
                            channelImport: channelImport, scanner: scanner, pairing: pairing,
                            colorblind: colorblind)
                    }
                }

                Section {
                    Button("Leave crew", role: .destructive) { showLeaveConfirm = true }
                }
            } else {
                Section {
                    Text("You're not on a crew yet.").foregroundStyle(Color.ffMuted)
                }
            }
        }
        .navigationTitle("Crew")
        .accessibilityIdentifier("Screen.Crew")
        .sheet(isPresented: $showCode) {
            if let profile = controller.profile {
                CrewCodeCard(profile: profile)
            }
        }
        .alert("Leave \(controller.profile?.humanName ?? "this crew")?",
               isPresented: $showLeaveConfirm) {
            Button("Cancel", role: .cancel) {}
            Button("Leave crew", role: .destructive) {
                Task {
                    if !(await controller.leaveCrew()) {
                        leaveError = controller.leaveErrorMessage
                    }
                }
            }
        } message: {
            Text("Your puck stops sharing your location with this crew, and your crew stops " +
                 "showing up on your radar. You can rejoin any time with the code " +
                 "\(controller.profile?.code ?? "").")
        }
        .alert("Couldn't leave", isPresented: Binding(
            get: { leaveError != nil }, set: { if !$0 { leaveError = nil } })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(leaveError ?? "")
        }
    }

    private var members: [CrewJoinedMember] {
        let hidden = controller.hiddenIDs()
        return membership.currentMembers().filter { !hidden.contains($0.id) }
    }

    private var hiddenMembers: [UInt32] {
        Array(controller.hiddenIDs()).sorted()
    }

    private var puckStatusLine: String {
        connect.link == .ready ? "Connected" : connect.statusLabel.capitalized
    }

    private func memberRow(_ member: CrewJoinedMember) -> some View {
        HStack {
            Text(CrewCopy.displayName(member.displayName)).foregroundStyle(Color.ffInk)
            Spacer()
            VStack(alignment: .trailing, spacing: 2) {
                Text(CrewCopy.presenceLine(member.heardPresence, ageMs: nil))
                    .font(.caption)
                    .foregroundStyle(Color.ffMuted)
                if let chip = CrewCopy.presenceChip(member.heardPresence) {
                    Text(chip).font(.caption2.weight(.bold)).foregroundStyle(Color.ffAmber)
                } else {
                    Text("NAME?").font(.caption2.weight(.bold)).foregroundStyle(Color.ffMuted)
                }
            }
        }
    }
}

/// §5: "Show code pushes the same QR + code panel Start ends on, minus
/// the mint — one screen, two entry points."
struct CrewCodeCard: View {
    let profile: CrewProfile

    var body: some View {
        let link = (try? CrewCode.parse(profile.code)).map { CrewLink.encode(code: $0, name: profile.humanName) }
            ?? "firefly://crew?v=1&code=\(profile.code)"
        return VStack(spacing: 20) {
            Text(profile.humanName).font(.title2.weight(.bold)).foregroundStyle(Color.ffInk)
            CrewQRCodeView(text: link).frame(width: 220, height: 220)
            Text(profile.code)
                .font(.system(.title, design: .monospaced).weight(.bold))
                .foregroundStyle(Color.ffInk)
                .textSelection(.enabled)
            ShareLink(item: "Join my Firefly crew: \(profile.code)\n\(link)") {
                Label("Share link", systemImage: "square.and.arrow.up")
            }
            .buttonStyle(.bordered)
        }
        .padding(32)
        .background(Color.ffBackground)
    }
}

/// §6.5's Advanced inventory. The radio picker itself is the EXISTING
/// `ConnectScreen`, reached here rather than reimplemented (§6.6: Connect
/// stays reachable as Advanced → Radio).
struct CrewAdvancedScreen: View {
    let controller: CrewController
    let connect: ConnectViewModel
    let client: any MeshtasticClientProtocol
    let channelImport: ChannelImportViewModel
    let scanner: (any NodeScanning)?
    let pairing: CrewPairingController
    let colorblind: Bool

    var body: some View {
        List {
            Section {
                NavigationLink("Your puck (radio picker)") {
                    ConnectScreen(connect: connect, client: client, channelImport: channelImport,
                                  scanner: scanner, pairing: pairing, colorblind: colorblind)
                }
            }
            if let profile = controller.profile, let code = try? CrewCode.parse(profile.code) {
                // §6.5: "channel index, channel name (= the code), modem
                // preset (read-only), position precision, PSK
                // fingerprint (first 4 bytes, hex — never the key
                // itself)". Computed straight from the code (never from
                // `controller.confirmationTechnicalDetails`, which only
                // exists mid-way through an active Start/Join
                // confirmation, not once a crew is already settled) —
                // channel index 0 and precision 32 are always what a
                // Firefly crew writes (§1.5), so nothing here depends on
                // a live radio round trip.
                Section("Technical details") {
                    Text("Channel index 0, name \(code.canonical)")
                        .font(.system(.caption, design: .monospaced)).foregroundStyle(Color.ffMuted)
                    Text("Position precision 32 bits")
                        .font(.system(.caption, design: .monospaced)).foregroundStyle(Color.ffMuted)
                    Text("PSK fingerprint \(CrewKey.psk(for: code).prefix(4).map { String(format: "%02x", $0) }.joined())")
                        .font(.system(.caption, design: .monospaced)).foregroundStyle(Color.ffMuted)
                }
            }
            Section {
                if let code = controller.profile?.code, let parsed = try? CrewCode.parse(code) {
                    Button("Copy Meshtastic link") {
                        #if os(iOS)
                        UIPasteboard.general.string = CrewChannel.meshtasticURL(for: parsed)
                        #elseif os(macOS)
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(CrewChannel.meshtasticURL(for: parsed), forType: .string)
                        #endif
                    }
                }
            }
            Section("About this crew's key") {
                Text("Your crew code keeps this crew private from other people at the festival. " +
                     "It is not strong enough to stop someone who really wants in. Don't put " +
                     "anything on here you'd mind a determined stranger reading.")
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
            }
        }
        .navigationTitle("Advanced")
    }
}
