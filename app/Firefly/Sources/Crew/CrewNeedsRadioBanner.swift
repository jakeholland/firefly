//
//  CrewNeedsRadioBanner.swift — the persistent "Connect your puck to
//  join" banner Start and Join both show whenever no puck is connected
//  (owner report, 2026-09-14, build 328).
//
//  Persistent, not a toast and not a post-tap error: the reason the
//  primary action is disabled has to be on screen BEFORE the tap, or the
//  disabled button is just a button that does nothing — which is the
//  thing this whole change exists to stop. Its CONNECT button opens the
//  same `CrewConnectPuckView` step the first-launch flow uses; there is
//  no second connect path.
//
import SwiftUI

struct CrewNeedsRadioBanner: View {
    let onConnect: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .top, spacing: 10) {
                Image(systemName: "dot.radiowaves.left.and.right")
                    .foregroundStyle(Color.ffAmber)
                    .padding(.top, 2)
                VStack(alignment: .leading, spacing: 4) {
                    Text(CrewController.needRadioBannerTitle)
                        .font(.headline)
                        .foregroundStyle(Color.ffInk)
                        .accessibilityIdentifier("CrewBanner.NeedsRadio")
                    Text(CrewController.needRadioBannerDetail)
                        .font(.footnote)
                        .foregroundStyle(Color.ffMuted)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Spacer(minLength: 0)
            }
            Button(action: onConnect) {
                Text("CONNECT")
                    .font(.system(.subheadline, design: .rounded).weight(.bold))
                    .frame(maxWidth: .infinity, minHeight: 44)
            }
            .buttonStyle(.borderedProminent)
            .tint(Color.ffAmber)
            .foregroundStyle(Color.ffBackground)
            .accessibilityIdentifier("CrewBanner.Connect")
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.ffSurface)
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(Color.ffAmber.opacity(0.5), lineWidth: 1))
    }
}

/// The progress/failure block Start and Join share — one place, so the
/// two screens can never drift into saying different things about the
/// same `CrewController.ApplyPhase`.
struct CrewApplyStatusView: View {
    let controller: CrewController
    let onRetry: (() -> Void)?

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            if let progress = controller.progressLabel {
                HStack(spacing: 8) {
                    if controller.isBusy { ProgressView().controlSize(.small) }
                    Text(progress)
                        .font(.footnote)
                        .foregroundStyle(Color.ffMuted)
                        .accessibilityIdentifier("CrewApply.Progress")
                }
            }
            if let failure = controller.failureMessage {
                Text(failure)
                    .font(.footnote)
                    .foregroundStyle(Color.ffAlert)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityIdentifier("CrewApply.Failure")
                if let onRetry {
                    Button("TRY AGAIN", action: onRetry)
                        .buttonStyle(.bordered)
                        .tint(Color.ffAmber)
                        .frame(minHeight: 44)
                        .accessibilityIdentifier("CrewApply.Retry")
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}
