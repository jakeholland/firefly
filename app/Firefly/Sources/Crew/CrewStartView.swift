//
//  CrewStartView.swift — Start a crew (`docs/specs/A02-crew-join.md`,
//  §2, artboard `CrewStart.dc.html`). Mints a code, applies it through
//  the existing admin write-back path (`CrewController`, which itself
//  reuses `ChannelImportViewModel`/`AdminWriteConfirmationSheet`), then
//  shows the code screen: QR, big mono code, Share link, Show on puck,
//  and the Joined list fed by an injected `CrewMembershipProviding`.
//
import FireflyMesh
import FireflyModel
import SwiftUI

struct CrewStartView: View {
    let controller: CrewController
    let membership: any CrewMembershipProviding
    /// Opens A02 §6.1's connect step (owner report, build 328).
    let onConnectPuck: () -> Void
    let onDone: () -> Void

    @State private var humanName = "My crew"
    @State private var hasBegun = false
    @State private var showConfirmation = false
    @State private var showOnPuckSheet = false
    /// Set once THIS Start flow's own `confirmApply()` succeeds — the
    /// code screen then reads `controller.profile` live (so a later
    /// in-place rename reflects immediately), but this flag is what
    /// gates showing it at all: `controller.profile` can already be
    /// non-nil on entry (Advanced → "Start a new crew" while already on
    /// one), and this view must not show a stale crew's code screen
    /// before its OWN mint has actually been confirmed.
    @State private var didConfirmStart = false

