//
//  LineupGridLayout.swift — the by-stage grid's layout model
//  (docs/specs/A01-companion-app.md, Lineup: "By-stage grid view:
//  columns per stage, rows by time... set blocks sized by duration").
//
//  Pure geometry over `FestpackScheduleSet`s already resolved for one
//  night by `FestpackSchedule.daySets(in:night:)` — this file adds
//  nothing to what `ff_sched` already knows about a night's schedule,
//  it only lays that schedule out on a shared time axis, which is a
//  SwiftUI-facing presentation concern, not a core-owned schedule fact
//  (`LineupViewModel.timeText`'s doc comment makes the identical call
//  for its own display formatting). Every offset/duration below is in
//  MINUTES, in the same folded "minutes from the night's local
//  midnight, >= 1440 past midnight" space `FestpackScheduleSet
//  .startMinute` already uses — converting that to points is the
//  view's job (a fixed points-per-minute scale), not this model's.
//
//  End-time inference (published end, else the next known-start set on
//  the SAME stage, else a default length) mirrors settimes'
//  `buildFestival` loop (settimes/src/lib/festival.ts) byte-for-byte in
//  BEHAVIOR — not by calling into it, there is no such thing to call
//  from Swift — because no `ff_sched` API answers "every set's
//  effective end"; `ff_sched_now_playing`'s `pct_valid`/`mins_left`
//  only ever computes that for whichever set is live RIGHT NOW.
//
import Foundation

public enum LineupTimeInference {
    /// settimes' `DEFAULT_SET_MIN` — the fallback length assumed for the
    /// LAST known-start set on a stage when the pack gives it no
    /// explicit end and there is no following set to infer one from.
    public static let defaultSetMinutes = 60

    /// Every KNOWN-start set in `daySets`, mapped to its effective end
    /// minute — published `endMinute` when present, else the next
    /// known-start set's `startMinute` on the SAME stage, else
    /// `startMinute + defaultSetMinutes` for the last set on a stage.
    /// Keyed by `FestpackScheduleSet.id`, which that struct's own doc
    /// comment already limits to "stable only within one loaded
    /// Festpack" — exactly the lifetime this map is ever used over (one
    /// `daySets(...)` call's worth of sets). A set with no known start
    /// has no entry at all: there is no time axis position to infer an
    /// end FOR.
    public static func effectiveEndMinutes(for daySets: [FestpackScheduleSet]) -> [Int: Int] {
        var byStage: [String: [FestpackScheduleSet]] = [:]
        for set in daySets where set.startMinute != nil {
            byStage[set.stageID ?? "", default: []].append(set)
        }
        var result: [Int: Int] = [:]
        for (_, sets) in byStage {
            let sorted = sets.sorted { $0.startMinute! < $1.startMinute! }
            for (index, set) in sorted.enumerated() {
                let start = set.startMinute!
                if let end = set.endMinute {
                    result[set.id] = end
                } else if let next = sorted[(index + 1)...].first(where: { $0.startMinute! > start }) {
                    result[set.id] = next.startMinute!
                } else {
                    result[set.id] = start + defaultSetMinutes
                }
            }
        }
        return result
    }
}

/// The by-stage grid for one night: a column per stage (pack order,
/// stages with no known-start set on this night omitted — there is
/// nothing to place on the axis for them), a shared 15-minute time
/// axis spanning every known-start set's start-to-effective-end range,
/// and one `Block` per known-start set giving its offset/duration on
/// that axis.
public struct LineupGridLayout: Sendable, Equatable {
    public struct Block: Sendable, Equatable, Identifiable {
        public var id: Int { self.set.id }
        public var set: FestpackScheduleSet
        /// Minutes from `axisStartMinute` to this block's start.
        public var offsetMinutes: Int
        /// This block's effective duration — always >= 1, never zero
        /// (`LineupTimeInference.effectiveEndMinutes` guarantees
        /// `end > start` for every set it covers, since the shortest
        /// inferred/derived end is still one minute past the start).
        public var durationMinutes: Int
    }

    public struct Column: Sendable, Equatable, Identifiable {
        public var id: String { stage?.id ?? "" }
        public var stage: FestpackStage?
        public var blocks: [Block]
    }

    /// One whole-hour gridline: `offsetMinutes` from `axisStartMinute`,
    /// and its clock label ("7 PM").
    public struct HourLine: Sendable, Equatable, Identifiable {
        public var id: Int { offsetMinutes }
        public var offsetMinutes: Int
        public var label: String
    }

