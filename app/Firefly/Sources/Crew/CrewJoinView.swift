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

    private var canJoin: Bool { (try? CrewCode.parse(typedCode)) != nil }

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
                changes: controller.confirmationLines,
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
                },
                technicalDetails: controller.confirmationTechnicalDetails)
        }
        .task {
            guard !handledInitialPayload, let initialPayload else { return }
            handledInitialPayload = true
            await handle(payload: initialPayload)
        }
    }

    private var joinForm: some View {
        VStack(spacing: 24) {
            VStack(spacing: 4) {
                Text("Join a crew").font(.title2.weight(.bold)).foregroundStyle(Color.ffInk)
                Text("Scan the code your friend is showing")
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
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
            if let error = controller.errorMessage {
                Text(error).font(.footnote).foregroundStyle(Color.ffAlert)
            }

            Button("JOIN") {
                guard let code = try? CrewCode.parse(typedCode) else { return }
                Task { await handle(payload: .bareCode(code)) }
            }
            .buttonStyle(.borderedProminent)
            .tint(Color.ffAmber)
            .foregroundStyle(Color.ffBackground)
            .disabled(!canJoin || controller.isBusy)
            .frame(maxWidth: .infinity, minHeight: 48)

            Spacer()
        }
        .padding(24)
        .background(Color.ffBackground)
    }

    private var joinedConfirmation: some View {
        VStack(spacing: 16) {
            Image(systemName: "checkmark.circle.fill")
                .font(.system(size: 48))
                .foregroundStyle(Color.ffLiveGreen)
            Text("You're in \(controller.profile?.humanName ?? "the crew")")
                .font(.title3.weight(.bold))
                .foregroundStyle(Color.ffInk)
            Button("Done · go to Find", action: onDone)
                .buttonStyle(.borderedProminent)
                .tint(Color.ffAmber)
                .foregroundStyle(Color.ffBackground)
                .frame(maxWidth: .infinity, minHeight: 48)
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
            if await controller.beginJoin(payload: payload) {
                showConfirmation = true
            }
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
