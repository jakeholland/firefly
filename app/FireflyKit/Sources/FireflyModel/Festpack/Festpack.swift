//
//  Festpack.swift — the phone's plain-Swift festival data model
//  (docs/specs/S05-festpack.md; fest-almanac schema v0.1).
//
//  Decoded entirely from `fp_pack_t`, via `FestpackParser.swift`'s call
//  into the SAME C parser (`fp_parse`) the puck uses — never a second,
//  independent JSON schema parser in Swift (the coordinator's explicit
//  instruction). Everything below is a plain Sendable value; no C
//  pointer or struct escapes the bridge (the same "Data flow" rule
//  Bridge/* already follows — see CString+Swift.swift's header comment).
//
import Foundation

public struct FestpackStage: Sendable, Equatable, Identifiable, Hashable {
    public let id: String
    public let name: String
    /// 0x00RRGGBB, straight off `fp_stage_t.color_rgb`.
    public let colorRGB: UInt32

    public init(id: String, name: String, colorRGB: UInt32) {
        self.id = id
        self.name = name
        self.colorRGB = colorRGB
    }
}

public struct FestpackScheduleSet: Sendable, Equatable, Identifiable {
    /// This set's index into the parsed `fp_pack_t.sets[]` — stable
    /// only within ONE loaded `Festpack`, never across a re-fetch (a
    /// refreshed pack can reorder/add/remove sets). Starring therefore
    /// keys off `PicksCodec.setID(for:in:)` — a value derived from a
    /// set's own content — not `id`, see `PicksStore`.
    public let id: Int
    public let artist: String
    public let stageID: String?
    /// Day-of-year the festival NIGHT this set is billed under, already
    /// folded per S05's 2026-09-09 amendment: an after-midnight set
    /// carries the night BEFORE its calendar start, not the calendar day
    /// it technically starts on.
    public let nightDayOfYear: Int
    /// Minutes from that night's local midnight; `nil` = unknown
    /// (`fp_set_t.start_min == -1`). `>= 1440` for an after-midnight
    /// set — the same minute space `ff_sched` uses, so sorting by this
    /// value already puts after-midnight sets after the pre-midnight
    /// ones on the same night.
    public let startMinute: Int?
    public let endMinute: Int?
    public let note: String

    public init(id: Int, artist: String, stageID: String?, nightDayOfYear: Int,
                startMinute: Int?, endMinute: Int?, note: String) {
        self.id = id
        self.artist = artist
        self.stageID = stageID
        self.nightDayOfYear = nightDayOfYear
        self.startMinute = startMinute
        self.endMinute = endMinute
        self.note = note
    }
}

public enum FestpackFeatureKind: Sendable, Equatable, CaseIterable {
    case unknown, stage, camping, water, path, entrance, vendor, medical, poi
}

public struct FestpackPoint: Sendable, Equatable {
    public let eastMeters: Double
    public let northMeters: Double
    public init(eastMeters: Double, northMeters: Double) {
        self.eastMeters = eastMeters
        self.northMeters = northMeters
    }
}

public struct FestpackFeature: Sendable, Equatable, Identifiable {
    public let id: Int
    public let kind: FestpackFeatureKind
    public let stageID: String?
    public let label: String
    /// Projected at parse time (`ff_geo_project`, meaningful only when
    /// the owning `Festpack.originKnown` is true — same caveat
    /// `fp_landmark_t`/`fp_feature_t`'s own doc comments carry). Empty
    /// when the pack's polygon/point for this feature was null.
    public let points: [FestpackPoint]

    public init(id: Int, kind: FestpackFeatureKind, stageID: String?, label: String, points: [FestpackPoint]) {
        self.id = id
        self.kind = kind
        self.stageID = stageID
        self.label = label
        self.points = points
    }
}

public struct FestpackLandmark: Sendable, Equatable, Identifiable {
    public let id: String
    public let name: String
    public let position: FestpackPoint?

    public init(id: String, name: String, position: FestpackPoint?) {
        self.id = id
        self.name = name
        self.position = position
    }
}

/// `fp_meta_t` (2026-09-11 S05 amendment) — the pack's own provenance:
/// when fest-almanac last updated it, where it sourced the data, and how
/// complete each section is. `updated`/`sources`/completeness strings
/// are exactly what the pack's authors wrote — this is display data, not
/// a claim this app verifies.
public struct FestpackMeta: Sendable, Equatable {
    /// `false` iff the pack carried no top-level "meta" object at all —
    /// every other field on this type reads as empty/nil in that case,
    /// never a fabricated "unknown" string.
    public let present: Bool
    /// `meta.updated`, an ISO date string ("2026-09-09") straight off
    /// the pack — never reformatted or reinterpreted as a `Date` here,
    /// since the pack does not commit to a time-of-day or timezone for
    /// this field.
    public let updated: String?
    public let sources: [String]
    public let completeLineup: String?
    public let completeSetTimes: String?
    public let completeMap: String?

    public init(present: Bool, updated: String?, sources: [String],
                completeLineup: String?, completeSetTimes: String?, completeMap: String?) {
        self.present = present
        self.updated = updated
        self.sources = sources
        self.completeLineup = completeLineup
        self.completeSetTimes = completeSetTimes
        self.completeMap = completeMap
    }

    public static let empty = FestpackMeta(present: false, updated: nil, sources: [],
                                            completeLineup: nil, completeSetTimes: nil, completeMap: nil)
}

