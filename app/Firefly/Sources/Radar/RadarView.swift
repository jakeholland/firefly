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
    /// `SettingsViewModel.colorblindPalette`, read fresh by `RootView`
    /// on every redraw (M2) and synced onto `model.colorblind` below —
    /// this view model's own doc comment: "wiring it to `SettingsStoring`
    /// once slice C lands is a follow-up", which this is.
    let colorblind: Bool

    init(model: RadarViewModel, colorblind: Bool) {
        _model = State(initialValue: model)
        self.colorblind = colorblind
    }

    // There is deliberately NO no-argument `init()` any more. The one
    // this screen used to have called `AppDependencies.current()` for
    // itself, which built a SECOND `MeshtasticClient` over a SECOND
    // `BLETransport` every time the Radar destination was shown — so
    // the screen observed a client nothing ever connected. The view
    // model now comes from the app's single `AppGraph`
    // (`FireflyApp.init` -> `RootView.radar`), which is also what makes
    // FIND's pings go out over the same radio the Connect screen
    // connected.

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
            // M3: one identifying accessibility identifier per screen —
            // see `ConnectScreen`'s own comment.
            .accessibilityIdentifier("Screen.Radar")
        }
        .background(Color.ffBackground)
        // `model.observe()`/`model.stopObserving()` are deliberately NOT
        // called here any more — `AppGraph.makeRadarViewModel(haptics:)`
        // starts `observe()` once, for the life of the graph, the same
        // fix `ConnectScreen.swift`'s own `.onAppear` comment documents
        // for the identical NavigationSplitView detail-column remount
        // hazard (this screen is one of `RootView`'s own `detail(for:)`
        // destinations, same as Connect). Tying heading/location
        // mirroring to THIS screen's appear/disappear risked the exact
        // same permanent-orphan bug — a remount firing `.onDisappear`
        // once with no matching `.onAppear` ever following it again.
        .onChange(of: colorblind, initial: true) { _, newValue in model.colorblind = newValue }
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
        case .noSel: return .ffMuted
        case .noFix: return .ffAmber
        case .noHdg: return .radarStaleAmber
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
