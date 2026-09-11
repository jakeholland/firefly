//
//  FestpackSchedule.swift — the Swift-safe wrapper over `ff_sched`
//  (firmware/festpack/include/ff_sched.h) and `ff_wall_split_local`
//  (firmware/core/include/ff_wall.h).
//
//  Now/next-per-stage, the day lineup, and the "starred set upcoming"
//  question are all schedule-schema logic (the half-open "now" window,
//  derived-end-from-next-set, the after-midnight fold) that
//  `ff_sched.c` already implements and the puck already ships — the
//  coordinator's instruction was to bind it, not reimplement it.
//
//  `Festpack` (this app's public model) is a plain, already-decoded
//  Swift value with no `fp_pack_t` inside it — by the time it exists,
//  the C struct that produced it is long gone (`FestpackParser.parse`'s
//  own doc comment: "C types never leave the bridge"). So every call
//  here RE-ENCODES a transient `fp_pack_t` from the `Festpack` plus the
//  caller's current starred set, calls straight into `ff_sched`, and
//  decodes the result back to plain Swift before returning — the same
//  "stack scratch the bridge owns for the duration of one call"
//  convention `InboxBridge.conversations(crew:now:)` documents for
//  itself. A pack tops out at 256 sets; re-encoding it is a bounded,
//  cheap memcpy loop, not a hot-path concern.
//
import FireflyCore
import Foundation

public enum FestpackSchedule {
    /// `ff_now_row_t`, decoded.
    public struct NowRow: Sendable, Equatable, Identifiable {
        public var id: Int { self.set.id }
        public var set: FestpackScheduleSet
        /// `effective_end - now_min`; meaningless (still present, but
        /// see `percentValid`) when the set's true end is unknowable.
        public var minutesLeft: Int
        public var percentDone: Int
        /// `false` iff this set's end is genuinely unknowable (last
        /// known-start set on its stage this night, no explicit end) —
        /// see `ff_now_row_t.pct_valid`'s own doc comment. Callers must
        /// gate any progress bar/percentage on this, never on
        /// `percentDone` alone.
        public var percentValid: Bool
    }

    /// `ff_next_t`, decoded.
    public struct NextStarred: Sendable, Equatable {
        public var set: FestpackScheduleSet
        public var minutesUntil: Int
    }

    /// `ff_sched_now_playing` — every set live on `night` (day-of-year)
    /// at `nowMinute`, one row per stage. See `ff_sched.h`'s own doc
    /// comment for the half-open `[start, end)` window and the
    /// derived-end-from-next-set rule this does not reimplement.
    public static func nowPlaying(in pack: Festpack, night: Int, nowMinute: Int, starred: Set<String> = []) -> [NowRow] {
        withEncodedPack(pack, starred: starred) { cpack, stageIDs in
            withUnsafePointer(to: cpack) { pptr -> [NowRow] in
                var rawRows = [ff_now_row_t](repeating: ff_now_row_t(), count: Int(FP_MAX_STAGES))
                let n: UInt8 = rawRows.withUnsafeMutableBufferPointer { buf in
                    ff_sched_now_playing(pptr, UInt16(night), Int16(nowMinute), buf.baseAddress, UInt8(FP_MAX_STAGES))
                }
                return (0..<Int(n)).compactMap { i -> NowRow? in
                    guard let setPointer = rawRows[i].set else { return nil }
                    let decoded = FestpackParser.decodeSet(setPointer.pointee, id: i, stageIDs: stageIDs)
                    return NowRow(set: decoded, minutesLeft: Int(rawRows[i].mins_left),
                                  percentDone: Int(rawRows[i].pct_done), percentValid: rawRows[i].pct_valid)
                }
            }
        }
    }

    /// `ff_sched_next_starred` — the earliest not-yet-started starred
    /// set on `night`. `nil` if nothing qualifies (nothing starred,
    /// nothing starred on this night, or every starred set on this
    /// night has already started/finished/has no known start).
    public static func nextStarred(in pack: Festpack, night: Int, nowMinute: Int, starred: Set<String>) -> NextStarred? {
        guard !starred.isEmpty else { return nil }
        return withEncodedPack(pack, starred: starred) { cpack, stageIDs in
            withUnsafePointer(to: cpack) { pptr -> NextStarred? in
                var out = ff_next_t()
                let found = ff_sched_next_starred(pptr, UInt16(night), Int16(nowMinute), &out)
                guard found, let setPointer = out.set else { return nil }
                let decoded = FestpackParser.decodeSet(setPointer.pointee, id: 0, stageIDs: stageIDs)
                return NextStarred(set: decoded, minutesUntil: Int(out.mins_until))
            }
        }
    }