/// The phone's whole-pack value — everything `fp_pack_t` carries, in
/// Swift-native shapes, plus `FestpackMeta` (an S05 amendment the C
/// struct did not carry before this feature). This is what
/// `FestpackProviding.current` hands out and what `festpackUpdates()`
/// streams.
public struct Festpack: Sendable, Equatable {
    public let name: String
    public let year: Int
    /// Day-of-year (1-based, Gregorian) of the festival's first and last
    /// calendar day — `fp_pack_t.start_doy`/`end_doy`. Use `date(forDayOfYear:)`
    /// to turn one of these (or a schedule set's `nightDayOfYear`) into an
    /// actual `Date` in the festival's own timezone.
    public let startDayOfYear: Int
    public let endDayOfYear: Int
    /// Minutes east of UTC — the schema's only timezone representation
    /// (see `fp_pack.h`'s own doc comment: no IANA name, no DST rule, one
    /// fixed offset for the whole event). This is honestly all the "timezone"
    /// this pack format carries; do not infer an IANA zone from it.
    public let utcOffsetMinutes: Int
    /// `true` iff the pack omitted `utc_offset_min` and this is the
    /// documented -240 (EDT) fallback, not a value the pack actually stated.
    public let utcOffsetAssumed: Bool
    public let originKnown: Bool
    public let originApproximate: Bool
    /// `fp_pack_t.origin` — the festival.venue lat/lon `fp_parse` used
    /// as the projection origin for every `FestpackPoint` in `features`/
    /// `landmarks` below. Meaningless (reads as 0,0) unless `originKnown`
    /// is true, exactly like the C struct's own doc comment warns for
    /// `origin`/any projected `eastMeters`/`northMeters` — callers MUST
    /// check `originKnown` before trusting these two fields, same as
    /// they already must for `FestpackPoint`. Added for the Map tab's
    /// own adapter (`Map/FestpackProvidingMapAdapter.swift`), which
    /// needs the real anchor to recover WGS84 points from this pack's
    /// projected meters — `fp_parse` always computes this value; it was
    /// simply never read back out into Swift before that adapter needed
    /// it.
    public let originLatitude: Double
    public let originLongitude: Double
    public let stages: [FestpackStage]
    public let sets: [FestpackScheduleSet]
    public let features: [FestpackFeature]
    public let landmarks: [FestpackLandmark]
    public let meta: FestpackMeta

    public init(name: String, year: Int, startDayOfYear: Int, endDayOfYear: Int,
                utcOffsetMinutes: Int, utcOffsetAssumed: Bool, originKnown: Bool, originApproximate: Bool,
                originLatitude: Double = 0, originLongitude: Double = 0,
                stages: [FestpackStage], sets: [FestpackScheduleSet], features: [FestpackFeature],
                landmarks: [FestpackLandmark], meta: FestpackMeta) {
        self.name = name
        self.year = year
        self.startDayOfYear = startDayOfYear
        self.endDayOfYear = endDayOfYear
        self.utcOffsetMinutes = utcOffsetMinutes
        self.utcOffsetAssumed = utcOffsetAssumed
        self.originKnown = originKnown
        self.originApproximate = originApproximate
        self.originLatitude = originLatitude
        self.originLongitude = originLongitude
        self.stages = stages
        self.sets = sets
        self.features = features
        self.landmarks = landmarks
        self.meta = meta
    }

    /// A fixed-offset `TimeZone` built from `utcOffsetMinutes` — the only
    /// "timezone" this schema can honestly express. `nil` only if the
    /// offset is out of `TimeZone`'s own representable range (never true
    /// for a real festpack; defensive rather than force-unwrapped).
    public var timeZone: TimeZone? { TimeZone(secondsFromGMT: utcOffsetMinutes * 60) }

    /// How many festival calendar days this pack spans — `endDayOfYear`
    /// is inclusive (S05: "festival.start"/"festival.end"), so a
    /// same-day event is 1, not 0. Never negative even if the pack's
    /// dates are malformed in a way `fp_parse` still accepted (defensive
    /// floor at 1 — a 0- or negative-day festival is not a fact to
    /// render).
    public var dayCount: Int { max(1, endDayOfYear - startDayOfYear + 1) }

    /// 1-based festival day index for `dayOfYear` ("Day N of `dayCount`"),
    /// clamped into `[1, dayCount]` — a `dayOfYear` outside the festival's
    /// own span (should not happen for a set drawn from this same pack)
    /// reads as the nearest edge rather than an out-of-range number.
    public func dayIndex(forDayOfYear dayOfYear: Int) -> Int {
        let raw = dayOfYear - startDayOfYear + 1
        return min(max(raw, 1), dayCount)
    }

    /// Converts a Gregorian day-of-year (1-based) in `year` to a `Date`
    /// at local midnight in this pack's `timeZone`. Pure calendar
    /// arithmetic (Jan 1 + (dayOfYear - 1) days) — never reads the real
    /// clock.
    public func date(forDayOfYear dayOfYear: Int) -> Date? {
        guard let timeZone else { return nil }
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        guard let jan1 = calendar.date(from: DateComponents(year: year, month: 1, day: 1)) else { return nil }
        return calendar.date(byAdding: .day, value: dayOfYear - 1, to: jan1)
    }

    public func stage(withID id: String?) -> FestpackStage? {
        guard let id else { return nil }
        return stages.first { $0.id == id }
    }
}