    var body: some View {
        Group {
            if controller.regionIsUnset {
                RegionGateView(controller: controller) {
                    Task { await begin() }
                }
            } else if didConfirmStart, let profile = controller.profile {
                codeScreen(profile: profile)
            } else {
                preflight
            }
        }
        .accessibilityIdentifier("Screen.CrewStart")
        .task {
            guard !hasBegun else { return }
            hasBegun = true
            // No puck, no attempt — and no spinner pretending one is in
            // flight. The banner below states why and offers the connect
            // step; `hasBegun` stays set so this cannot re-fire on every
            // redraw, and `.onChange(of:)` picks the flow back up the
            // instant a puck actually connects.
            if !controller.regionIsUnset, controller.hasConnectedRadio { await begin() }
        }
        // Coming back from the connect step (or a reconnect that happened
        // on its own) starts the mint without a second tap — the person
        // already asked for a crew.
        .onChange(of: controller.hasConnectedRadio) { _, nowConnected in
            guard nowConnected, !didConfirmStart, controller.pending == nil,
                  !controller.isBusy, !controller.regionIsUnset else { return }
            controller.clearFailure()
            Task { await begin() }
        }
        .sheet(isPresented: $showConfirmation) {
            AdminWriteConfirmationSheet(
                title: controller.confirmationTitle,
                primaryText: controller.confirmationPrimaryText,
                changes: controller.confirmationLines,
                technicalDetails: controller.confirmationTechnicalDetails,
                isBusy: controller.isApplying,
                errorMessage: controller.errorMessage,
                onConfirm: {
                    Task {
                        if await controller.confirmApply() {
                            didConfirmStart = true
                            showConfirmation = false
                        }
                    }
                },
                onCancel: {
                    controller.cancelPending()
                    showConfirmation = false
                    onDone()
                })
        }
        .sheet(isPresented: $showOnPuckSheet) {
            VStack(spacing: 16) {
                Text("On your puck").font(.headline)
                Text("SETTINGS → CREW → SHOW CODE")
                    .font(.system(.body, design: .monospaced))
                    .foregroundStyle(Color.ffAmber)
                Text("Your puck can always show the code on its own — nothing is sent from your phone.")
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
                    .multilineTextAlignment(.center)
                Button("Done") { showOnPuckSheet = false }
                    .buttonStyle(.borderedProminent)
                    .tint(Color.ffAmber)
                    .foregroundStyle(Color.ffBackground)
                    .frame(minHeight: 44)
            }
            .padding(24)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Color.ffBackground)
            .presentationDetents([.fraction(0.35)])
        }
    }

    /// What the screen shows before its own mint has been confirmed:
    /// the radio banner when there is no puck, otherwise the live
    /// progress line, plus any honest failure with TRY AGAIN.
    private var preflight: some View {
        VStack(spacing: 16) {
            if !controller.hasConnectedRadio {
                CrewNeedsRadioBanner(onConnect: onConnectPuck)
            } else if controller.failureMessage == nil {
                ProgressView()
                Text(controller.progressLabel ?? "Setting up your crew…")
                    .foregroundStyle(Color.ffMuted)
            }
            CrewApplyStatusView(
                controller: controller,
                // Same rule as Join: retrying cannot fix "no puck", so
                // the banner's CONNECT is the only action offered there.
                onRetry: controller.hasConnectedRadio ? { Task { controller.clearFailure(); await begin() } } : nil)
            Spacer(minLength: 0)
        }
        .padding(24)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .background(Color.ffBackground)
    }

    private func begin() async {
        if await controller.beginStart(humanName: humanName) {
            showConfirmation = true
        }
    }

    // MARK: - Code screen

    private func codeScreen(profile: CrewProfile) -> some View {
        let link = (try? CrewCode.parse(profile.code)).map { CrewLink.encode(code: $0, name: profile.humanName) }
            ?? "firefly://crew?v=1&code=\(profile.code)"

        // PR #308 review: the whole screen used to be ONE ScrollView
        // with "Done \u{00B7} go to Find" as its last child, which put
        // the primary action below the fold on an iPhone 17 Pro (it was
        // clipped by the home indicator in this PR's own screenshots).
        // Content scrolls; the button is pinned in a safe-area bar that
        // is always on screen, which is also the only arrangement that
        // survives a long crew name or a nine-row Joined list.
        return VStack(spacing: 0) {
            ScrollView {
            VStack(spacing: 20) {
                VStack(spacing: 4) {
                    TextField("Crew name", text: Binding(
                        get: { profile.humanName },
                        set: { controller.rename(humanName: $0) }))
                        .font(.title2.weight(.bold))
                        .foregroundStyle(Color.ffInk)
                        .multilineTextAlignment(.center)
                    Text("Your crew · tap the name to change it")
                        .font(.caption)
                        .foregroundStyle(Color.ffMuted)
                }

                CrewQRCodeView(text: link)
                    .frame(width: 220, height: 220)

                Text(profile.code)
                    .font(.system(.title, design: .monospaced).weight(.bold))
                    .foregroundStyle(Color.ffInk)
                    .textSelection(.enabled)

                Text("Show this, or read the code out loud. Anyone who scans or types it is in your crew.")
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
                    .multilineTextAlignment(.center)

                HStack(spacing: 12) {
                    ShareLink(item: "Join my Firefly crew: \(profile.code)\n\(link)") {
                        Label("Share link", systemImage: "square.and.arrow.up")
                            .frame(maxWidth: .infinity, minHeight: 44)
                    }
                    .buttonStyle(.bordered)
                    .tint(Color.ffMuted)
                    .foregroundStyle(Color.ffAmber)

                    Button {
                        showOnPuckSheet = true
                    } label: {
                        Label("Show on puck", systemImage: "dot.radiowaves.left.and.right")
                            .frame(maxWidth: .infinity, minHeight: 44)
                    }
                    .buttonStyle(.bordered)
                    .tint(Color.ffMuted)
                    .foregroundStyle(Color.ffAmber)
                }

                joinedSection
            }
            .padding(24)
            }
            .background(Color.ffBackground)

            VStack(spacing: 0) {
                Divider().overlay(Color.ffDim)
                Button(action: onDone) {
                    // The frame belongs on the LABEL: a `.frame` applied
                    // after `.buttonStyle` widens the button's slot, not
                    // the bordered-prominent capsule inside it, so the
                    // control stays hug-width. Same shape every other
                    // full-width primary in this app uses
                    // (`CrewWelcomeView`).
                    Text("Done · go to Find")
                        .font(.system(.headline, design: .rounded).weight(.bold))
                        .frame(maxWidth: .infinity, minHeight: 48)
                }
                    .buttonStyle(.borderedProminent)
                    .tint(Color.ffAmber)
                    .foregroundStyle(Color.ffBackground)
                    .padding(.horizontal, 24)
                    .padding(.top, 12)
            }
            .background(Color.ffBackground)
        }
        .background(Color.ffBackground)
        // The bar sits ABOVE the home indicator, never under it.
        .safeAreaPadding(.bottom, 12)
    }

    private var joinedSection: some View {
        let members = membership.currentMembers()
        return VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("Joined · \(members.count)")
                    .font(.headline)
                    .foregroundStyle(Color.ffInk)
                Spacer()
                Text("updates live").font(.caption).foregroundStyle(Color.ffMuted)
            }
            if members.isEmpty {
                Text("Nobody has scanned your code yet.")
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
            }
            ForEach(members) { member in
                HStack {
                    Text(CrewCopy.displayName(member.displayName))
                        .foregroundStyle(Color.ffInk)
                    Spacer()
                    // The same pill the Crew page and the Inbox render
                    // (PR #304's vocabulary). The old flat "IN" chip
                    // said the same thing about a member heard 40
                    // minutes ago as one heard just now.
                    PresencePill(presence: CrewCopy.tag(for: member.heardPresence),
                                 age: member.heardAgeMs.map { TimeInterval($0) / 1000 })
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16)
        .background(Color.ffSurface)
        .clipShape(RoundedRectangle(cornerRadius: 12))
    }
}

