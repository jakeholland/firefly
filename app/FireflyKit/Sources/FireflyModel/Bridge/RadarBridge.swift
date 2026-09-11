//
//  RadarBridge.swift — the Swift-safe wrapper over
//  `firmware/core/ff_radar` (docs/specs/A01-companion-app.md, slice B).
//
//  Heap-owns one `ff_radar_smooth_t` — the arrow's exponential-smoothing
//  state, which must persist across ticks (the smoothing time constant
//  needs the wall-clock delta between calls; see ff_radar.h's deviation
//  note). `ff_radar_view_t` itself is NOT a persistent context: nothing
//  in `ff_radar_compute` stores that pointer past the one call it is
//  handed to, so it is a local, stack-allocated scratch struct per
//  `compute()` call, decoded into `RadarView` and discarded — exactly
//  the "stack or heap struct the bridge owns for the duration of one
//  compute call" the spec's "Memory ownership" section describes for
//  `ff_radar_view_t`/`ff_inbox_t`/`ff_inbox_thread_t`.
//
import FireflyCore
import Foundation

/// `radar_mode_t` (core/ff_radar.h) — every mode `ff_radar_compute` can
/// resolve to, including the two amendments (`NOHDG`, `SIGNAL`) that
/// post-date the spec's original sketch.
public enum RadarMode: Sendable, Equatable, CaseIterable {
    case live, stale, lost, place, close, noFix, noHdg, signal, noSel

    init(ffMode: radar_mode_t) {
        switch ffMode {
        case RADAR_LIVE: self = .live
        case RADAR_STALE: self = .stale
        case RADAR_LOST: self = .lost
        case RADAR_PLACE: self = .place
        case RADAR_CLOSE: self = .close
        case RADAR_NOFIX: self = .noFix
        case RADAR_NOHDG: self = .noHdg
        case RADAR_SIGNAL: self = .signal
        default: self = .noSel
        }
    }
}

/// One crew-ring dot (`ff_radar_dot_t`).
public struct RadarDot: Sendable, Equatable {
    public let ringDeg: Float
    public let initial: Character?
    public let colorIndex: UInt8
    public let stale: Bool
    public let place: Bool
    public let imprecise: Bool
}

/// One inner "signal ring" entry (`ff_radar_signal_dot_t`, S29) — a
/// paired member who is heard but has no placeable position.
public struct RadarSignalDot: Sendable, Equatable {
    public let initial: Character?
    public let colorIndex: UInt8
    public let tier: SignalTierPresentation
    public let viaRelay: Bool
}

/// The whole radar-face view-state snapshot (`ff_radar_view_t`), decoded
/// into plain Swift values. `clock_str`/`batt_pct`/`mesh_ok` are
/// deliberately NOT included: `ff_radar_compute` never writes them (see
/// ff_radar.h's own deviation note — they come from the RTC/battery
/// ADC/mesh-link subsystems), so this bridge has nothing honest to
/// report for them either; a caller that has those facts renders them
/// from elsewhere.
public struct RadarView: Sendable, Equatable {
    public let mode: RadarMode
    /// Smoothed screen rotation, valid only when `arrowValid`.
    public let arrowDeg: Float
    public let arrowValid: Bool
    public let name: String
    public let distanceText: String
    /// True when `distanceText` is an approximate-AREA statement
    /// ("~5.8 km area"), never a raw point distance (issue #47).
    public let distanceImprecise: Bool
    public let ageText: String
    /// CLOSE/SIGNAL warmer(+)/colder(-)/steady(0).
    public let trend: RSSITrend
    /// Absolute true bearing — honestly knowable even without a
    /// heading (RADAR_NOHDG), unlike `arrowDeg`.
    public let bearingDeg: Float
    public let bearingValid: Bool
    /// The SELECTED member's own freshness reduced to the two booleans
    /// every ring dot already uses — populated unconditionally,
    /// independent of `mode` (see ff_radar.h's amendment: this is what
    /// lets RADAR_NOHDG's renderer pick a rim tint without fabricating
    /// one).
    public let place: Bool
    public let stale: Bool
    public let heardPresence: HeardPresence
    public let dots: [RadarDot]
    /// Computed unconditionally, independent of `mode` (S29).
    public let signalTier: SignalTierPresentation
    public let signalHeard: Bool
    public let signalViaRelay: Bool
    public let signalAgeText: String
    public let signalDots: [RadarSignalDot]

