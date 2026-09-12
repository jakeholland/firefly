//
//  LineupScreen.swift — the Lineup tab: day pills, the by-stage Grid,
//  and My picks (docs/specs/A01-companion-app.md, Lineup).
//
//  Honest states throughout: no pack shown until one has actually
//  loaded, the source footer always says exactly which of no pack /
//  bundled / cached (age) / fresh is on screen, and a night with no
//  published times says so instead of pretending an empty grid means
//  nothing is happening.
//
import FireflyModel
import SwiftUI

struct LineupScreen: View {
    @Bindable var model: LineupViewModel
    @State private var isShowingImportSheet = false
    @State private var importFeedback: String?

    var body: some View {
        VStack(spacing: 0) {
            header
            if model.festpack != nil {
                dayPillRow
                tabToggle
            }
            content
            if let nowNext = model.pickedNowNext {
                NowNextStrip(nowNext: nowNext)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .background(Color.ffBackground)
        .navigationTitle("Lineup")
        // The system nav bar is hidden on iOS (the approved mock has no
        // separate nav-bar chrome — "Lineup" is drawn by this screen's
        // OWN header, below) rather than merely set `.inline`: even
        // inline, the bar's own translucent material painted OVER this
        // screen's non-scrolling root content (`LineupGridView`'s
        // `ScrollView` is nested several levels down, not this view's
        // own root), which visually swallowed this header's "festival
        // name"/day-info text underneath the bar entirely — found
        // screenshotting demo mode for this PR (`.background(Color
        // .yellow)` on `header` during debugging showed the bar's
        // material painting right over it). `.navigationTitle` is left
        // in place for macOS's window/sidebar title, which does not
        // hit this bug.
        #if os(iOS)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar(.hidden, for: .navigationBar)
        #endif
        .task { model.observe() }
        .sheet(item: $model.selectedSet) { set in
            SetDetailSheet(model: model, set: set)
        }
        .sheet(isPresented: $isShowingImportSheet) {
            ImportPicksSheet(model: model, isPresented: $isShowingImportSheet, feedback: $importFeedback)
        }
        .alert("Picks imported", isPresented: Binding(get: { importFeedback != nil }, set: { if !$0 { importFeedback = nil } })) {
            Button("OK") { importFeedback = nil }
        } message: {
            Text(importFeedback ?? "")
        }
    }

    // MARK: - Header

    @ViewBuilder
    private var header: some View {
        if let festpack = model.festpack {
            VStack(alignment: .leading, spacing: 4) {
                Text("Lineup")
                    .font(.system(.title2, design: .rounded).weight(.bold))
                    .foregroundStyle(Color.ffInk)
                    .frame(maxWidth: .infinity, alignment: .leading)
                Text(headerSubtitle(festpack))
                    .font(.caption)
                    .foregroundStyle(Color.ffMuted)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .padding(.horizontal)
            .padding(.top, 12)
            .padding(.bottom, 8)
        } else {
            VStack(spacing: 8) {
                ProgressView()
                Text(model.isRefreshing ? "Loading festival data…" : "No festival pack loaded yet")
                    .font(.callout)
                    .foregroundStyle(Color.ffMuted)
            }
            .frame(maxWidth: .infinity)
            .padding(.top, 24)
        }
    }

    private func headerSubtitle(_ festpack: Festpack) -> String {
        var parts = ["\(festpack.name) \(festpack.year)"]
        if let dayLabel = model.dayLabel { parts.append(dayLabel) }
        if let tz = model.timeZoneLabel { parts.append(tz) }
        parts.append(sourceStateText)
        return parts.joined(separator: " · ")
    }

    private var sourceStateText: String {
        switch model.sourceState {
        case .none: return "no pack"
        case .bundled: return "bundled copy"
        case .fresh: return "fresh"
        case .cached(let age):
            let minutes = max(0, Int(age / 60))
            return minutes < 1 ? "cached (just now)" : "cached (\(minutes) min ago)"
        }
    }

    // MARK: - Day pills

    private var dayPillRow: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                ForEach(model.nights, id: \.self) { night in
                    let selected = model.selectedNightDayOfYear == night
                    Button {
                        model.selectedNightDayOfYear = night
                    } label: {
                        Text(model.dayPillLabel(for: night))
                            .font(.system(.subheadline, design: .rounded).weight(.semibold))
                            .frame(height: 40)
                            .padding(.horizontal, 16)
                            .background(selected ? Color.ffAmber : Color.clear)
                            .foregroundStyle(selected ? Color.black : Color.ffMuted)
                            .overlay(
                                Capsule().stroke(selected ? Color.clear : Color.ffDim, lineWidth: 1.5)
                            )
                            .clipShape(Capsule())
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.horizontal)
        }
        .padding(.bottom, 8)
    }

    // MARK: - Grid / My picks toggle

    private var tabToggle: some View {
        HStack(spacing: 2) {
            ForEach(LineupViewModel.Tab.allCases) { tab in
                let selected = model.selectedTab == tab
                Button {
                    model.selectedTab = tab
                } label: {
                    Text(tabLabel(tab))
                        .font(.system(.subheadline, design: .rounded).weight(.semibold))
                        .padding(.vertical, 8)
                        .frame(maxWidth: .infinity)
                        .background(selected ? Color.ffAmber : Color.clear)
                        .foregroundStyle(selected ? Color.black : Color.ffMuted)
                        .clipShape(RoundedRectangle(cornerRadius: 11))
                }
                .buttonStyle(.plain)
            }
        }
        .padding(3)
        .background(Color.ffSurface)
        .clipShape(RoundedRectangle(cornerRadius: 14))
        .padding(.horizontal)
        .padding(.bottom, 8)
    }

    private func tabLabel(_ tab: LineupViewModel.Tab) -> String {
        switch tab {
        case .grid: return "Grid"
        case .picks: return model.pickedCount > 0 ? "My picks · \(model.pickedCount)" : "My picks"
        }
    }

    // MARK: - Content

    @ViewBuilder
    private var content: some View {
        if model.festpack == nil {
            Spacer()
        } else if model.isSetTimesTBD {
            VStack(spacing: 8) {
                Text("SET TIMES TBD")
                    .font(.system(.callout, design: .monospaced).weight(.bold))
                    .foregroundStyle(Color.ffStaleAmber)
                Text("This night's lineup is published, but set times have not been announced yet.")
                    .font(.footnote)
                    .foregroundStyle(Color.ffMuted)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 32)
                untimedLineupList
            }
            .padding(.top, 16)
        } else {
            switch model.selectedTab {
            case .grid: LineupGridView(model: model)
            case .picks: picksTabContent
            }
        }
    }

    /// A night with no published times at all has nothing a time-axis
    /// grid can honestly place — the lineup itself still shows, as a
    /// plain per-stage list rather than a fabricated axis.
    private var untimedLineupList: some View {
        let sets = model.festpack.flatMap { festpack -> [FestpackScheduleSet]? in
            guard let night = model.selectedNightDayOfYear else { return nil }
            return festpack.sets.filter { $0.nightDayOfYear == night }
        } ?? []
        return List(sets, id: \.id) { set in
            HStack(spacing: 8) {
                StageSwatch(colorRGB: model.festpack?.stage(withID: set.stageID)?.colorRGB)
                Text(set.artist).font(.system(.body, design: .rounded))
                Spacer()
                Text("TBD").font(.caption2).foregroundStyle(Color.ffMuted)
            }
            .listRowBackground(Color.ffBackground)
        }
        .listStyle(.plain)
    }

    private var picksTabContent: some View {
        VStack(spacing: 0) {
            LineupPicksView(model: model)
            shareImportRow
        }
    }

    private var shareImportRow: some View {
        HStack(spacing: 12) {
            if let url = model.shareURL {
                ShareLink(item: url) {
                    Label("Share picks", systemImage: "square.and.arrow.up")
                        .font(.system(.footnote, design: .rounded).weight(.semibold))
                }
                .buttonStyle(.plain)
                .foregroundStyle(Color.ffAmber)
            }
            Spacer()
            Button {
                isShowingImportSheet = true
            } label: {
                Label("Import picks", systemImage: "square.and.arrow.down")
                    .font(.system(.footnote, design: .rounded).weight(.semibold))
            }
            .buttonStyle(.plain)
            .foregroundStyle(Color.ffMuted)
        }
        .padding(.horizontal)
        .padding(.vertical, 10)
    }
}

/// The "up next for me" footer strip — `LineupViewModel.pickedNowNext`'s
/// own doc comment for when this shows at all.
private struct NowNextStrip: View {
    let nowNext: LineupViewModel.NowNextPick

