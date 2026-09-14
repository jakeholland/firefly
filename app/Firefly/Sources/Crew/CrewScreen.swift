//
//  CrewScreen.swift — the Crew page (`docs/specs/A02-crew-join.md`, §5,
//  artboard `CrewPage.dc.html`). Replaces Settings' Crew + Channel
//  sections and Connect's status line as the one place a crew member
//  looks: your puck, the People list with presence words, hide, Show
//  code, Advanced (the existing Connect radio picker), Leave crew.
//
import FireflyMesh
import FireflyModel
import MeshtasticProto
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
                            Text(puckTitle)
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
                            .buttonStyle(.bordered)
                            .tint(Color.ffMuted)
                            .foregroundStyle(Color.ffAmber)
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
                                    .buttonStyle(.bordered)
                                    .tint(Color.ffMuted)
                                    .foregroundStyle(Color.ffAmber)
                            }
                        }
                    }
                }

                Section {
                    NavigationLink("Advanced — radio, frequency, invite link") {
                        CrewAdvancedScreen(
                            controller: controller, membership: membership, connect: connect, client: client,
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

    /// One name, one presence pill — the SAME pill an Inbox row renders
    /// (`PresencePill`), so the two screens can only ever say the same
    /// thing about the same member. The old second "NAME?" chip is
    /// gone: a nameless row already says "New crew member" in its name
    /// column, and stamping a question mark beside it said the same
    /// unknown twice in two vocabularies (PR #308 review).
    private func memberRow(_ member: CrewJoinedMember) -> some View {
        HStack {
            Text(CrewCopy.displayName(member.displayName)).foregroundStyle(Color.ffInk)
            Spacer()
            PresencePill(presence: CrewCopy.tag(for: member.heardPresence),
                         age: member.heardAgeMs.map { TimeInterval($0) / 1000 })
        }
    }

    /// §5's "Your puck" row names the ACTUAL radio: its long name, else
    /// its BLE name, else that name's own `RadioListRow.shortID`-style
    /// suffix (`Meshtastic_e3d4` -> `e3d4`). With nothing connected the
    /// row is "Your puck" alone — never the word "unknown", which is
    /// not a radio's name, reads as a fault, and was on screen in this
    /// PR's own screenshots (PR #308 review).
    private var puckTitle: String {
        guard let radio = connect.connectedRadio else { return "Your puck" }
        if let longName = radio.longName, !longName.isEmpty {
            return "Your puck \u{00B7} \(longName)"
        }
        if let bleName = radio.bleName, !bleName.isEmpty {
            if let underscore = bleName.lastIndex(of: "_") {
                let suffix = String(bleName[bleName.index(after: underscore)...])
                if !suffix.isEmpty { return "Your puck \u{00B7} \(suffix)" }
            }
            return "Your puck \u{00B7} \(bleName)"
        }
        return "Your puck"
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
                    .frame(minHeight: 44)
            }
            .buttonStyle(.bordered)
            .tint(Color.ffMuted)
            .foregroundStyle(Color.ffAmber)
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
    /// The Joined/People list seam (`CrewMembershipProviding`) — the
    /// SAME instance `CrewScreen` already holds, threaded through so
    /// this screen can read the two slice-E-only seams
    /// (`CrewHeardListProviding`/`CrewDiagnosticsProviding`) off it via
    /// a runtime cast (`heardListViewModel`/`diagnosticsViewModel`
    /// below) rather than widening this file's own parameter list with
    /// a concrete `CrewMembershipEngine` type — every real composition
    /// hands in exactly that engine (`AppGraph.crewMembership`, #306),
    /// so the cast always succeeds outside a test that deliberately
    /// passes a narrower stub.
    let membership: any CrewMembershipProviding
    let connect: ConnectViewModel
    let client: any MeshtasticClientProtocol
    let channelImport: ChannelImportViewModel
    let scanner: (any NodeScanning)?
    let pairing: CrewPairingController
    let colorblind: Bool

    private var heardListViewModel: CrewHeardListViewModel? {
        guard let heard = membership as? any CrewHeardListProviding else { return nil }
        return CrewHeardListViewModel(heard: heard, membership: membership)
    }

    private var diagnosticsViewModel: CrewDiagnosticsViewModel? {
        guard let source = membership as? any CrewDiagnosticsProviding else { return nil }
        return CrewDiagnosticsViewModel(source: source)
    }

    var body: some View {
        List {
            Section {
                NavigationLink("Your puck (radio picker)") {
                    ConnectScreen(connect: connect, client: client, channelImport: channelImport,
                                  scanner: scanner, pairing: pairing, colorblind: colorblind)
                }
            }
            // §6.5: "Start a new crew (mints a fresh code; leaves the
            // current crew)" — reuses `CrewStartView`/`CrewController
            // .beginStart` unchanged; the only new behaviour is the
            // confirmation sheet's extra line when a crew is already
            // active (`CrewController.confirmationLines`'s own
            // `.start(_, _, .some)` case).
            Section {
                NavigationLink("Start a new crew") {
                    CrewStartNewCrewView(controller: controller, membership: membership,
                                         connect: connect, scanner: scanner)
                }
            }
            // §4.7/§6.5, scoped for this slice — `CrewHeardListProviding
            // .swift`'s own header comment on why this is crew-channel
            // overflow/hidden state rather than raw nodeDB strangers.
            if let heardListViewModel {
                Section {
                    NavigationLink("People my puck hears") {
                        CrewHeardListView(viewModel: heardListViewModel)
                    }
                }
            }
            // §6.5's "Crew diagnostics" — plain labels, numbers only
            // where they help; UNKNOWN is never rendered as 0
            // (`CrewDiagnosticsViewModel`'s own header comment).
            if let diagnosticsViewModel {
                Section {
                    LabeledContent("Channel", value: diagnosticsViewModel.channelLabel)
                    LabeledContent("Admitted", value: diagnosticsViewModel.admittedLabel)
                    LabeledContent("Refused", value: diagnosticsViewModel.refusedLabel)
                    ForEach(diagnosticsViewModel.refusalBreakdown, id: \.reason) { entry in
                        LabeledContent("  \u{2014} \(entry.reason)", value: "\(entry.count)")
                            .font(.caption)
                            .foregroundStyle(Color.ffMuted)
                    }
                    LabeledContent("Last admission", value: diagnosticsViewModel.lastAdmissionLabel)
                } header: {
                    Text("Crew diagnostics")
                } footer: {
                    // PR #313 review — `checksNote`'s own doc comment on
                    // why the numbers above carry a caveat rather than
                    // quietly meaning something other than they look.
                    if let note = diagnosticsViewModel.checksNote {
                        Text(note).font(.caption).foregroundStyle(Color.ffMuted)
                    }
                }
                .font(.system(.footnote, design: .monospaced))
                .foregroundStyle(Color.ffMuted)
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
            // §6.5's "Frequency band (region) -> the existing picker +
            // `setRegion`". Added by PR #308's review: this screen's own
            // navigation label promises "radio, frequency, invite link",
            // and "frequency" had nowhere to go. Same
            // `regionSelection`/`confirmRegion()` pair the §1.7 gate
            // uses — a default in a CONTROL, applied only on the tap,
            // and `confirmRegion()` itself refuses `.unset`.
            Section("Frequency band") {
                Picker("Region", selection: Bindable(controller).regionSelection) {
                    ForEach(Config.LoRaConfig.RegionCode.allCases.filter { $0 != .unset }, id: \.self) { region in
                        Text(String(describing: region).uppercased()).tag(region)
                    }
                }
                .pickerStyle(.menu)
                .tint(Color.ffAmber)
                if let error = controller.regionErrorMessage {
                    Text(error).font(.caption).foregroundStyle(Color.ffAlert)
                }
                Button(controller.isSettingRegion ? "SAVING\u{2026}" : "Save frequency band") {
                    Task { _ = await controller.confirmRegion() }
                }
                .buttonStyle(.bordered)
                .tint(Color.ffMuted)
                .foregroundStyle(Color.ffAmber)
                .disabled(controller.isSettingRegion)
                Text("Radios use different frequencies in different countries. Your puck already " +
                     "has one set \u{2014} only change this if you're travelling.")
                    .font(.caption)
                    .foregroundStyle(Color.ffMuted)
            }
            Section {
                if let code = controller.profile?.code, let parsed = try? CrewCode.parse(code) {
                    // §1.8 amendment (2026-09-14, bench finding): the
                    // exported link now carries the radio's CURRENT LoRa
                    // config (`--seturl` and the official apps' URL
                    // import REPLACE it, and an absent one writes the
                    // importing radio deaf — region UNSET, preset off).
                    // `controller.meshtasticURL` is `nil` exactly when
                    // that config isn't known yet or is itself `.unset`
                    // (same fact `regionIsUnset` already gates Start/
                    // Join on) — show the same honest blocker here
                    // instead of a link that would go on to break
                    // something.
                    if let url = controller.meshtasticURL(for: parsed) {
                        Button("Copy Meshtastic link") {
                            #if os(iOS)
                            UIPasteboard.general.string = url
                            #elseif os(macOS)
                            NSPasteboard.general.clearContents()
                            NSPasteboard.general.setString(url, forType: .string)
                            #endif
                        }
                        .buttonStyle(.bordered)
                        .tint(Color.ffMuted)
                        .foregroundStyle(Color.ffAmber)
                        Text("For other apps and for setting up a puck by hand. Whatever imports this " +
                             "link will use this crew as its main channel and turn its other channels off.")
                            .font(.caption)
                            .foregroundStyle(Color.ffMuted)
                    } else {
                        Text("Set the radio region first")
                            .foregroundStyle(Color.ffMuted)
                        Text("Your puck's frequency band isn't set yet, so this link can't safely carry " +
                             "it. Set it above, then come back here.")
                            .font(.caption)
                            .foregroundStyle(Color.ffMuted)
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
