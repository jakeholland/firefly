//
//  LineupViewModel.swift — the Lineup screen's state (owner's canvas:
//  chips "Now & next" / "By stage" / "Starred"; "Day N of M").
//
//  Every schedule-schema question (what's live, what's next, whether a
//  night's times are all TBD) goes through `FestpackSchedule`'s
//  `ff_sched` bridge — this view model only shapes those answers into
//  rows a SwiftUI screen can render, plus the wall-clock question
//  (`FestpackWallClock.split`, `ff_wall_split_local`) of which night
//  and minute "now" actually is.
//
import FireflyMesh
import Foundation
import Observation

@MainActor
@Observable
public final class LineupViewModel {
    public enum Tab: String, CaseIterable, Sendable, Identifiable {
        case nowNext = "Now & next"
        case byStage = "By stage"
        case starred = "Starred"
        public var id: String { rawValue }
    }

    public struct NowNextRow: Sendable, Identifiable {
        public var id: String
        public var stage: FestpackStage?
        public var now: FestpackScheduleSet?
        public var next: FestpackScheduleSet?
        /// Minutes until `next` starts — `nil` if there is no known-time
        /// next set, never a fabricated countdown.
        public var startsInMinutes: Int?
    }

    public struct StarredRow: Sendable, Identifiable {
        public var id: Int { self.set.id }
        public var set: FestpackScheduleSet
        public var stage: FestpackStage?
        public var startsInMinutes: Int?
    }

    private let festpackProvider: any FestpackProviding
    private let starredStore: any StarredArtistsStoring
    private let now: @Sendable () -> Date
    private var observationTask: Task<Void, Never>?

    public private(set) var festpack: Festpack?
    public private(set) var sourceState: FestpackSourceState = .none
    public private(set) var starred: Set<String>
    public private(set) var isRefreshing = false
    public var selectedTab: Tab = .nowNext
    /// The night being shown. Defaults to whatever the phone's own
    /// clock resolves to once a pack loads (`resolvedNight`); the chip
    /// row can page it to any other night the pack has programming for.
    public var selectedNightDayOfYear: Int?

    public init(festpackProvider: any FestpackProviding, starredStore: any StarredArtistsStoring,
                now: @escaping @Sendable () -> Date = { Date() }) {
        self.festpackProvider = festpackProvider
        self.starredStore = starredStore
        self.now = now
        self.starred = starredStore.starredArtists()
    }

    /// Idempotent, same convention as every other screen's `observe()`
    /// (MVVM conventions #4) — subscribes to `festpackUpdates()`
    /// SYNCHRONOUSLY before this method returns (the stream itself is
    /// created here, not inside the `Task`), so a pack the provider
    /// yields between this call and the `Task` actually starting is
    /// never missed (`EventHub`'s own "arrives after this call does not
    /// see it" rule).
    public func observe() {
        guard observationTask == nil else { return }
        let stream = festpackProvider.festpackUpdates()
        observationTask = Task { [weak self] in
            for await pack in stream {
                guard let self else { return }
                await self.apply(pack)
            }
        }
        Task { [weak self] in await self?.refresh() }
    }

    private func apply(_ pack: Festpack) async {
        festpack = pack
        sourceState = await festpackProvider.sourceState()
        let packNights = nights
        guard selectedNightDayOfYear == nil || !packNights.contains(selectedNightDayOfYear!) else { return }
        // The phone's wall clock only picks the selected night when it
        // actually lands on one the PACK has programming for — a demo
        // pack (or a real one browsed before/after the festival dates)
        // can easily have "today" fall outside every night it knows
        // about, and defaulting to that non-existent night would clamp
        // `dayIndex` to an edge while highlighting nothing in the night
        // pager (a real bug this exact case caught: Firefly Fields runs
        // Sept 4-6, "today" resolved to Sept 11, and every list rendered
        // honestly empty with no night chip selected at all — confusing,
        // not honest). Falling back to the pack's first night is the
        // honest answer to "which night has real content to show."
        if let resolved = resolvedNight(for: pack), packNights.contains(resolved) {
            selectedNightDayOfYear = resolved
        } else {
            selectedNightDayOfYear = packNights.first
        }
    }

    /// Settings' "refresh" action, and the one call `observe()` makes at
    /// screen-open. A failed fetch/parse leaves `festpack`/`sourceState`
    /// exactly as they were — `AlmanacFestpackProvider`'s own contract.
    public func refresh() async {
        isRefreshing = true
        await festpackProvider.refresh()
        if let pack = await festpackProvider.current() {
            await apply(pack)
        } else {
            sourceState = await festpackProvider.sourceState()
        }
        isRefreshing = false
    }

    public func isStarred(_ artist: String) -> Bool { starred.contains(artist) }

    public func toggleStar(_ artist: String) {
        starredStore.toggle(artist)
        starred = starredStore.starredArtists()
    }

    /// The phone's own wall clock (always trusted — unlike the puck,
    /// which has to earn that over the mesh) folded through
    /// `ff_wall_split_local` against the pack's own UTC offset. `nil`
    /// only when the reading falls outside that function's fixed
    /// plausibility window — see `FestpackWallClock.split`'s own doc
    /// comment.
    private func resolvedNight(for pack: Festpack) -> Int? {
        FestpackWallClock.split(now: now(), utcOffsetMinutes: pack.utcOffsetMinutes)?.dayOfYear
    }