    var body: some View {
        HStack(alignment: .top) {
            if let now = nowNext.now {
                VStack(alignment: .leading, spacing: 2) {
                    Text("NOW").font(.system(.caption2, design: .monospaced).weight(.bold))
                    Text(now.artist.uppercased()).font(.system(.subheadline, design: .rounded).weight(.bold))
                    Text(nowNext.nowStage?.name ?? "Unknown stage").font(.caption)
                }
            }
            Spacer()
            if let next = nowNext.next {
                VStack(alignment: .trailing, spacing: 2) {
                    Text("UP NEXT").font(.system(.caption2, design: .monospaced).weight(.bold))
                    Text(next.artist.uppercased()).font(.system(.subheadline, design: .rounded).weight(.bold))
                    HStack(spacing: 4) {
                        if let time = LineupViewModel.timeText(next.startMinute) {
                            Text(time)
                        }
                        if let minutes = nowNext.nextStartsInMinutes {
                            Text("· in \(minutes) min")
                        }
                    }
                    .font(.caption)
                }
            }
        }
        .foregroundStyle(Color.black)
        .padding(12)
        .background(Color.ffLiveGreen)
        .clipShape(RoundedRectangle(cornerRadius: 14))
        .padding(.horizontal)
        .padding(.bottom, 10)
    }
}

/// A small filled circle in a stage's colour. `nil` (stage unknown)
/// renders as an honest hollow ring rather than a fabricated colour.
struct StageSwatch: View {
    let colorRGB: UInt32?

    var body: some View {
        Group {
            if let colorRGB {
                Circle().fill(Color(fireflyHex: colorRGB))
            } else {
                Circle().stroke(Color.ffMuted, lineWidth: 1.5)
            }
        }
        .frame(width: 10, height: 10)
    }
}