/// §6.5's "Start a new crew" — the Advanced-only entry point, PUSHED
/// (not `fullScreenCover`d like the first-launch onboarding container)
/// since this is reached from deep inside the app, already on a crew.
/// A thin wrapper, not a fork: `CrewStartView` itself is unchanged and
/// does not know whether it was reached from onboarding or from here —
/// the only Advanced-specific behaviour is `onDone` popping this screen
/// with `dismiss()` instead of tearing down a modal cover.
struct CrewStartNewCrewView: View {
    let controller: CrewController
    let membership: any CrewMembershipProviding
    /// The same two the first-launch flow uses — this entry point needs
    /// the connect step too: Advanced is reachable with the puck
    /// disconnected, and minting a new crew is the same write.
    let connect: ConnectViewModel
    let scanner: (any NodeScanning)?

    @Environment(\.dismiss) private var dismiss
    @State private var showConnect = false
    /// Constructed up front, exactly as `ConnectScreen` and
    /// `CrewOnboardingContainer` do: `MeshPeripheralDiscovery.init` only
    /// stores the scanner — nothing touches CoreBluetooth until
    /// `startScanning()`, which only the connect step's own `.task`
    /// calls.
    @State private var discovery: any PeripheralDiscovering

    init(controller: CrewController, membership: any CrewMembershipProviding,
         connect: ConnectViewModel, scanner: (any NodeScanning)?) {
        self.controller = controller
        self.membership = membership
        self.connect = connect
        self.scanner = scanner
        _discovery = State(initialValue: scanner.map { MeshPeripheralDiscovery(scanner: $0) }
                            ?? StubPeripheralDiscovery())
    }

    var body: some View {
        CrewStartView(controller: controller, membership: membership,
                      onConnectPuck: { showConnect = true },
                      onDone: { dismiss() })
            .sheet(isPresented: $showConnect) {
                CrewConnectPuckView(
                    connect: connect,
                    discovery: discovery,
                    isRadioUsable: { controller.hasConnectedRadio },
                    onConnected: { showConnect = false },
                    onSkip: { showConnect = false })
            }
    }
}