    /// `ff_sched_day_sets` — every set attributed to `night`, pack
    /// order, INCLUDING ones with unknown times (the per-night lineup
    /// scroll — S05/S07: unlike `nowPlaying`, a null-time set still
    /// belongs here). Sorted by `startMinute` ascending (unknown times
    /// last) for display — since an after-midnight `startMinute` is
    /// already `>= 1440` in the same folded minute space `ff_sched`
    /// uses, this sort is what puts after-midnight sets after the
    /// pre-midnight ones on the same night, per S05's 2026-09-09
    /// amendment, with zero midnight special-casing here.
    public static func daySets(in pack: Festpack, night: Int) -> [FestpackScheduleSet] {
        let sets: [FestpackScheduleSet] = withEncodedPack(pack, starred: []) { cpack, stageIDs in
            withUnsafePointer(to: cpack) { pptr -> [FestpackScheduleSet] in
                var pointers = [UnsafePointer<fp_set_t>?](repeating: nil, count: Int(FP_MAX_SETS))
                let n: UInt16 = pointers.withUnsafeMutableBufferPointer { buf in
                    ff_sched_day_sets(pptr, UInt16(night), buf.baseAddress, UInt16(FP_MAX_SETS))
                }
                return (0..<Int(n)).compactMap { i -> FestpackScheduleSet? in
                    guard let p = pointers[i] else { return nil }
                    return FestpackParser.decodeSet(p.pointee, id: i, stageIDs: stageIDs)
                }
            }
        }
        return sets.sorted { lhs, rhs in
            switch (lhs.startMinute, rhs.startMinute) {
            case let (l?, r?): return l < r
            case (nil, nil): return lhs.artist < rhs.artist
            case (nil, _): return false
            case (_, nil): return true
            }
        }
    }

    /// `ff_sched_day_tbd` — true iff `night` has at least one set and
    /// NONE of them have a known start time ("SET TIMES TBD").
    public static func dayIsAllTBD(in pack: Festpack, night: Int) -> Bool {
        withEncodedPack(pack, starred: []) { cpack, _ in
            withUnsafePointer(to: cpack) { ff_sched_day_tbd($0, UInt16(night)) }
        }
    }

    // MARK: - Transient fp_pack_t re-encoding

    private static func withEncodedPack<R>(_ pack: Festpack, starred: Set<String>, _ body: (fp_pack_t, [String]) -> R) -> R {
        var cpack = fp_pack_t()
        let stageIDs = pack.stages.map(\.id)
        let stageIndex = Dictionary(uniqueKeysWithValues: stageIDs.enumerated().map { ($1, $0) })

        withUnsafeMutableBytes(of: &cpack.stages) { raw in
            let items = raw.bindMemory(to: fp_stage_t.self)
            for (i, stage) in pack.stages.prefix(Int(FP_MAX_STAGES)).enumerated() {
                var s = fp_stage_t()
                FixedCString.encode(stage.id, into: &s.id)
                FixedCString.encode(stage.name, into: &s.name)
                s.color_rgb = stage.colorRGB
                items[i] = s
            }
        }
        cpack.n_stages = UInt8(min(pack.stages.count, Int(FP_MAX_STAGES)))

        withUnsafeMutableBytes(of: &cpack.sets) { raw in
            let items = raw.bindMemory(to: fp_set_t.self)
            for (i, set) in pack.sets.prefix(Int(FP_MAX_SETS)).enumerated() {
                var s = fp_set_t()
                FixedCString.encode(set.artist, into: &s.artist)
                s.stage_idx = set.stageID.flatMap { stageIndex[$0] }.map { Int8($0) } ?? -1
                s.day_doy = UInt16(set.nightDayOfYear)
                s.start_min = Int16(set.startMinute ?? -1)
                s.end_min = Int16(set.endMinute ?? -1)
                FixedCString.encode(set.note, into: &s.note)
                s.starred = starred.contains(set.artist)
                items[i] = s
            }
        }
        cpack.n_sets = UInt16(min(pack.sets.count, Int(FP_MAX_SETS)))

        return body(cpack, stageIDs)
    }
}

/// The phone-side wall-clock fold: `ff_wall_split_local` binds a
/// unix-seconds reading + the pack's own UTC offset into the
/// `(day_doy, now_min)` pair `ff_sched` consumes. Unlike the puck
/// (`ff_wall.h`'s whole header is about whether an RF-derived timestamp
/// can be TRUSTED at all), the phone's own system clock always is —
/// this file's only job is the same festival-day arithmetic
/// (06:00 roll, `[360, 1800)` window), not a second, hand-rolled copy
/// of it.
public enum FestpackWallClock {
    /// `nil` iff `now` falls outside `ff_wall_split_local`'s fixed
    /// plausibility window (`FF_WALL_EPOCH_FLOOR`..`FF_WALL_EPOCH_CEILING`,
    /// documented in `ff_wall.h` — presently 2026-08-01 through
    /// 2030-08-01) or `utcOffsetMinutes` is out of range. That is an
    /// honest "cannot resolve a festival night from this clock right
    /// now" answer, never a fabricated one.
    public static func split(now: Date, utcOffsetMinutes: Int) -> (dayOfYear: Int, nowMinute: Int)? {
        var dayDoy: UInt16 = 0
        var nowMin: Int16 = 0
        let unixSeconds = Int64(now.timeIntervalSince1970)
        guard ff_wall_split_local(unixSeconds, Int16(utcOffsetMinutes), &dayDoy, &nowMin) else { return nil }
        return (Int(dayDoy), Int(nowMin))
    }
}