    private func currentNowMinute(for pack: Festpack) -> Int? {
        FestpackWallClock.split(now: now(), utcOffsetMinutes: pack.utcOffsetMinutes)?.nowMinute
    }

    /// Every distinct festival night the pack has sets on, ascending.
    public var nights: [Int] {
        guard let festpack else { return [] }
        return Array(Set(festpack.sets.map(\.nightDayOfYear))).sorted()
    }

    /// "Day N of M" — `nil` before a pack/night is known.
    public var dayLabel: String? {
        guard let festpack, let night = selectedNightDayOfYear else { return nil }
        return "Day \(festpack.dayIndex(forDayOfYear: night)) of \(festpack.dayCount)"
    }

    /// The festival's own timezone abbreviation-free label — this
    /// schema carries only a UTC offset (`Festpack.timeZone`'s own doc
    /// comment), so this is exactly that, never an invented zone name.
    public var timeZoneLabel: String? {
        guard let festpack else { return nil }
        let totalMinutes = festpack.utcOffsetMinutes
        let sign = totalMinutes < 0 ? "-" : "+"
        let hours = abs(totalMinutes) / 60
        let minutes = abs(totalMinutes) % 60
        return minutes == 0 ? "UTC\(sign)\(hours)" : String(format: "UTC%@%d:%02d", sign, hours, minutes)
    }

    /// True iff the selected night has sets but NONE with a known start
    /// time — `ff_sched_day_tbd`, bound straight through.
    public var isSetTimesTBD: Bool {
        guard let festpack, let night = selectedNightDayOfYear else { return false }
        return FestpackSchedule.dayIsAllTBD(in: festpack, night: night)
    }

    /// "Now & next" per stage — every stage with any programming on the
    /// selected night, `now` from `ff_sched_now_playing`'s half-open
    /// live window, `next` the earliest known-start set after it (or
    /// after "right now" if nothing is currently live on that stage).
    public var nowNextRows: [NowNextRow] {
        guard let festpack, let night = selectedNightDayOfYear else { return [] }
        let nowMinute = currentNowMinute(for: festpack) ?? 0
        let live = FestpackSchedule.nowPlaying(in: festpack, night: night, nowMinute: nowMinute, starred: starred)
        let lineup = FestpackSchedule.daySets(in: festpack, night: night)

        return festpack.stages.compactMap { stage -> NowNextRow? in
            let stageLineup = lineup.filter { $0.stageID == stage.id }
            guard !stageLineup.isEmpty else { return nil }
            let liveSet = live.first { $0.set.stageID == stage.id }?.set
            let boundary = liveSet?.startMinute ?? nowMinute
            let next = stageLineup.first { ($0.startMinute ?? -1) > boundary }
            let startsIn = next?.startMinute.map { max(0, $0 - nowMinute) }
            return NowNextRow(id: stage.id, stage: stage, now: liveSet, next: next, startsInMinutes: startsIn)
        }
    }

    /// The By-stage view: every stage's full night lineup, in start
    /// order, unknown-time sets last (`FestpackSchedule.daySets`'s own
    /// sort).
    public var byStageGroups: [(stage: FestpackStage?, sets: [FestpackScheduleSet])] {
        guard let festpack, let night = selectedNightDayOfYear else { return [] }
        let lineup = FestpackSchedule.daySets(in: festpack, night: night)
        var order: [String] = []
        var buckets: [String: [FestpackScheduleSet]] = [:]
        for set in lineup {
            let key = set.stageID ?? ""
            if buckets[key] == nil { order.append(key) }
            buckets[key, default: []].append(set)
        }
        return order.map { key in (festpack.stage(withID: key.isEmpty ? nil : key), buckets[key] ?? []) }
    }

    /// The Starred list — every starred artist's sets across the whole
    /// pack (not just the selected night), earliest first.
    /// "starts in N min" is only shown for a set on the CURRENTLY
    /// selected/resolved night; a starred set on a different night gets
    /// no countdown rather than a countdown measured against the wrong
    /// day's clock.
    public var starredRows: [StarredRow] {
        guard let festpack else { return [] }
        let nowMinute = currentNowMinute(for: festpack)
        return festpack.sets
            .filter { starred.contains($0.artist) }
            .sorted { lhs, rhs in
                if lhs.nightDayOfYear != rhs.nightDayOfYear { return lhs.nightDayOfYear < rhs.nightDayOfYear }
                return (lhs.startMinute ?? .max) < (rhs.startMinute ?? .max)
            }
            .map { set in
                let startsIn: Int? = {
                    guard let start = set.startMinute, let nowMinute, set.nightDayOfYear == selectedNightDayOfYear else { return nil }
                    let delta = start - nowMinute
                    return delta > 0 ? delta : nil
                }()
                return StarredRow(set: set, stage: festpack.stage(withID: set.stageID), startsInMinutes: startsIn)
            }
    }
}
