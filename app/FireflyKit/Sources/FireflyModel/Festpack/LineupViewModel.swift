//
//  LineupViewModel.swift — the Lineup screen's state: day pills, the
//  by-stage Grid, and My picks (docs/specs/A01-companion-app.md,
//  Lineup). Replaces the earlier "Now & next / By stage / Starred"
//  list experience — see git history for that version.
//
//  Every schedule-schema question (what's on a night, whether a
//  night's times are all TBD) goes through `FestpackSchedule`'s
//  `ff_sched` bridge; this view model shapes those answers into rows a
//  SwiftUI screen can render, plus three things that are honestly this
//  app's OWN presentation logic, not core-owned schedule facts (same
//  call `timeText`'s doc comment already made for the old version):
//  the by-stage grid's geometry (`LineupGridLayout`), pick-vs-pick
//  conflict detection, and the wall-clock question (`FestpackWallClock
//  .split`) of which night and minute "now" actually is.
//
import FireflyMesh
import Foundation
import Observation

@MainActor
@Observable
public final class LineupViewModel {
    public enum Tab: String, CaseIterable, Sendable, Identifiable {
        case grid = "Grid"
        case picks = "My picks"
        public var id: String { rawValue }
    }

    public struct PickedRow: Sendable, Identifiable {
        public var id: Int { self.set.id }
        public var set: FestpackScheduleSet
        public var stage: FestpackStage?
        /// The settimes-compatible id this pick is stored/shared under
        /// (`PicksCodec.setID(for:in:)`).
        public var pickID: String
        /// Artist names of other CURRENTLY PICKED sets whose time
        /// overlaps this one (same night; both this set and the other
        /// need a known start AND effective end — `LineupTimeInference
        /// .effectiveEnds`'s own doc comment) — empty when this pick
        /// conflicts with nothing.
        public var conflictsWithArtists: [String]
        /// True when at least one overlap above was decided using an
        /// INFERRED end time (this set's or the other's). The clash is
        /// then this app's inference, not something the festpack
        /// states, and the screen says so rather than asserting a
        /// collision the pack never published.
        public var conflictsAreInferred: Bool
    }

    /// The "up next for me" footer strip both the Grid and Picks
    /// screens share (the mocks' green NOW/UP NEXT card) — built ONLY
    /// from picked sets on the night the wall clock actually resolves
    /// to right now, never against a night the user merely has
    /// selected/browsing (same "no countdown against the wrong day's
    /// clock" rule the old `starredRows` doc comment already stated).
    /// `nil` when there is nothing honestly live or upcoming to show —
    /// no picks, no picks tonight, or "tonight" is not actually now.
    public struct NowNextPick: Sendable, Equatable {
        public var now: FestpackScheduleSet?
        public var nowStage: FestpackStage?
        public var next: FestpackScheduleSet?
        public var nextStage: FestpackStage?
        public var nextStartsInMinutes: Int?
    }

    public enum ImportResult: Sendable, Equatable {
        case imported(count: Int, dropped: Int)
        /// The pasted text carried no recognisable picks link/code at
        /// all, or the pack has not loaded yet.
        case nothingFound
    }

    private let festpackProvider: any FestpackProviding
    private let picksStore: any PicksStoring
    private let now: @Sendable () -> Date
    private var observationTask: Task<Void, Never>?

    public private(set) var festpack: Festpack?
    public private(set) var sourceState: FestpackSourceState = .none
    public private(set) var pickedSetIDs: Set<String>
    public private(set) var isRefreshing = false
    public var selectedTab: Tab = .grid
    /// The day pill selected. Defaults to whatever the phone's own
    /// clock resolves to once a pack loads (`resolvedNight`); the pill
    /// row can page it to any other day the pack has programming for.
    public var selectedNightDayOfYear: Int?
    /// The set whose detail sheet is open (tapped from a grid block or
    /// a picks row) — `nil` when no sheet should show.
    public var selectedSet: FestpackScheduleSet?