    public var columns: [Column]
    public var hourLines: [HourLine]
    /// Minutes-from-night-midnight of the axis's first row — floored to
    /// the nearest 15 minutes at/below the earliest known set start.
    /// `0` (and `axisEndMinute == 0`, `columns`/`hourLines` empty) when
    /// the night has no known-start set at all.
    public var axisStartMinute: Int
    /// Minutes-from-night-midnight of the axis's last row — ceiled to
    /// the nearest 15 minutes at/above the latest known set's effective
    /// end.
    public var axisEndMinute: Int

    public var axisLengthMinutes: Int { max(0, axisEndMinute - axisStartMinute) }

    /// `nowMinute`'s offset from `axisStartMinute`, for the "now" line —
    /// `nil` when `nowMinute` falls outside `[axisStartMinute,
    /// axisEndMinute)`, an honest "not on this axis right now" rather
    /// than clamping the line to an edge it is not actually at.
    public func nowOffsetMinutes(nowMinute: Int) -> Int? {
        guard axisLengthMinutes > 0, nowMinute >= axisStartMinute, nowMinute < axisEndMinute else { return nil }
        return nowMinute - axisStartMinute
    }

    public static func build(daySets: [FestpackScheduleSet], stages: [FestpackStage]) -> LineupGridLayout {
        let effectiveEnds = LineupTimeInference.effectiveEndMinutes(for: daySets)
        let timedSets = daySets.filter { $0.startMinute != nil && effectiveEnds[$0.id] != nil }
        guard !timedSets.isEmpty else {
            return LineupGridLayout(columns: [], hourLines: [], axisStartMinute: 0, axisEndMinute: 0)
        }

        let earliestStart = timedSets.map { $0.startMinute! }.min()!
        let latestEnd = timedSets.map { effectiveEnds[$0.id]! }.max()!
        let axisStart = floorToQuarterHour(earliestStart)
        let axisEnd = ceilToQuarterHour(latestEnd)

        // Pack order for stages, restricted to ones with a placeable
        // set tonight — an empty stage column is nothing to scroll to.
        let stagesWithSets = Set(timedSets.map { $0.stageID ?? "" })
        let orderedStages: [FestpackStage?] = stages.filter { stagesWithSets.contains($0.id) }
        // A set whose `stageID` matches no known stage still gets its
        // own (unknown-stage) column, at the end — the same "honest
        // hollow ring rather than fabricated colour" call
        // `LineupScreen`'s `StageSwatch` already makes for this case,
        // extended to "still gets a column" here.
        let hasUnknownStageSets = timedSets.contains { set in
            !stages.contains { $0.id == set.stageID }
        }
        let stageColumnsKeys: [FestpackStage?] = hasUnknownStageSets ? orderedStages + [nil] : orderedStages

        let columns: [Column] = stageColumnsKeys.map { stage in
            let stageSets = timedSets
                .filter { set in
                    if let stage { return set.stageID == stage.id }
                    // The unknown-stage column: everything whose
                    // `stageID` matches none of `stages` — NOT merely
                    // `stageID == nil` (a set can carry a non-nil id
                    // that simply is not in this pack's stage list).
                    return !stages.contains { $0.id == set.stageID }
                }
                .sorted { $0.startMinute! < $1.startMinute! }
            let blocks = stageSets.map { set in
                Block(set: set, offsetMinutes: set.startMinute! - axisStart,
                      durationMinutes: max(1, effectiveEnds[set.id]! - set.startMinute!))
            }
            return Column(stage: stage, blocks: blocks)
        }

        var hourLines: [HourLine] = []
        var hour = ((axisStart + 59) / 60) * 60 // ceil axisStart to the next whole hour
        while hour <= axisEnd {
            hourLines.append(HourLine(offsetMinutes: hour - axisStart, label: hourLabel(forFoldedMinute: hour)))
            hour += 60
        }

        return LineupGridLayout(columns: columns, hourLines: hourLines, axisStartMinute: axisStart, axisEndMinute: axisEnd)
    }

    private static func floorToQuarterHour(_ minute: Int) -> Int { (minute / 15) * 15 }
    private static func ceilToQuarterHour(_ minute: Int) -> Int {
        let remainder = minute % 15
        return remainder == 0 ? minute : minute + (15 - remainder)
    }

    /// A folded night-minute (>= 1440 past midnight) to its ordinary
    /// clock hour label — "7 PM", "12 AM".
    private static func hourLabel(forFoldedMinute minute: Int) -> String {
        let wrapped = ((minute % 1440) + 1440) % 1440
        let hour24 = wrapped / 60
        let hour12 = hour24 % 12 == 0 ? 12 : hour24 % 12
        return "\(hour12) \(hour24 < 12 ? "AM" : "PM")"
    }
}
