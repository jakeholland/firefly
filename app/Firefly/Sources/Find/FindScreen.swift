//
//  FindScreen.swift — owner decision (2026-09-13, docs/specs/
//  A01-companion-app.md "Navigation"): combine the old Radar and Map
//  tabs into ONE tab, Find, with a segmented control — Radar · Map ·
//  Field, Radar the default — instead of two separate tab-bar
//  destinations plus Map's own internal Field/GPS toggle.
//
//  Reuses `RadarView`/`RadarViewModel` and `MapTabView`/`MapViewModel`
//  UNCHANGED — this file is only the segmented control plus the
//  start/stop wiring that used to be implicit in "which tab is
//  selected" and is now explicit, because two screens' worth of pumps
//  now share one tab. See `FindSegment.swift`'s own header comment for
//  the full start/stop rule, including why Radar's background/
//  foreground lifecycle stays owned by `AppGraph`, untouched, rather
//  than duplicated here.
//
//  `MapTabView`'s own internal Field/GPS segmented control — the
//  "now-redundant inner toggle" the owner's brief calls out — is gone;
//  `segment` there is now a plain external parameter this file drives
//  from the ONE segmented control below.
//
//  This file's own `onAppear`/`onDisappear`/`.onChange(of: segment)`
//  (not each segment's own view appearing/disappearing) is what drives
//  `FindLifecycle.apply`/`.stopAll` — deliberately, so the only mount/
//  unmount pairing this depends on is a plain `@State` change while
//  ALREADY mounted (a completely ordinary SwiftUI mechanism, unrelated
//  to view identity), not a second `NavigationSplitView` detail-column
//  transition. `RadarView` itself is untouched — it still has no
//  `.onAppear`/`.onDisappear` of its own, same as before this tab
//  existed.
//
import FireflyModel
import SwiftUI

extension RadarViewModel: FindSegmentObserving {}
extension MapViewModel: FindSegmentObserving {}

struct FindScreen: View {
    let radar: RadarViewModel
    let map: MapViewModel
    @Binding var segment: FindSegment
    /// `SettingsViewModel.colorblindPalette`, read fresh by `RootView`
    /// on every redraw — same convention `RadarView`/`MapTabView`
    /// already followed individually before this file existed.
    let colorblind: Bool
    /// Forwarded straight through to `MapTabView`'s GPS pin cards —
    /// see `RootView.detail(for:)`'s own comment on what each one does
    /// (FIND switches this file's own `segment` to `.radar`; MESSAGE
    /// switches the OUTER tab to Inbox, which only `RootView` can do).
    let onFind: (UInt32) -> Void
    let onMessage: (UInt32) -> Void

