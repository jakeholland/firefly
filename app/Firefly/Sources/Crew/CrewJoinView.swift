//
//  CrewJoinView.swift — Join a crew (`docs/specs/A02-crew-join.md`, §3,
//  artboard `CrewJoin.dc.html`). Camera scan (iOS, reusing
//  `QRScannerSheet` inline) or a typed six-box code, live-canonicalised
//  as you type; either path lands on ONE confirmation sheet, then the
//  same apply path Start uses.
//
import FireflyModel
import SwiftUI

struct CrewJoinView: View {
    let controller: CrewController
    /// Set when this screen was reached via `onOpenURL` (§1.8) — a
    /// deep-linked code/link that should stage a confirmation
    /// immediately rather than waiting for a scan or a typed code.
    var initialPayload: CrewScanPayload?
    /// Opens A02 §6.1's connect step (owner report, build 328). The
    /// container owns the push; this screen only says when.
    let onConnectPuck: () -> Void
    /// A `meshtastic.org/e/#…`/`meshtastic://e/#…` scan (§3.1 shape 3) is
    /// handed to the EXISTING channel-import flow, not this screen's own
    /// apply path — the caller (e.g. `RootView`) owns that sheet.
    let onMeshtasticLink: (String) -> Void
    let onDone: () -> Void

    @State private var typedCode = ""
    @State private var showConfirmation = false
    @State private var scanMessage: String?
    @State private var isJoined = false
    @State private var handledInitialPayload = false
    /// The payload the last attempt used — scanned, deep-linked or
    /// typed — so TRY AGAIN retries THAT, not a re-derivation of it
    /// from the text field (which is empty on the scan path).
    @State private var lastPayload: CrewScanPayload?

    private var canJoin: Bool { (try? CrewCode.parse(typedCode)) != nil }

    /// `nil` means JOIN is live. Anything else is both the disable AND
    /// the sentence shown next to it — one source, so the two can never
    /// disagree.
    private var joinDisabledReason: String? {
        if !controller.hasConnectedRadio { return CrewController.needRadioMessage }
        // While a join is in flight the reason is already on screen, in
        // `CrewApplyStatusView`'s progress line — printing it twice
        // under the button would just be the same sentence, twice.
        if controller.isBusy { return nil }
        if !canJoin { return "Type the six characters after FIRE-." }
        return nil
    }

    /// Disabled and "has a reason to show" are deliberately separate:
    /// the button is also dead while a write is in flight, which
    /// `joinDisabledReason` above returns `nil` for on purpose.
    private var isJoinDisabled: Bool {
        !controller.hasConnectedRadio || controller.isBusy || !canJoin
    }

    /// TRY AGAIN after a failed attempt: re-run whichever payload got us
    /// here. A failure never leaves the typed code behind, so this is
    /// always the same attempt, not a new one the user has to retype.
    private var retryAction: (() -> Void)? {
        // Nothing to retry until an attempt has actually been made, and
        // nothing retrying can fix while there is no puck — the banner's
        // own CONNECT is the action in that case, and a TRY AGAIN next
        // to it would just be a second button that fails.
        guard lastPayload != nil, controller.hasConnectedRadio else { return nil }
        return retryLastAttempt
    }

    private func retryLastAttempt() {
        guard let payload = lastPayload else { return }
        controller.clearFailure()
        Task { await handle(payload: payload) }
    }