    static func decode(_ v: ff_radar_view_t) -> RadarView {
        RadarView(
            mode: RadarMode(ffMode: v.mode),
            arrowDeg: v.arrow_deg,
            arrowValid: v.arrow_valid,
            name: FixedCString.decode(v.name),
            distanceText: FixedCString.decode(v.dist_str),
            distanceImprecise: v.dist_imprecise,
            ageText: FixedCString.decode(v.age_str),
            trend: RSSITrend(raw: v.trend),
            bearingDeg: v.bearing_deg,
            bearingValid: v.bearing_valid,
            place: v.place,
            stale: v.stale,
            heardPresence: HeardPresence(ffPresence: v.heard_presence),
            dots: decodeDots(v),
            signalTier: SignalTierPresentation(ffTier: v.signal_tier),
            signalHeard: v.signal_heard,
            signalViaRelay: v.signal_via_relay,
            signalAgeText: FixedCString.decode(v.signal_age_str),
            signalDots: decodeSignalDots(v)
        )
    }

    /// `dots[FF_CREW_MAX]` imports as a fixed-size tuple with no
    /// per-index accessor; `withMemoryRebound` is the standard,
    /// function-scoped way to walk it — the pointer never escapes this
    /// call, so nothing here violates "no C pointer escapes its owner".
    private static func decodeDots(_ v: ff_radar_view_t) -> [RadarDot] {
        var v = v
        let n = Int(v.n_dots)
        guard n > 0 else { return [] }
        return withUnsafeMutablePointer(to: &v.dots) { tuplePtr in
            tuplePtr.withMemoryRebound(to: ff_radar_dot_t.self, capacity: Int(FF_CREW_MAX)) { arr in
                (0..<n).map { i in
                    let d = arr[i]
                    return RadarDot(ringDeg: d.ring_deg, initial: Character(ffInitial: d.initial),
                                     colorIndex: d.color_idx, stale: d.stale, place: d.place, imprecise: d.imprecise)
                }
            }
        }
    }

    private static func decodeSignalDots(_ v: ff_radar_view_t) -> [RadarSignalDot] {
        var v = v
        let n = Int(v.n_signal_dots)
        guard n > 0 else { return [] }
        return withUnsafeMutablePointer(to: &v.signal_dots) { tuplePtr in
            tuplePtr.withMemoryRebound(to: ff_radar_signal_dot_t.self, capacity: Int(FF_CREW_MAX)) { arr in
                (0..<n).map { i in
                    let d = arr[i]
                    return RadarSignalDot(initial: Character(ffInitial: d.initial), colorIndex: d.color_idx,
                                           tier: SignalTierPresentation(ffTier: d.tier), viaRelay: d.via_relay)
                }
            }
        }
    }
}

/// Heap-owns one `ff_radar_smooth_t`. One `RadarBridge` per radar
/// *session* (docs/specs/A01-companion-app.md's own use of "session" —
/// reset when the selection changes if a caller wants the next frame to
/// snap rather than sweep; rarely necessary per `ff_radar_smooth_reset`'s
/// own doc comment).
public final class RadarBridge {
    private let smooth: UnsafeMutablePointer<ff_radar_smooth_t>

    public init() {
        smooth = UnsafeMutablePointer<ff_radar_smooth_t>.allocate(capacity: 1)
        smooth.initialize(to: ff_radar_smooth_t())
        ff_radar_smooth_reset(smooth)
    }

    deinit {
        smooth.deinitialize(count: 1)
        smooth.deallocate()
    }

    public func resetSmoothing() {
        ff_radar_smooth_reset(smooth)
    }

    /// `headingDeg: nil` — or any negative value — means "heading
    /// unknown" (macOS: permanently, no magnetometer; iOS: an invalid
    /// `CLHeading` reading), the exact sentinel `ff_geo_heading_deg`
    /// itself returns and `ff_radar_compute` checks for RADAR_NOHDG.
    /// `myPosition: nil` means MY position is unknown (RADAR_NOFIX).
    public func compute(crew: CrewStore, headingDeg: Float?,
                         myPosition: (latitude: Double, longitude: Double)?,
                         imperial: Bool, now: UInt32) -> RadarView {
        var view = ff_radar_view_t()
        let myPos = myPosition.map { ff_latlon_t(lat: $0.latitude, lon: $0.longitude) } ?? ff_latlon_t(lat: 0, lon: 0)
        let heading: Float
        if let headingDeg, headingDeg >= 0 { heading = headingDeg } else { heading = -1 }
        ff_radar_compute(&view, smooth, crew.raw, heading, myPos, myPosition != nil, imperial, now)
        return RadarView.decode(view)
    }
}