    var body: some View {
        VStack(spacing: 0) {
            segmentedControl
            content
        }
        .background(Color.ffBackground)
        // BUG FIX (this PR): the Map segment's `Map` (MapKit) is a
        // scrollable-content view, and the instant one of THOSE
        // appears anywhere under `RootView`'s `NavigationStack { Find }`,
        // iOS commits to reserving a REAL navigation-bar frame for that
        // stack — even though nothing here ever sets a title — where it
        // previously rendered it at zero height. Radar/Field have no
        // such content, so they never trigger this; Map does, every
        // time. That extra bar renders ABOVE this VStack, so the
        // segmented control — this VStack's own FIRST child, laid out
        // identically in all three cases — visibly lands lower only
        // when Map is the segment on screen, with the reserved bar's
        // own (black) background showing through above it.
        //
        // Measured, not guessed: swapping `GPSMapView`'s `Map` for a
        // plain `Color` made the control land at the SAME y as Radar/
        // Field's; restoring `Map` reproduced the ~64pt drop every
        // time — isolating the trigger to `Map`'s mere presence, not
        // this file's own layout (unchanged across all three cases) or
        // any safe-area/`ignoresSafeArea` handling on the map content
        // itself (tried first; no effect, because the extra space is a
        // real navigation-bar frame, not a safe-area accounting quirk).
        //
        // `.toolbar(.hidden, for: .navigationBar)` forces that bar to
        // zero height regardless of what any segment's content wants,
        // which is what Radar/Field already got "for free" from having
        // nothing scrollable — this just makes the SAME thing true when
        // Map is showing, so the control's position stops depending on
        // which segment is selected. iOS-only (`.navigationBar` is
        // unavailable as a `ToolbarPlacement` on macOS, which has no
        // such bar to hide) — same guard `LineupScreen.swift` already
        // uses for its own, unrelated instance of this exact
        // "scrollable content quietly grows a navigation bar" bug.
        #if os(iOS)
        .toolbar(.hidden, for: .navigationBar)
        #endif
        // M3 convention: one identifying accessibility identifier per
        // screen (`ConnectScreen`'s own comment) — this is Find's own,
        // alongside the per-segment ones each child view already
        // carries (`Screen.Radar`, `Screen.Map`, `Screen.Map.GPS`,
        // `Screen.Map.Field`), all kept exactly as they were.
        //
        // `.accessibilityElement(children: .contain)` is load-bearing,
        // not decorative: without it, this VStack's OWN
        // `.accessibilityIdentifier` swallows every descendant button's
        // identifier that has no `.accessibilityLabel` of its own —
        // measured empirically (a UI-test diagnostic dump), not
        // assumed: EVERY segmented-control button reported
        // `identifier == "Screen.Find"` instead of its own
        // `"Find.Segment.<name>"` until this was added. `.contain`
        // tells SwiftUI this view is a container whose children keep
        // their own accessibility identity, rather than one opaque
        // element carrying this identifier for its whole subtree.
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("Screen.Find")
        .onAppear { FindLifecycle.apply(segment: segment, radar: radar, map: map) }
        .onDisappear { FindLifecycle.stopAll(radar: radar, map: map) }
        .onChange(of: segment) { _, newValue in
            FindLifecycle.apply(segment: newValue, radar: radar, map: map)
        }
    }

    @ViewBuilder
    private var content: some View {
        switch segment {
        case .radar:
            RadarView(model: radar, colorblind: colorblind)
        case .map, .field:
            // Same `MapTabView` instance/identity across a Map<->Field
            // switch (both cases render this ONE call) — no remount, no
            // stop/restart of `MapViewModel`'s pump between them, same
            // as `MapTabView`'s own internal toggle always did.
            MapTabView(model: map, segment: segment == .field ? .field : .gps, colorblind: colorblind,
                       onFind: onFind, onMessage: onMessage)
        }
    }

    // MARK: - Segmented control

    /// Same visual recipe as Lineup's Grid/My picks toggle
    /// (`LineupScreen.tabToggle`: an `ffSurface` pill, an `ffAmber`
    /// selected chip) rather than iOS's stock `.segmented` Picker style
    /// `MapTabView`'s old inner toggle used — not a shared component
    /// today (that toggle is `LineupScreen`'s own private computed
    /// view), so this matches its tokens by hand. Plain buttons, not a
    /// native segmented control, also means every segment is a full
    /// tap target — the owner's own reason for merging these two tabs
    /// was partly "bigger targets for gloved hands".
    private var segmentedControl: some View {
        HStack(spacing: 2) {
            ForEach(FindSegment.allCases) { candidate in
                let selected = segment == candidate
                Button {
                    segment = candidate
                } label: {
                    Text(candidate.rawValue.uppercased())
                        .font(.system(.subheadline, design: .rounded).weight(.semibold))
                        .padding(.vertical, 10)
                        .frame(maxWidth: .infinity)
                        .background(selected ? Color.ffAmber : Color.clear)
                        .foregroundStyle(selected ? Color.black : Color.ffMuted)
                        .clipShape(RoundedRectangle(cornerRadius: 11))
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("Find.Segment.\(candidate.rawValue)")
            }
        }
        .padding(3)
        .background(Color.ffSurface)
        .clipShape(RoundedRectangle(cornerRadius: 14))
        .padding(12)
        // `.contain` here too, for the SAME reason `body`'s own
        // `.accessibilityElement(children: .contain)` comment explains
        // — this HStack ALSO sets its own `.accessibilityIdentifier`
        // ("Find.Segment"), so it ALSO needs to opt back into exposing
        // each button's own identifier rather than becoming one opaque
        // element itself. Both containers needed the fix independently:
        // fixing only the outer `body` VStack was not enough (measured
        // — a UI-test rerun still failed the same way with just that
        // one in place).
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("Find.Segment")
    }
}
