//
//  RadarView.swift — Slice D's Radar screen: every `radar_mode_t`.
//
//  Spec: docs/specs/A01-companion-app.md ("Slice D — Radar"),
//  docs/specs/S06-radar-face.md, docs/specs/S29-radio-only.md.
//
//  This view renders `RadarViewModel.snapshot` and its derived display
//  strings verbatim — it invents no geometry, no distance, no bearing.
//  The whole "which words for which mode" decision lives in the view
//  model (MVVM convention #5: state -> strings happens THERE), so this
//  file is only layout.
//
import FireflyModel
import SwiftUI

struct RadarView: View {
    @State private var model: RadarViewModel
    @State private var showingFind = false

    init(model: RadarViewModel) {
        _model = State(initialValue: model)
    }

    /// The composition root for this screen: `AppDependencies.current()`
    /// supplies the already-landed heading/location seams (S5); the
    /// radar/FIND computation itself is `MockRadarComputing`/
    /// `MockFindSession` until slice B's real bridge replaces them (see
    /// `RadarViewModel.swift`'s top comment) — swapped in at
    /// `RadarViewModel.live(dependencies:)` alone, nothing here changes
    /// when that happens.
    init() {
        let dependencies = AppDependencies.current()
        #if os(iOS)
        let haptics: any HapticSignaling = UIKitHapticSignaling()
        #else
        let haptics: any HapticSignaling = NoHapticSignaling()
        #endif
        _model = State(initialValue: .live(dependencies: dependencies, haptics: haptics))
    }

    var body: some View {
        ScrollView {
            VStack(spacing: 20) {
                ring
                textStack
                positionLines
                findAffordance
            }
            .padding()
            .frame(maxWidth: .infinity)
        }
        .background(Color.ffBackground)
        .onAppear { model.observe() }
        .onDisappear { model.stopObserving() }
        .sheet(isPresented: $showingFind) {
            FindPanel(model: model)
        }
    }

    private var ring: some View {
        ZStack {
            RadarRingView(snapshot: model.snapshot, colorblind: model.colorblind)
            if model.snapshot.mode == .close {
                ClosePulseRings()
            }
        }
        .frame(width: 280, height: 280)
        .contentShape(Circle())
        .onTapGesture { model.cycleSelection() }
        .accessibilityLabel("Radar. Tap to cycle selected friend.")
    }

    private var textStack: some View {
        VStack(spacing: 6) {
            Text(model.chipText)
                .font(.system(.headline, design: .rounded))
                .foregroundStyle(chipColor)
                .multilineTextAlignment(.center)

            if let sub = model.subheadline {
                Text(sub)
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
                    .multilineTextAlignment(.center)
            }

            if let hint = model.bearingHintText {
                Text(hint)
                    .font(.system(.subheadline, design: .monospaced))
                    .foregroundStyle(Color.radarStaleAmber)
            }

            if !model.primaryReadoutText.isEmpty {
                Text(model.primaryReadoutText)
                    .font(.system(.title, design: .monospaced))
                    .foregroundStyle(model.snapshot.distanceImprecise ? Color.ffMuted : Color.ffInk)
            }

            if model.showsTrendChip {
                Text(model.trendLabel)
                    .font(.caption.bold())
                    .padding(.horizontal, 10)
                    .padding(.vertical, 4)
                    .background(trendColor.opacity(0.2))
                    .foregroundStyle(trendColor)
                    .clipShape(Capsule())
            }

            if model.snapshot.mode == .signal, !model.snapshot.signalDots.isEmpty {
                Text(RadarViewModel.signalRingDisclaimer)
                    .font(.caption2)
                    .foregroundStyle(Color.ffMuted)
            }
        }
    }

    /// A01 slice D acceptance criterion: "every position on screen shows
    /// its source and its age." Three independent lines: the selected
    /// friend's position (nil when nothing is honestly knowable yet),
    /// their radio evidence (S29, nil when never heard), and the phone's
    /// own reading — always present, even when both halves say
    /// "unavailable".
    private var positionLines: some View {
        VStack(alignment: .leading, spacing: 4) {
            if let line = model.theirPositionLine {
                Text(line).font(.caption).foregroundStyle(Color.ffMuted)
            }
            if let line = model.theirSignalLine {
                Text(line).font(.caption).foregroundStyle(Color.ffMuted)
            }
            Text(model.myPositionLine).font(.caption).foregroundStyle(Color.ffMuted)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var findAffordance: some View {
        Button {
            showingFind = true
        } label: {
            Label(model.isFindActive ? "FIND — \(model.findPingCount) sent" : "FIND",
                  systemImage: "dot.radiowaves.left.and.right")
        }
        .buttonStyle(.bordered)
        .tint(.ffAmber)
        .frame(minWidth: 44, minHeight: 44)
        .contentShape(Rectangle())
        .disabled(model.findTargetNodeID == nil && !model.isFindActive)
    }

    private var chipColor: Color {
        switch model.snapshot.mode {
        case .nosel: return .ffMuted
        case .nofix: return .ffAmber
        case .nohdg: return .radarStaleAmber
        case .live: return .ffLiveGreen
        case .stale: return .radarStaleAmber
        case .lost: return .ffMuted
        case .place: return .ffInk
        case .close: return .ffLiveGreen
        case .signal:
            switch model.snapshot.signalTier {
            case .strong, .good: return .ffLiveGreen
            case .weak: return .radarStaleAmber
            case .faint, .none: return .ffMuted
            }
        }
    }

    private var trendColor: Color {
        switch model.snapshot.trend {
        case ..<0: return .radarStaleAmber
        case 0: return .ffMuted
        default: return .ffLiveGreen
        }
    }
}