    public init(festpackProvider: any FestpackProviding, picksStore: any PicksStoring,
                now: @escaping @Sendable () -> Date = { Date() }) {
        self.festpackProvider = festpackProvider
        self.picksStore = picksStore
        self.now = now
        self.pickedSetIDs = picksStore.pickedSetIDs()
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
        // `dayIndex` to an edge while highlighting nothing in the day
        // pill row (a real bug this exact case caught: Firefly Fields
        // runs Sept 4-6, "today" resolved to Sept 11, and every list
        // rendered honestly empty with no day pill selected at all —
        // confusing, not honest). Falling back to the pack's first
        // night is the honest answer to "which night has real content
        // to show."
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

    // MARK: - Picks

    /// `nil` only if no pack has loaded yet — a set can always be
    /// identified once its owning pack is known.
    public func pickID(for set: FestpackScheduleSet) -> String? {
        festpack.map { PicksCodec.setID(for: set, in: $0) }
    }

    public func isPicked(_ set: FestpackScheduleSet) -> Bool {
        guard let id = pickID(for: set) else { return false }
        return pickedSetIDs.contains(id)
    }

    public func togglePick(_ set: FestpackScheduleSet) {
        guard let id = pickID(for: set) else { return }
        picksStore.toggle(id)
        pickedSetIDs = picksStore.pickedSetIDs()
    }

    public func selectSet(_ set: FestpackScheduleSet) { selectedSet = set }
    public func dismissSetDetail() { selectedSet = nil }

    /// `set`'s effective end for the detail sheet's time range, WITH
    /// its provenance (`LineupTimeInference.EndSource`) — a screen
    /// showing this must say whether the end was published or inferred,
    /// which is why this returns the source rather than a bare minute.
    /// `nil` for an unknown-start set (nothing to infer an end FROM),
    /// never a guess.
    public func effectiveEnd(for set: FestpackScheduleSet) -> LineupTimeInference.EffectiveEnd? {
        guard let festpack else {
            return set.endMinute.map { .init(minute: $0, source: .published) }
        }
        let daySets = FestpackSchedule.daySets(in: festpack, night: set.nightDayOfYear)
        return LineupTimeInference.effectiveEnds(for: daySets)[set.id]
    }

    /// The "Share picks" URL for the currently selected day — `nil`
    /// before a pack/night is known. Always a real link even with zero
    /// picks (a clean night URL, no `?picks=`) — see `PicksCodec
    /// .shareURL`'s own doc comment.
    public var shareURL: URL? {
        guard let festpack, let night = selectedNightDayOfYear else { return nil }
        return PicksCodec.shareURL(for: night, pickIDs: pickedSetIDs, in: festpack)
    }

    /// Parses `raw` (a pasted settimes share URL, bare query, or bare
    /// code list — `PicksCodec.parsePicksInput`) and adds every code
    /// that resolves against the CURRENT pack to the persisted pick
    /// list. Existing picks not mentioned in `raw` are left untouched
    /// — import only ever ADDS, it never replaces the whole list.
    @discardableResult
    public func importPicks(from raw: String) -> ImportResult {
        guard let festpack, let code = PicksCodec.parsePicksInput(raw) else { return .nothingFound }
        let decoded = PicksCodec.decodePicks(code, in: festpack)
        for id in decoded.ids { picksStore.setPicked(true, setID: id) }
        pickedSetIDs = picksStore.pickedSetIDs()
        return .imported(count: decoded.ids.count, dropped: decoded.dropped)
    }

    /// Every night with at least one pick, ascending, each with its
    /// picked sets sorted by start (unknown-time picks last) and
    /// conflict-marked against every OTHER pick on the SAME night —
    /// same half-open interval-overlap rule as settimes' `overlapsOf`.
    public var pickedGroups: [(night: Int, rows: [PickedRow])] {
        guard let festpack else { return [] }
        let pickedNights = Set(festpack.sets.compactMap { set -> Int? in
            pickedSetIDs.contains(PicksCodec.setID(for: set, in: festpack)) ? set.nightDayOfYear : nil
        }).sorted()

        return pickedNights.map { night in
            let daySets = FestpackSchedule.daySets(in: festpack, night: night)
            let effectiveEnds = LineupTimeInference.effectiveEnds(for: daySets)
            let picked = daySets.filter { pickedSetIDs.contains(PicksCodec.setID(for: $0, in: festpack)) }
            let rows = picked
                .map { set -> PickedRow in
                    // Half-open `[start, end)` on both sides, so two
                    // ABUTTING sets (one ends exactly when the next
                    // begins) do NOT count as a clash — same rule as
                    // settimes' `overlapsOf`.
                    let clashes = picked.filter { other in
                        guard other.id != set.id,
                              let otherStart = other.startMinute, let otherEnd = effectiveEnds[other.id],
                              let setStart = set.startMinute, let setEnd = effectiveEnds[set.id] else { return false }
                        return otherStart < setEnd.minute && setStart < otherEnd.minute
                    }
                    let inferred = clashes.contains { other in
                        effectiveEnds[other.id]?.isPublished == false
                    } || effectiveEnds[set.id]?.isPublished == false
                    return PickedRow(set: set, stage: festpack.stage(withID: set.stageID),
                                      pickID: PicksCodec.setID(for: set, in: festpack),
                                      conflictsWithArtists: clashes.map(\.artist),
                                      conflictsAreInferred: !clashes.isEmpty && inferred)
                }
                .sorted { lhs, rhs in
                    if lhs.set.startMinute == nil { return false }
                    if rhs.set.startMinute == nil { return true }
                    return lhs.set.startMinute! < rhs.set.startMinute!
                }
            return (night: night, rows: rows)
        }
    }

    public var pickedCount: Int { pickedSetIDs.count }

    /// See `NowNextPick`'s own doc comment for why this is `nil`
    /// whenever the selected night is not the one "now" actually is.
    public var pickedNowNext: NowNextPick? {
        guard let festpack, let night = selectedNightDayOfYear, !pickedSetIDs.isEmpty,
              resolvedNight(for: festpack) == night, let nowMinute = currentNowMinute(for: festpack) else { return nil }
        let daySets = FestpackSchedule.daySets(in: festpack, night: night)
        let effectiveEnds = LineupTimeInference.effectiveEndMinutes(for: daySets)
        let picked = daySets.filter { pickedSetIDs.contains(PicksCodec.setID(for: $0, in: festpack)) }

        let live = picked.first { set in
            guard let start = set.startMinute, let end = effectiveEnds[set.id] else { return false }
            return nowMinute >= start && nowMinute < end
        }
        let next = picked
            .filter { ($0.startMinute ?? Int.max) > nowMinute }
            .min { ($0.startMinute ?? Int.max) < ($1.startMinute ?? Int.max) }
        guard live != nil || next != nil else { return nil }
        return NowNextPick(now: live, nowStage: live.flatMap { festpack.stage(withID: $0.stageID) },
                            next: next, nextStage: next.flatMap { festpack.stage(withID: $0.stageID) },
                            nextStartsInMinutes: next?.startMinute.map { max(0, $0 - nowMinute) })
    }

    // MARK: - Grid

    /// The by-stage grid for the selected night — `LineupGridLayout
    /// .build`'s own doc comment for the geometry rules. Empty
    /// (`columns`/`hourLines: []`) before a pack/night is known or a
    /// night has no known-start set.
    public var gridLayout: LineupGridLayout {
        guard let festpack, let night = selectedNightDayOfYear else {
            return LineupGridLayout(columns: [], hourLines: [], axisStartMinute: 0, axisEndMinute: 0)
        }
        return LineupGridLayout.build(daySets: FestpackSchedule.daySets(in: festpack, night: night), stages: festpack.stages)
    }

    /// The "now" line's offset into `gridLayout`, in minutes from
    /// `gridLayout.axisStartMinute` — `nil` whenever the selected night
    /// is not the one "now" actually is, or the axis has nothing on it.
    public var gridNowOffsetMinutes: Int? {
        guard let festpack, let night = selectedNightDayOfYear, resolvedNight(for: festpack) == night,
              let nowMinute = currentNowMinute(for: festpack) else { return nil }
        return gridLayout.nowOffsetMinutes(nowMinute: nowMinute)
    }

    // MARK: - Day pills / night metadata

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

    /// Every distinct festival night the pack has sets on, ascending —
    /// one day pill per entry.
    public var nights: [Int] {
        guard let festpack else { return [] }
        return Array(Set(festpack.sets.map(\.nightDayOfYear))).sorted()
    }

    /// A day pill's label — "Thu", "Fri" — the pack's own weekday for
    /// that night's date, always in a fixed (`en_US_POSIX`) locale so
    /// this never varies with the phone's own region setting the way
    /// festival programming itself does not.
    public func dayPillLabel(for dayOfYear: Int) -> String {
        guard let festpack, let date = festpack.date(forDayOfYear: dayOfYear) else { return "Day \(dayOfYear)" }
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = festpack.timeZone
        formatter.dateFormat = "EEE"
        return formatter.string(from: date)
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

    /// Minutes-from-night-midnight -> "HH:MM", wrapping the >= 1440
    /// after-midnight space back to an ordinary clock reading. Purely a
    /// display formatter — the schedule math itself never uses this.
    public static func timeText(_ minute: Int?) -> String? {
        guard let minute else { return nil }
        let wrapped = minute % 1440
        return String(format: "%02d:%02d", wrapped / 60, wrapped % 60)
    }
}
