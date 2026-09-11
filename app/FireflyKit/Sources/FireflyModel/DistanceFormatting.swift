//
//  DistanceFormatting.swift — the ONE seam every distance/area string in
//  the app renders through (M2; PR #265's review flagged the bug this
//  closes — see `SettingsStoring.swift`'s "Units preference" section).
//
//  Two rules this file exists to enforce mechanically, not by
//  convention:
//
//   1. Metric/imperial threshold math is never reimplemented in Swift.
//      `CrewStore.formatDistance(meters:imperial:)` already calls
//      straight into `ff_fmt_distance` (firmware/core/src/ff_crew.c) —
//      the SAME function the puck itself formats a distance with — so
//      routing every Swift-side distance string through it (rather than
//      a hand-rolled "< 1000 ? m : km" of its own) is what makes "the
//      app agrees with the puck about unit boundaries" true by
//      construction instead of by two authors independently reading the
//      same threshold off a doc comment.
//
//   2. A distance is always formatted from its raw metres, never by
//      taking an already-formatted string apart and re-rendering it in
//      the other unit system — that would be lossy (a "5 km" already
//      rounded to one decimal loses the precision an honest ft/mi
//      conversion needs) and fragile (parsing "~5.8 km" back apart to
//      recover 5800.0 is exactly the kind of string-shape coupling a
//      formatting seam exists to remove). Every function below takes
//      `Double` metres in and hands back a fresh string.
//
//  Radar's own `dist_str` (`RadarBridge`/`ff_radar_compute`) does NOT
//  route through this file's functions: it already calls
//  `ff_fmt_distance` itself, with the caller-supplied `imperial` flag
//  baked into that ONE compute() call, so it is honest by the same
//  construction as this file for free — see `AppGraph
//  .makeRadarViewModel`'s own comment. This file is for every OTHER
//  distance a Swift view model computes on its own from raw metres
//  (a RALLY packet's lat/lon distance, a Diagnostics position-accuracy
//  reading) — currently none render in M1, but wiring one up needs
//  exactly one call here, never a second implementation of these rules.
//
import Foundation

public enum DistanceFormatting {
    /// A plain point distance ("42 m", "1.1 km", "137 ft", "2.4 mi").
    /// `preference` resolves against `locale` when `.system` (see
    /// `UnitsPreference.resolvedImperial(locale:)`).
    public static func distance(meters: Double, preference: UnitsPreference,
                                 locale: Locale = .current) -> String {
        distance(meters: meters, imperial: preference.resolvedImperial(locale: locale))
    }

    /// The same formatting, given an already-resolved unit system —
    /// the seam `RadarViewModel`'s own honesty markers (see below) and
    /// `distance(meters:preference:locale:)` both funnel through, so
    /// there is exactly one call to `CrewStore.formatDistance` in this
    /// file.
    public static func distance(meters: Double, imperial: Bool) -> String {
        CrewStore.formatDistance(meters: Float(meters), imperial: imperial)
    }

    /// An approximate-AREA statement ("~5.8 km", "~3.6 mi") — issue
    /// #47's own honesty marker: a degraded-precision fix is a CELL,
    /// not a point, and the "~" must never go missing just because the
    /// unit system changed underneath it. Same thresholds as
    /// `distance(meters:preference:locale:)`, prefixed.
    public static func areaDistance(meters: Double, preference: UnitsPreference,
                                     locale: Locale = .current) -> String {
        "~" + distance(meters: meters, preference: preference, locale: locale)
    }

    public static func areaDistance(meters: Double, imperial: Bool) -> String {
        "~" + distance(meters: meters, imperial: imperial)
    }
}