    var body: some View {
        Group {
            if controller.regionIsUnset {
                RegionGateView(controller: controller) {}
            } else if isJoined {
                joinedConfirmation
            } else {
                joinForm
            }
        }
        .accessibilityIdentifier("Screen.CrewJoin")
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
                            showConfirmation = false
                            isJoined = true
                        }
                    }
                },
                onCancel: {
                    controller.cancelPending()
                    showConfirmation = false
                })
        }
        .task {
            guard !handledInitialPayload, let initialPayload else { return }
            handledInitialPayload = true
            await handle(payload: initialPayload)
        }
        // The controller is the source of truth for "did this actually
        // join", not this view's own tap bookkeeping: a join can also be
        // completed by the bench seam (`-FireflyDebugJoinCrew`) while
        // this screen is up, and a screen showing a live JOIN form under
        // a crew that is already joined would be lying about state it
        // can see.
        .onChange(of: controller.phase) { _, newPhase in
            if newPhase == .joined { isJoined = true }
        }
    }

    private var joinForm: some View {
        // Scrolls for the same reason `CrewStartView`'s code screen does
        // (PR #308 review): camera card + six-box field + up to three
        // message lines + JOIN does not fit every device with a
        // keyboard up.
        ScrollView {
        VStack(spacing: 24) {
            VStack(spacing: 4) {
                Text("Join a crew").font(.title2.weight(.bold)).foregroundStyle(Color.ffInk)
                Text("Scan the code your friend is showing")
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
            }

            if !controller.hasConnectedRadio {
                CrewNeedsRadioBanner(onConnect: onConnectPuck)
            }

            #if os(iOS)
            CrewScannerCard { payload in
                Task { await handleScan(payload) }
            }
            .frame(height: 220)
            #endif

            VStack(spacing: 8) {
                Text("or type the code").font(.footnote).foregroundStyle(Color.ffMuted)
                HStack(spacing: 4) {
                    Text("FIRE-").font(.system(.title3, design: .monospaced)).foregroundStyle(Color.ffMuted)
                    TextField("000000", text: Binding(
                        get: { typedCode },
                        set: { newValue in typedCode = Self.liveCanonicalize(newValue) }))
                        .font(.system(.title3, design: .monospaced).weight(.bold))
                        .foregroundStyle(Color.ffInk)
                        .disableAutocorrection(true)
                        #if os(iOS)
                        .textInputAutocapitalization(.characters)
                        .keyboardType(.asciiCapable)
                        #endif
                }
                .padding(12)
                .background(Color.ffSurface)
                .clipShape(RoundedRectangle(cornerRadius: 10))
            }

            if let scanMessage {
                Text(scanMessage).font(.footnote).foregroundStyle(Color.ffAlert)
            }
            if let rejoinMessage = controller.rejoinOwnCrewMessage {
                Text(rejoinMessage).font(.footnote).foregroundStyle(Color.ffMuted)
            }

            // Progress ("Writing to your puck… -> Checking… -> Joined")
            // and every honest failure — a puck that disconnected
            // mid-write, a NAK, a timeout, a read-back mismatch — with
            // TRY AGAIN. `controller.errorMessage` is deliberately NOT
            // printed a second time alongside this: it carries the same
            // sentence `failureMessage` does.
            CrewApplyStatusView(controller: controller, onRetry: retryAction)

            Button {
                guard let code = try? CrewCode.parse(typedCode) else { return }
                Task { await handle(payload: .bareCode(code)) }
            } label: {
                Text("JOIN")
                    .font(.system(.headline, design: .rounded).weight(.bold))
                    .frame(maxWidth: .infinity, minHeight: 48)
            }
            .buttonStyle(.borderedProminent)
            .tint(Color.ffAmber)
            .foregroundStyle(Color.ffBackground)
            .disabled(isJoinDisabled)
            .accessibilityIdentifier("CrewJoin.Join")

            // The reason is on screen WITH the disabled button, always —
            // never discovered by tapping it. Build 328 shipped a JOIN
            // that stayed enabled with no radio and did nothing; a
            // disabled button with no stated reason would be the same
            // bug with a greyer button.
            if let reason = joinDisabledReason {
                Text(reason)
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .accessibilityIdentifier("CrewJoin.DisabledReason")
            }

            Spacer(minLength: 0)
        }
        .padding(24)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color.ffBackground)
        .scrollDismissesKeyboard(.interactively)
    }

    private var joinedConfirmation: some View {
        VStack(spacing: 16) {
            Image(systemName: "checkmark.circle.fill")
                .font(.system(size: 48))
                .foregroundStyle(Color.ffLiveGreen)
            Text("You're in \(controller.profile?.humanName ?? "the crew")")
                .font(.title3.weight(.bold))
                .foregroundStyle(Color.ffInk)
            Button(action: onDone) {
                Text("Done · go to Find")
                    .font(.system(.headline, design: .rounded).weight(.bold))
                    .frame(maxWidth: .infinity, minHeight: 48)
            }
            .buttonStyle(.borderedProminent)
            .tint(Color.ffAmber)
            .foregroundStyle(Color.ffBackground)
        }
        .padding(24)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color.ffBackground)
    }

    private func handleScan(_ payload: String) async {
        let classified = CrewScanPayload.classify(payload)
        await handle(payload: classified)
    }

    private func handle(payload: CrewScanPayload) async {
        scanMessage = nil
        switch payload {
        case .meshtasticChannelLink(let url):
            onMeshtasticLink(url)
        case .unrecognized:
            scanMessage = "That's not a Firefly crew code."
        case .crewLink, .bareCode:
            lastPayload = payload
            // Review of PR #319: put the code a SCAN produced into the
            // field. Without this, a scan made with no puck connected
            // is refused, the banner sends the user to the connect
            // step, and coming back leaves JOIN disabled saying "Type
            // the six characters after FIRE-." — the code they scanned
            // silently gone, which is the same "my tap did nothing"
            // this change exists to stop. With it, the code is on
            // screen the whole time and JOIN goes live the moment a
            // puck connects. A typed code sets `typedCode` already, so
            // this only ever re-states what the user just supplied.
            if let code = Self.code(of: payload) { typedCode = code.symbols }
            if await controller.beginJoin(payload: payload) {
                showConfirmation = true
            }
        }
    }

    /// The crew code inside a scan/deep-link payload, for the two cases
    /// that carry one.
    private static func code(of payload: CrewScanPayload) -> CrewCode? {
        switch payload {
        case .bareCode(let code): return code
        case .crewLink(let link): return link.code
        case .meshtasticChannelLink, .unrecognized: return nil
        }
    }

    /// §3.2: canonicalises on every keystroke so `i`/`o` visibly become
    /// `1`/`0` as the user types, capped at 6 resulting symbols.
    private static func liveCanonicalize(_ raw: String) -> String {
        var mapped = ""
        for character in raw.uppercased() where !character.isWhitespace && character != "-" {
            switch character {
            case "I", "L": mapped.append("1")
            case "O": mapped.append("0")
            default:
                if CrewCodeAlphabet.symbols.contains(character) { mapped.append(character) }
            }
            if mapped.count == 6 { break }
        }
        return mapped
    }
}

#if os(iOS)
/// Wraps `QRScannerViewController` (`QRScannerSheet.swift`) INLINE
/// (§3.1: "reuses the existing `QRScannerSheet`, inline rather than
/// modal") rather than presenting it as a `.sheet` the way Connect's
/// own channel scanner does.
private struct CrewScannerCard: View {
    let onScanned: (String) -> Void

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 16).fill(Color.black)
            InlineQRScannerRepresentable(onScanned: onScanned)
                .clipShape(RoundedRectangle(cornerRadius: 16))
            VStack {
                Spacer()
                Text("Camera · point at the QR")
                    .font(.caption)
                    .foregroundStyle(.white)
                    .padding(.bottom, 8)
            }
        }
    }
}

private struct InlineQRScannerRepresentable: UIViewControllerRepresentable {
    let onScanned: (String) -> Void

    func makeUIViewController(context: Context) -> QRScannerViewController {
        QRScannerViewController(onScanned: onScanned)
    }

    func updateUIViewController(_ uiViewController: QRScannerViewController, context: Context) {}
}
#endif
