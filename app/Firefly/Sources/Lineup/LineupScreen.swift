//
//  LineupScreen.swift — the Lineup tab (owner's canvas: chips
//  "Now & next" / "By stage" / "Starred"; "Day N of M").
//
//  Honest states throughout: no pack shown until one has actually
//  loaded, the source footer always says exactly which of no pack /
//  bundled / cached (age) / fresh is on screen, and a night with no
//  published times says so instead of pretending an empty list means
//  nothing is happening.
//
import FireflyModel
import SwiftUI

struct LineupScreen: View {
    @Bindable var model: LineupViewModel

    var body: some View {
        VStack(spacing: 0) {
            header
            if model.festpack != nil {
                tabChips
            }
            content
        }
        .background(Color.ffBackground)
        .navigationTitle("Lineup")
        .task { model.observe() }
        .refreshable { await model.refresh() }
    }

    // MARK: - Header

    @ViewBuilder
    private var header: some View {
        if let festpack = model.festpack {
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text(festpack.name)
                        .font(.system(.headline, design: .rounded).weight(.bold))
                        .foregroundStyle(Color.ffInk)
                    Spacer()
                    if let dayLabel = model.dayLabel {
                        Text(dayLabel.uppercased())
                            .font(.system(.caption, design: .monospaced))
                            .foregroundStyle(Color.ffAmber)
                    }
                }
                HStack(spacing: 6) {
                    if let tz = model.timeZoneLabel {
                        Text(tz)
                    }
                    Text("·")
                    Text(sourceStateText)
                }
                .font(.caption)
                .foregroundStyle(Color.ffMuted)
            }
            .padding(.horizontal)
            .padding(.top, 12)
            .padding(.bottom, 8)

            if !model.nights.isEmpty {
                nightPager
            }
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

