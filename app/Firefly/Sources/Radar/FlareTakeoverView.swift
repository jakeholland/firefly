//
//  FlareTakeoverView.swift — M2: the inbound-FLARE full-screen takeover
//  (docs/specs/S10-flare.md's "Receive" behavior). Composited by
//  `RootView` above every destination, exactly the way `DemoBadge` is —
//  see that file's own header comment for why a full-screen overlay is
//  a real layout layer here, not a floating `.overlay`.
//
import FireflyModel
import SwiftUI

struct FlareTakeoverView: View {
    @Bindable var model: FlareTakeoverViewModel

    var body: some View {
        TimelineView(.periodic(from: .now, by: 1)) { context in
            VStack(spacing: 20) {
                Spacer()

                Image(systemName: "flame.fill")
                    .font(.system(size: 56))
                    .foregroundStyle(Color(fireflyHex: senderColorHex))

                Text("FLARE")
                    .font(.system(.largeTitle, design: .rounded).weight(.heavy))
                    .foregroundStyle(Color.ffAlert)
                    .tracking(2)

                Text("\(model.senderName.uppercased()) WANTS YOU TO COME FIND THEM")
                    .font(.system(.title3, design: .rounded).weight(.bold))
                    .foregroundStyle(Color.ffInk)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 24)

                bearingLine

                Text("auto-dismisses in \(model.remainingSeconds(now: context.date))s")
                    .font(.system(.footnote, design: .monospaced))
                    .foregroundStyle(Color.ffMuted)

                Spacer()

                Button("DISMISS") { model.dismiss() }
                    .buttonStyle(.borderedProminent)
                    .tint(Color(fireflyHex: senderColorHex))
                    .foregroundStyle(Color.ffBackground)
                    .font(.system(.headline, design: .rounded).weight(.bold))
                    .frame(minWidth: 200, minHeight: 44)
                    .padding(.bottom, 40)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Color.ffBackground.ignoresSafeArea())
        }
        .accessibilityAddTraits(.isModal)
    }

    private var senderColorHex: UInt32 {
        RadarCrewPalette.hex(index: model.senderColorIndex, colorblind: false)
    }

    /// S10's own honest vocabulary: a real bearing/distance when both
    /// positions are known, otherwise a plain admission that neither is
    /// — never a fabricated arrow.
    @ViewBuilder
    private var bearingLine: some View {
        if let distanceText = model.distanceText, let compassPoint = model.compassPoint {
            Text("\(distanceText) \(compassPoint) of you")
                .font(.system(.title2, design: .monospaced).weight(.bold))
                .foregroundStyle(Color.ffAmber)
        } else if let reason = model.noBearingReason {
            Text("no bearing (\(reason))")
                .font(.system(.callout, design: .monospaced))
                .foregroundStyle(Color.ffMuted)
        } else {
            Text("no bearing — position not known for both of you")
                .font(.system(.callout, design: .monospaced))
                .foregroundStyle(Color.ffMuted)
        }
    }
}