    private var nightPager: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                ForEach(model.nights, id: \.self) { night in
                    let selected = model.selectedNightDayOfYear == night
                    Button {
                        model.selectedNightDayOfYear = night
                    } label: {
                        Text(nightLabel(night))
                            .font(.system(.caption, design: .rounded).weight(.semibold))
                            .padding(.horizontal, 12)
                            .padding(.vertical, 6)
                            .background(selected ? Color.ffAmber : Color.ffSurface)
                            .foregroundStyle(selected ? Color.black : Color.ffInk)
                            .clipShape(Capsule())
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.horizontal)
        }
        .padding(.bottom, 8)
    }

    private func nightLabel(_ dayOfYear: Int) -> String {
        guard let festpack = model.festpack, let date = festpack.date(forDayOfYear: dayOfYear) else {
            return "Night \(dayOfYear)"
        }
        let formatter = DateFormatter()
        formatter.dateFormat = "EEE M/d"
        formatter.timeZone = festpack.timeZone
        return formatter.string(from: date)
    }

    // MARK: - Tab chips

    private var tabChips: some View {
        HStack(spacing: 8) {
            ForEach(LineupViewModel.Tab.allCases) { tab in
                let selected = model.selectedTab == tab
                Button {
                    model.selectedTab = tab
                } label: {
                    Text(tab.rawValue)
                        .font(.system(.subheadline, design: .rounded).weight(.semibold))
                        .padding(.horizontal, 14)
                        .padding(.vertical, 8)
                        .frame(maxWidth: .infinity)
                        .background(selected ? Color.ffSurface : Color.clear)
                        .foregroundStyle(selected ? Color.ffAmber : Color.ffMuted)
                        .clipShape(RoundedRectangle(cornerRadius: 10))
                }
                .buttonStyle(.plain)
            }
        }
        .padding(.horizontal)
        .padding(.bottom, 4)
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
                byStageList // still show the lineup itself, just with no times/countdowns
            }
            .padding(.top, 16)
        } else {
            switch model.selectedTab {
            case .nowNext: nowNextList
            case .byStage: byStageList
            case .starred: starredList
            }
        }
    }

    private var nowNextList: some View {
        List(model.nowNextRows) { row in
            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: 8) {
                    StageSwatch(colorRGB: row.stage?.colorRGB)
                    Text(row.stage?.name ?? "Unknown stage")
                        .font(.system(.subheadline, design: .rounded).weight(.bold))
                        .foregroundStyle(Color.ffInk)
                    Spacer()
                }
                if let now = row.now {
                    setRow(now, label: "NOW", labelColor: .ffLiveGreen)
                } else {
                    Text("nothing playing")
                        .font(.footnote)
                        .foregroundStyle(Color.ffMuted)
                }
                if let next = row.next {
                    setRow(next, label: "NEXT", labelColor: .ffMuted, startsInMinutes: row.startsInMinutes)
                }
            }
            .padding(.vertical, 4)
            .listRowBackground(Color.ffBackground)
        }
        .listStyle(.plain)
    }

    private var byStageList: some View {
        List {
            ForEach(model.byStageGroups, id: \.stage?.id) { group in
                Section {
                    ForEach(group.sets) { set in
                        setRow(set, label: nil)
                    }
                } header: {
                    HStack(spacing: 6) {
                        StageSwatch(colorRGB: group.stage?.colorRGB)
                        Text(group.stage?.name ?? "Unknown stage")
                    }
                }
            }
        }
        .listStyle(.plain)
    }

    private var starredList: some View {
        Group {
            if model.starredRows.isEmpty {
                VStack(spacing: 8) {
                    Image(systemName: "star")
                        .font(.largeTitle)
                        .foregroundStyle(Color.ffMuted)
                    Text("Star an artist from any list to track it here.")
                        .font(.footnote)
                        .foregroundStyle(Color.ffMuted)
                }
                .frame(maxWidth: .infinity)
                .padding(.top, 32)
            } else {
                List(model.starredRows) { row in
                    HStack(spacing: 8) {
                        StageSwatch(colorRGB: row.stage?.colorRGB)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(row.set.artist).font(.system(.body, design: .rounded).weight(.semibold))
                            Text(row.stage?.name ?? "Unknown stage")
                                .font(.caption)
                                .foregroundStyle(Color.ffMuted)
                        }
                        Spacer()
                        countdownText(row.startsInMinutes)
                    }
                    .listRowBackground(Color.ffBackground)
                }
                .listStyle(.plain)
            }
        }
    }

    @ViewBuilder
    private func setRow(_ set: FestpackScheduleSet, label: String?, labelColor: Color = .ffMuted, startsInMinutes: Int? = nil) -> some View {
        HStack(spacing: 8) {
            if let label {
                Text(label)
                    .font(.system(.caption2, design: .monospaced).weight(.bold))
                    .foregroundStyle(labelColor)
                    .frame(width: 40, alignment: .leading)
            }
            Button {
                model.toggleStar(set.artist)
            } label: {
                Image(systemName: model.isStarred(set.artist) ? "star.fill" : "star")
                    .foregroundStyle(model.isStarred(set.artist) ? Color.ffAmber : Color.ffMuted)
            }
            .buttonStyle(.plain)
            VStack(alignment: .leading, spacing: 2) {
                Text(set.artist).font(.system(.body, design: .rounded))
                if !set.note.isEmpty {
                    Text(set.note).font(.caption2).foregroundStyle(Color.ffMuted)
                }
            }
            Spacer()
            if let startsInMinutes, startsInMinutes > 0 {
                countdownText(startsInMinutes)
            } else if let time = timeText(set.startMinute) {
                Text(time).font(.system(.caption, design: .monospaced)).foregroundStyle(Color.ffMuted)
            } else {
                Text("set time TBD").font(.caption2).foregroundStyle(Color.ffMuted)
            }
        }
    }

    @ViewBuilder
    private func countdownText(_ minutes: Int?) -> some View {
        if let minutes {
            Text("starts in \(minutes) min")
                .font(.system(.caption, design: .monospaced).weight(.semibold))
                .foregroundStyle(Color.ffAmber)
        } else {
            EmptyView()
        }
    }

    /// Minutes-from-night-midnight -> "HH:MM", wrapping the >= 1440
    /// after-midnight space back to an ordinary clock reading. Purely a
    /// display formatter — the schedule math itself never uses this.
    private func timeText(_ minute: Int?) -> String? {
        guard let minute else { return nil }
        let wrapped = minute % 1440
        return String(format: "%02d:%02d", wrapped / 60, wrapped % 60)
    }
}

/// A small filled circle in a stage's colour — the "stage colour swatch
/// per row" the owner's canvas calls for. `nil` (stage unknown) renders
/// as an honest hollow ring rather than a fabricated colour.
private struct StageSwatch: View {
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
