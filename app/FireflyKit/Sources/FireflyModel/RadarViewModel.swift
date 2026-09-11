//
//  RadarViewModel.swift — Slice D: the Radar face's whole view model.
//
//  Spec: docs/specs/A01-companion-app.md ("Slice D — Radar"),
//  docs/specs/S06-radar-face.md (every `radar_mode_t`, all amendments),
//  docs/specs/S29-radio-only.md (the no-GPS signal view and FIND).
//
//  ## The integration seam this file defines for slice B
//
//  Slice B (the C-core bridge) owns `FireflyModel/Bridge/*` — a real
//  `RadarBridge` wrapping `ff_crew_t`/`ff_radar_compute` and a real
//  `FindBridge` wrapping `ff_find_t` — and is being built in parallel.
//  Rather than invent those files here (out of this slice's file list,
//  A01's shared-file table), this file defines the two narrow protocols
//  `RadarComputing` and `FindPinging` that `RadarViewModel` depends on,
//  plus honest mocks (`MockRadarComputing`, `MockFindSession`) that
//  invent nothing — the exact shape `StubMeshtasticClient` already
//  established for the mesh client seam (A01, "the stub client's
//  defining property is what it *refuses* to do").
//
//  INTEGRATED: slice B's bridges now fill both seams for real, through
//  `Live/LiveAdapters.swift` (`CoreRadarComputing` over `ff_crew` +
//  `ff_radar_compute`, `CoreFindSession` over `ff_find` plus a real
//  portnum-269 send), composed in `AppGraph.makeRadarViewModel(haptics:)`
//  — and, exactly as this comment promised, nothing else in this file
//  or in `app/Firefly/Sources/Radar/*` changed to make that happen. The
//  two mocks stay as TEST doubles (`RadarViewModel.mocked(...)`), since
//  `RadarViewModelTests` drives them with exact field values transcribed
//  from `firmware/tests/fixtures/radar_*.json`.
//
//  ALL bearing/distance/geometry math is either already computed by
//  whatever fills in `RadarSnapshot` (the C core, via slice B's real
//  bridge) or, for the two pure/stateless C helpers already safe to call
//  directly through `FireflyCore` (`ff_radar_signal_tier`,
//  `ff_geo_compass_point` — the same pattern `SignalPresentation.swift`
//  already uses for the former), called through `FireflyCore` rather
//  than reimplemented here. This file never computes a bearing, a
//  distance, or a compass point from raw coordinates itself.
//
import FireflyCore
import Foundation
import Observation

// MARK: - Radar mode (mirrors `radar_mode_t`, core/include/ff_radar.h)
//
// `RadarMode` itself now comes from slice B's `Bridge/RadarBridge.swift`
// (same 9 cases this file used to declare as its own stand-in —
// `live/stale/lost/place/close/noFix/noHdg/signal/noSel` — just this
// file's `nofix/nohdg/nosel` recased to match, see call sites below): B
// has landed in this tree, so the duplicate declared here collided with
// the bridge's own `RadarMode` as two top-level types of the same name
// in this module — exactly the convergence this file's header comment
// already anticipated ("slice B's RadarBridge... swapped in... nothing
// else in this file changes").

// MARK: - Heard presence (mirrors `ff_crew_presence_t`, core/include/ff_crew.h)
//
// `HeardPresence` itself now comes from slice B's `Bridge/CrewStore.swift`
// — identical 4 cases (`heard/stale/lost/never`) and the same
// `init(ffPresence:)` mapping this file used to declare as its own
// stand-in. B has landed in this tree, so the duplicate declared here
// collided with the bridge's own `HeardPresence` as two top-level types
// of the same name in this module.

// MARK: - Compass point (wraps `ff_geo_compass_point` — a pure, stateless

// C helper, called directly through FireflyCore exactly the way
// `SignalPresentation.tier(rssiDbm:)` already calls `ff_radar_signal_tier`
// — never reimplemented in Swift.
public enum CompassPoint {
    /// The 16-point compass name (N, NNE, NE, ... NNW) for an absolute
    /// true bearing in degrees. `ff_geo_compass_point` wraps any input
    /// internally, so this accepts any finite value.
    public static func name(forBearingDegrees deg: Double) -> String {
        var buf: (CChar, CChar, CChar, CChar) = (0, 0, 0, 0)
        withUnsafeMutableBytes(of: &buf) { raw in
            let ptr = raw.baseAddress!.assumingMemoryBound(to: CChar.self)
            ff_geo_compass_point(Float(deg), ptr)
        }
        return withUnsafeBytes(of: &buf) { raw in
            String(cString: raw.baseAddress!.assumingMemoryBound(to: CChar.self))
        }
    }
}

// MARK: - View-state value types (mirror `ff_radar_view_t` field-for-field)

public struct RadarSnapshotDot: Sendable, Equatable, Identifiable {
    public var id: Int
    public var ringDegrees: Double
    public var initial: Character
    public var colorIndex: Int
    /// STALE/LOST/NEVER, per that member's own freshness. Never true
    /// alongside `place`.
    public var stale: Bool
    /// That member's latest position is asserted (`FF_FRESH_ASSERTED`),
    /// not measured — a place marker, not a friend dot.
    public var place: Bool
    /// Degraded precision (`< FF_CREW_POS_PRECISION_MIN_BITS`) — the
    /// coordinate could be kilometers off; independent of stale/place.
    public var imprecise: Bool

    public init(id: Int, ringDegrees: Double, initial: Character, colorIndex: Int,
                stale: Bool, place: Bool, imprecise: Bool) {
        self.id = id
        self.ringDegrees = ringDegrees
        self.initial = initial
        self.colorIndex = colorIndex
        self.stale = stale
        self.place = place
        self.imprecise = imprecise
    }
}

/// One entry on the S29 inner "signal ring" — a paired member who is
/// heard but has no placeable position. Mutually exclusive with
/// `RadarSnapshotDot` by construction (a positioned member is on the ordinary
/// ring, never both).
public struct RadarSnapshotSignalDot: Sendable, Equatable, Identifiable {
    public var id: Int
    public var initial: Character
    public var colorIndex: Int
    /// `.none` when `viaRelay` — a relayed packet's RSSI belongs to the
    /// relay, never the sender.
    public var tier: SignalTierPresentation
    public var viaRelay: Bool

    public init(id: Int, initial: Character, colorIndex: Int, tier: SignalTierPresentation, viaRelay: Bool) {
        self.id = id
        self.initial = initial
        self.colorIndex = colorIndex
        self.tier = tier
        self.viaRelay = viaRelay
    }
}

/// The whole Radar view-state snapshot — the Swift mirror of
/// `ff_radar_view_t` (docs/specs/S06-radar-face.md, S29's additive
/// fields). `RadarViewModel` renders this and only this; it derives no
/// geometry of its own.
public struct RadarSnapshot: Sendable, Equatable {
    public var mode: RadarMode
    public var arrowDegrees: Double
    /// False in CLOSE/NOFIX/NOHDG/NOSEL, and whenever the selection has
    /// no position fix to point at.
    public var arrowValid: Bool
    public var name: String
    /// "" when unknown. Never a fabricated number — an approximate-area
    /// statement (e.g. "~5.8 km") when `distanceImprecise`.
    public var distanceText: String
    public var distanceImprecise: Bool
    /// "" when there has never been a fix (the NEVER-folded-into-LOST
    /// case) — see `heardPresence` for how the renderer tells that apart
    /// from a genuinely stale one.
    public var ageText: String
    /// -1 / 0 / +1 — CLOSE's and SIGNAL's WARMER/COLDER/STEADY.
    public var trend: Int
    /// Absolute true bearing, [0, 360) — needs no heading, honestly
    /// knowable even in NOHDG.
    public var bearingDegrees: Double
    public var bearingValid: Bool
    /// The selection's OWN freshness, mirrored independent of `mode` —
    /// NOHDG's early return happens before the ordinary freshness switch,
    /// so the renderer needs these to know whether to show the STALE rim
    /// tint at all.
    public var place: Bool
    public var stale: Bool
    /// The HEARD axis (any packet, not position) — distinguishes "near,
    /// no fix" from a genuinely silent radio in the never-fixed LOST case.
    public var heardPresence: HeardPresence
    public var dots: [RadarSnapshotDot]

    // S29 — computed unconditionally, independent of mode.
    public var signalTier: SignalTierPresentation
    public var signalHeard: Bool
    public var signalViaRelay: Bool
    public var signalAgeText: String
    public var signalDots: [RadarSnapshotSignalDot]

    // Not derived by the compute step — populated by the caller from the
    // clock/battery/mesh-link subsystems, mirroring `ff_radar_view_t`'s
    // own "NOT written by ff_radar_compute" fields.
    public var clockText: String
    public var battPct: Int8?
    public var meshOK: Bool

    public init(mode: RadarMode, arrowDegrees: Double, arrowValid: Bool, name: String,
                distanceText: String, distanceImprecise: Bool, ageText: String, trend: Int,
                bearingDegrees: Double, bearingValid: Bool, place: Bool, stale: Bool,
                heardPresence: HeardPresence, dots: [RadarSnapshotDot], signalTier: SignalTierPresentation,
                signalHeard: Bool, signalViaRelay: Bool, signalAgeText: String,
                signalDots: [RadarSnapshotSignalDot], clockText: String, battPct: Int8?, meshOK: Bool) {
        self.mode = mode
        self.arrowDegrees = arrowDegrees
        self.arrowValid = arrowValid
        self.name = name
        self.distanceText = distanceText
        self.distanceImprecise = distanceImprecise
        self.ageText = ageText
        self.trend = trend
        self.bearingDegrees = bearingDegrees
        self.bearingValid = bearingValid
        self.place = place
        self.stale = stale
        self.heardPresence = heardPresence
        self.dots = dots
        self.signalTier = signalTier
        self.signalHeard = signalHeard
        self.signalViaRelay = signalViaRelay
        self.signalAgeText = signalAgeText
        self.signalDots = signalDots
        self.clockText = clockText
        self.battPct = battPct
        self.meshOK = meshOK
    }

    /// The honest empty state: no selection, nothing invented. What
    /// every mock/stub starts from and what a never-observed view model
    /// renders — the same "an empty Radar on a stub is the honest
    /// answer" rule `AppDependencies.stub()` documents for the client.
    public static let empty = RadarSnapshot(
        mode: .noSel, arrowDegrees: 0, arrowValid: false, name: "",
        distanceText: "", distanceImprecise: false, ageText: "", trend: 0,
        bearingDegrees: 0, bearingValid: false, place: false, stale: false,
        heardPresence: .never, dots: [], signalTier: .none, signalHeard: false,
        signalViaRelay: false, signalAgeText: "", signalDots: [],
        clockText: "", battPct: nil, meshOK: false)
}

// MARK: - RadarComputing — the bridge seam (slice B fills this in for real)

/// The narrow protocol `RadarViewModel` depends on for "what does the
/// radar look like right now" and "advance the selection". Slice B's
/// real `RadarBridge` (`FireflyModel/Bridge/RadarBridge.swift`) wraps
/// `ff_crew_t` + `ff_radar_compute` and conforms to this; until it lands,
/// `MockRadarComputing` stands in.
///
/// `compute` is the ONLY place geometry crosses this seam, and it always
/// returns an already-computed `RadarSnapshot` — no bearing, distance, or
/// mode-resolution logic belongs on the caller's side of this protocol.
///
/// M3 / Swift 6: `@MainActor`, not `Sendable`. Every conformance
/// (`CoreRadarComputing`, `MockRadarComputing`) and every call site
/// (`RadarViewModel`, itself `@MainActor`) already lived on the main
/// actor; `Sendable` plus `nonisolated` plus `MainActor.assumeIsolated`
/// at each entry point (the shape this protocol used before M3) was
/// this seam ASSERTING that placement at runtime instead of the
/// compiler PROVING it — exactly the ad-hoc pattern M3's concurrency
/// review flagged. Isolating the protocol proves it instead, and no
/// caller changes: `RadarViewModel`'s own members were already
/// `@MainActor`-isolated.
@MainActor
public protocol RadarComputing: AnyObject {
    /// True when at least one crew member is currently paired — lets the
    /// view model know whether "cycle selection" has anything to do,
    /// without duplicating the crew roster itself.
    var hasPairedMembers: Bool { get }

    /// The currently selected member's raw Meshtastic node id, or nil in
    /// NOSEL — `ff_radar_view_t` itself carries no node id (it is a pure
    /// render projection), so this is a second, narrow accessor onto
    /// whatever slice B's real `CrewStore`/`RadarBridge` already tracks,
    /// needed only for actions that must name a wire address (FIND's
    /// `mc_send_private` target) rather than render anything.
    var selectedNodeID: UInt32? { get }

    /// Tap-center gesture (S06): cycle to the next paired member. A
    /// no-op with zero paired members.
    func cycleSelection()

    /// Recompute the view for the given heading/position inputs.
    /// `headingDegrees == nil` mirrors `ff_geo_heading_deg`'s own
    /// negative "unreliable" sentinel — no magnetometer (macOS,
    /// permanently) or no valid reading yet.
    func compute(headingDegrees: Double?, myFix: LocationFix?, imperial: Bool, now: Date) -> RadarSnapshot
}

/// The honest mock: invents no crew, no positions, no signal readings.
/// Starts at `RadarSnapshot.empty` (NOSEL) and stays there until a test
/// or a preview explicitly scripts a snapshot via `nextSnapshot` — the
/// same "refuses to invent" discipline `StubMeshtasticClient` follows for
/// the mesh client seam. `RadarViewModelTests` drives this directly with
/// the exact field values transcribed from
/// `firmware/tests/fixtures/radar_*.json`.
///
/// M3 / Swift 6: `@MainActor`, matching `RadarComputing`'s own
/// isolation — plain stored properties, no `NSLock`. The lock this type
/// used to carry existed only to satisfy the protocol's old `Sendable`
/// requirement; every real caller (`RadarViewModelTests`, itself
/// `@MainActor`) was single-threaded already, so the lock was
/// serializing access that was never actually concurrent.
@MainActor
public final class MockRadarComputing: RadarComputing {
    public var nextSnapshot: RadarSnapshot = .empty
    public var hasPairedMembers = false
    public var selectedNodeID: UInt32?

    /// Recorded calls, for cadence/observability tests.
    public private(set) var computeCallCount = 0
    public private(set) var cycleSelectionCallCount = 0
    public private(set) var lastHeadingDegrees: Double??
    public private(set) var lastFix: LocationFix??
    public private(set) var lastImperial: Bool?

    public init() {}

    public func cycleSelection() {
        cycleSelectionCallCount += 1
    }

    public func compute(headingDegrees: Double?, myFix: LocationFix?, imperial: Bool, now: Date) -> RadarSnapshot {
        computeCallCount += 1
        lastHeadingDegrees = headingDegrees
        lastFix = myFix
        lastImperial = imperial
        return nextSnapshot
    }
}

// MARK: - FindPinging — the S29 PR2 FIND seam (slice B's FindBridge fills this in)

/// One PONG's worth of "how they hear us" — the FIND screen's replies
/// list entry.
public struct FindReply: Sendable, Equatable, Identifiable {
    public var id: Int
    public var rssiOfUs: Int
    public var hasSNR: Bool
    public var snrOfUs: Double
    public var tier: SignalTierPresentation
    public var ageText: String
    public var receivedAt: Date

    public init(id: Int, rssiOfUs: Int, hasSNR: Bool, snrOfUs: Double, tier: SignalTierPresentation,
                ageText: String, receivedAt: Date) {
        self.id = id
        self.rssiOfUs = rssiOfUs
        self.hasSNR = hasSNR
        self.snrOfUs = snrOfUs
        self.tier = tier
        self.ageText = ageText
        self.receivedAt = receivedAt
    }
}

public enum FindHaptic: Sendable, Equatable {
    case none, warmer, colder

    public init(ffHaptic: ff_find_haptic_t) {
        switch ffHaptic {
        case FF_FIND_HAPTIC_WARMER: self = .warmer
        case FF_FIND_HAPTIC_COLDER: self = .colder
        default: self = .none
        }
    }
}

/// The narrow protocol `RadarViewModel` depends on for FIND (S29 PR2).
/// Slice B's real `FindBridge` wraps `ff_find_t`
/// (`core/include/ff_find.h`) — single active session, a hard 10 s
/// send floor enforced INSIDE the session (not by the caller's tick
/// cadence), a 30-ping/5-minute cap. `MockFindSession` below
/// reimplements that same cadence/cap bookkeeping (session
/// rate-limiting, not bearing/distance math) as an honest stand-in.
///
/// M3 / Swift 6: `@MainActor`, not `Sendable` — see `RadarComputing`'s
/// own doc comment for the full reasoning; identical situation here
/// (`CoreFindSession`, `MockFindSession`, and every call site in
/// `RadarViewModel` already lived on the main actor).
@MainActor
public protocol FindPinging: AnyObject {
    var isActive: Bool { get }
    var targetNodeID: UInt32? { get }
    var pingCount: Int { get }

    /// Start (or restart) a session against `targetNodeID` — cancels any
    /// prior session outright (S29: single active target, no queuing).
    func start(targetNodeID: UInt32, now: Date)
    /// Cancel-on-face-leave / explicit stop.
    func stop()
    /// Periodic pump. The caller may tick as often as it likes — the 10 s
    /// floor (`FF_FIND_PING_INTERVAL_MS`) and the 30-ping/5-minute cap
    /// are enforced inside the session itself. Returns true iff a ping
    /// was actually sent this call.
    @discardableResult
    func tick(now: Date) -> Bool
    /// Feed a PONG's payload in — in production, decoded off the
    /// client's `incomingPrivate()` stream (portnum 269) by the
    /// composition root. Returns the warmer/colder haptic verdict for
    /// this update — `.none` on every call that isn't itself a fresh
    /// trend crossing.
    ///
    /// `nonce` is the PING nonce the PONG is answering. The real
    /// `ff_find_on_pong` DISCARDS a reply whose nonce isn't the most
    /// recently sent ping's, which is what keeps a late reply from a
    /// previous session out of this session's trend — so it is part of
    /// the seam, not an implementation detail of the bridge
    /// (`CoreFindSession`). `MockFindSession` models cadence and trend
    /// only and ignores it, which is why the mock is a stand-in and not
    /// a second implementation.
    func recordPong(fromNodeID: UInt32, nonce: UInt32, rssiDbm: Int16, hasSNR: Bool, snrDb: Double,
                    now: Date) -> FindHaptic
}

/// `ff_find.h`'s cadence/cap constants, transcribed — see that header's
/// own doc comment for the full derivation (both independently enforced
/// so an irregular tick loop can't dodge the wall-clock cap).
public enum FindSessionConstants {
    public static let pingIntervalSeconds: TimeInterval = 10
    public static let maxPings = 30
    public static let sessionMaxSeconds: TimeInterval = 5 * 60
    public static let trendSamples = 3
    public static let trendThresholdDbm: Double = 3.0
}

/// Honest stand-in for slice B's `FindBridge`. Reimplements `ff_find_t`'s
/// session bookkeeping (rate limiting, caps, trend-crossing detection) —
/// none of that is bearing/distance geometry, it is a state machine this
/// file is allowed to own until the real bridge replaces it; the PONG
/// payload itself is never invented, only fed in by a caller (tests, or
/// eventually the real client).
///
/// M3 / Swift 6: `@MainActor`, matching `FindPinging`'s own isolation —
/// same reasoning, and the same lock removal, as `MockRadarComputing`
/// above.
@MainActor
public final class MockFindSession: FindPinging {
    public private(set) var isActive = false
    public private(set) var targetNodeID: UInt32?
    private var startedAt: Date?
    public private(set) var pingCount = 0
    private var lastPingSentAt: Date?
    private var sampleHistory: [Double] = []
    private var lastFiredTrend = 0

    public init() {}

    public func start(targetNodeID: UInt32, now: Date) {
        isActive = true
        self.targetNodeID = targetNodeID
        startedAt = now
        pingCount = 0
        lastPingSentAt = nil
        sampleHistory.removeAll()
        lastFiredTrend = 0
    }

    public func stop() {
        isActive = false
        targetNodeID = nil
        startedAt = nil
        lastPingSentAt = nil
        sampleHistory.removeAll()
        lastFiredTrend = 0
    }

    @discardableResult
    public func tick(now: Date) -> Bool {
        guard isActive, let startedAt else { return false }
        if pingCount >= FindSessionConstants.maxPings
            || now.timeIntervalSince(startedAt) >= FindSessionConstants.sessionMaxSeconds {
            isActive = false
            return false
        }
        if let lastPingSentAt, now.timeIntervalSince(lastPingSentAt) < FindSessionConstants.pingIntervalSeconds {
            return false
        }
        lastPingSentAt = now
        pingCount += 1
        return true
    }

    /// `nonce` is accepted and ignored — see the protocol's own doc
    /// comment. Only `CoreFindSession`/`ff_find_on_pong` checks it.
    public func recordPong(fromNodeID: UInt32, nonce: UInt32, rssiDbm: Int16, hasSNR: Bool, snrDb: Double,
                           now: Date) -> FindHaptic {
        guard isActive, fromNodeID == targetNodeID else { return .none }
        sampleHistory.append(Double(rssiDbm))
        let window = 2 * FindSessionConstants.trendSamples
        if sampleHistory.count > window { sampleHistory.removeFirst(sampleHistory.count - window) }
        guard sampleHistory.count == window else { return .none }
        let newest = sampleHistory.suffix(FindSessionConstants.trendSamples)
        let oldest = sampleHistory.prefix(FindSessionConstants.trendSamples)
        let delta = (newest.reduce(0, +) / Double(newest.count)) - (oldest.reduce(0, +) / Double(oldest.count))
        let trend: Int
        if delta >= FindSessionConstants.trendThresholdDbm { trend = 1 }
        else if delta <= -FindSessionConstants.trendThresholdDbm { trend = -1 }
        else { trend = 0 }
        guard trend != 0, trend != lastFiredTrend else {
            if trend == 0 { lastFiredTrend = 0 }
            return .none
        }
        lastFiredTrend = trend
        return trend > 0 ? .warmer : .colder
    }
}

// MARK: - Crew ring colors (brand + colorblind-safe alternative)

/// The crew ring's two palettes. `FireflyTheme.crew` (already landed,
/// pinned against `ff_theme.h`) is the brand eight; the colorblind-safe
/// Okabe-Ito-derived eight is transcribed here rather than added to
/// `FireflyTheme.swift`, which this slice does not own — see
/// `ff_theme.h`'s own doc comment on `FF_THEME_CREW_CB_*` for the
/// citation and the one deliberate substitution (a pale mauve standing
/// in for the canonical set's black, invisible against this app's
/// near-black background).
public enum RadarCrewPalette {
    public static let colorblind: [UInt32] = [
        0xE69F00, // CB_ORANGE
        0x56B4E9, // CB_SKYBLUE
        0x009E73, // CB_GREEN ("bluish green")
        0xF0E442, // CB_YELLOW
        0x0072B2, // CB_BLUE
        0xD55E00, // CB_VERMILLION
        0xCC79A7, // CB_PURPLE ("reddish purple")
        0xD3B3DB, // CB_MAUVE — substitute for the canonical set's black
    ]

    /// 0xRRGGBB for a dot/member's `colorIndex`, wrapping — mirrors
    /// `ff_theme_crew_color`'s own modulo-wrap behavior exactly, so an
    /// out-of-range index degrades to a valid color instead of a crash.
    public static func hex(index: Int, colorblind: Bool) -> UInt32 {
        let palette = colorblind ? Self.colorblind : FireflyTheme.crew
        return palette[((index % palette.count) + palette.count) % palette.count]
    }
}

// MARK: - Haptics (S29: "haptics on iOS")

/// The real implementation (`UIImpactFeedbackGenerator`) lives in the app
/// target (`app/Firefly/Sources/Radar/HapticSignaling+UIKit.swift`) —
/// this protocol stays here so `RadarViewModel` (FireflyModel, no
/// UIKit/AppKit dependency per A01's "one target, no UI anywhere in
/// FireflyKit") can hold one without importing UIKit.
///
/// M3 / Swift 6: `@MainActor`, not `Sendable` — every real caller
/// (`RadarViewModel`, `FlareTakeoverViewModel`) is itself `@MainActor`,
/// and the real iOS conformance (`UIKitHapticSignaling`) wraps
/// `UIImpactFeedbackGenerator`/`UINotificationFeedbackGenerator`, which
/// the SDK itself now isolates to the main actor — its stored
/// properties could not otherwise be default-initialized at all.
@MainActor
public protocol HapticSignaling {
    func warmer()
    func colder()
    /// M2, S10: inbound FLARE's "haptic pattern (3 long) — overrides
    /// quiet hours". Defaulted (no-op) so every conformance written
    /// before this method existed — `NoHapticSignaling` below, any test
    /// double — keeps compiling without change; `UIKitHapticSignaling`
    /// (app target) is the one real override.
    func flareAlert()
}

public extension HapticSignaling {
    func flareAlert() {}
}

/// The macOS/unit-test default — there is no Taptic Engine on a Mac, and
/// a haptic that silently does nothing there is the honest answer, not a
/// gap to paper over.
public final class NoHapticSignaling: HapticSignaling, Sendable {
    public init() {}
    public func warmer() {}
    public func colder() {}
    public func flareAlert() {}
}

// MARK: - RadarViewModel

/// The Radar face's whole view model. Follows the MVVM conventions every
/// view model in this app follows (A01, "MVVM conventions"): protocol
/// existentials in `init`, no I/O of its own beyond consuming streams,
/// `observe()`/`stopObserving()`, and display strings computed HERE, not
/// in the view.
@MainActor
@Observable
public final class RadarViewModel {
    public private(set) var snapshot: RadarSnapshot = .empty
    /// Whether this build has a compass at all — `heading.headings()`
    /// yields `nil` forever on macOS (`NoHeadingProvider`); this is
    /// surfaced separately from a merely-invalid single reading so the
    /// view can explain WHY it's permanently NOHDG rather than reading
    /// as a transient glitch.
    public private(set) var lastHeading: HeadingReading?
    public private(set) var lastFix: LocationFix?
    public var imperial: Bool = false
    /// Crew-ring color palette selector (S17: colorblind-safe alternate
    /// 8-colour set). A Radar-local toggle in M1 — Settings (slice C)
    /// does not exist yet to drive this from a persisted value; wiring
    /// it to `SettingsStoring` once slice C lands is a follow-up, not a
    /// change to this property's meaning.
    public var colorblind: Bool = false

    // FIND
    public private(set) var findReplies: [FindReply] = []
    public private(set) var findHaptic: FindHaptic = .none
    private var findReplyCounter = 0

    private let radar: any RadarComputing
    private let heading: any HeadingProviding
    private let location: any LocationProviding
    private let find: any FindPinging
    private let haptics: any HapticSignaling
    private let clock: () -> Date

    private var headingObservation: Task<Void, Never>?
    private var locationObservation: Task<Void, Never>?
    private var recomputeLoop: Task<Void, Never>?
    private var findLoop: Task<Void, Never>?

    public init(radar: any RadarComputing, heading: any HeadingProviding, location: any LocationProviding,
                find: any FindPinging, haptics: any HapticSignaling = NoHapticSignaling(),
                clock: @escaping () -> Date = Date.init) {
        self.radar = radar
        self.heading = heading
        self.location = location
        self.find = find
        self.haptics = haptics
        self.clock = clock
    }

    /// The MOCK composition — previews and tests only, never the app.
    ///
    /// This used to be the M1 live composition, back when
    /// `MockRadarComputing`/`MockFindSession` stood in for slice B's
    /// bridge. The real composition is now
    /// `AppGraph.makeRadarViewModel(haptics:)`, over `CoreRadarComputing`
    /// (`ff_crew` + `ff_radar_compute`) and `CoreFindSession`
    /// (`ff_find` + a real portnum-269 send); the mocks stay because
    /// `RadarViewModelTests` drives them with exact values transcribed
    /// from `firmware/tests/fixtures/radar_*.json`, which is a thing no
    /// live bridge can be asked to do.
    ///
    /// Deliberately NOT named `live` any more: the name was the whole
    /// reason a screen could reach for it and believe it had a radio.
    public static func mocked(dependencies: AppDependencies,
                              haptics: any HapticSignaling = NoHapticSignaling()) -> RadarViewModel {
        RadarViewModel(radar: MockRadarComputing(), heading: dependencies.heading,
                        location: dependencies.location, find: MockFindSession(), haptics: haptics)
    }

    // MARK: Lifecycle

    /// Idempotent, like every other view model's `observe()`. Starts
    /// mirroring heading + location, and starts the periodic recompute
    /// pump (age text keeps advancing even when neither input changes).
    public func observe() {
        guard headingObservation == nil else { return }
        // Finding 3 (first real-radio session, macOS): nothing in this
        // app ever called `requestWhenInUseAuthorization()` before this
        // — Radar silently sat on "no fix" forever instead of asking.
        // Opening Radar is exactly the moment a real user expects that
        // system prompt, on macOS the same as iOS (`LocationProvider`
        // is the one CoreLocation-backed implementation for both — its
        // own doc comment). Only while genuinely undecided: a user who
        // already denied or granted it must never be re-prompted just
        // for opening this screen again.
        if location.authorization == .notDetermined {
            let location = self.location
            Task { await location.requestWhenInUseAuthorization() }
        }
        let headings = heading.headings()
        let fixes = location.fixes()

        headingObservation = Task { [weak self] in
            for await reading in headings {
                guard let self else { return }
                self.lastHeading = reading
                self.recompute()
            }
        }
        locationObservation = Task { [weak self] in
            for await fix in fixes {
                guard let self else { return }
                self.lastFix = fix
                self.recompute()
            }
        }
        recomputeLoop = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                self.recompute()
                try? await Task.sleep(nanoseconds: 1_000_000_000)
            }
        }
        recompute()
    }

    public func stopObserving() {
        headingObservation?.cancel(); headingObservation = nil
        locationObservation?.cancel(); locationObservation = nil
        recomputeLoop?.cancel(); recomputeLoop = nil
        stopFind()
    }

    private func recompute() {
        let headingDegrees: Double? = (lastHeading?.isValid == true) ? lastHeading?.headingDegrees : nil
        snapshot = radar.compute(headingDegrees: headingDegrees, myFix: lastFix, imperial: imperial, now: clock())
    }

    // MARK: Selection

    /// Tap-center gesture (S06).
    public func cycleSelection() {
        radar.cycleSelection()
        recompute()
    }

    // MARK: FIND (S29 PR2)

    public var isFindActive: Bool { find.isActive }
    public var findPingCount: Int { find.pingCount }
    /// nil when nobody is selected — the FIND affordance is disabled in
    /// that case, never silently pinging node 0.
    public var findTargetNodeID: UInt32? { radar.selectedNodeID }

    /// Starts a FIND session against the currently selected member, or
    /// does nothing if nobody is selected. Available whenever a member
    /// is selected, in ANY mode — S29: "FIND is available whenever a
    /// member is selected... a friend already LIVE can still be FIND'd."
    public func startFindOnSelection() {
        guard let target = radar.selectedNodeID else { return }
        startFind(targetNodeID: target)
    }

    /// Starts a FIND session against the currently selected member.
    public func startFind(targetNodeID: UInt32) {
        find.start(targetNodeID: targetNodeID, now: clock())
        findReplies.removeAll()
        findHaptic = .none
        guard findLoop == nil else { return }
        findLoop = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                _ = self.find.tick(now: self.clock())
                if !self.find.isActive {
                    self.findLoop?.cancel()
                    self.findLoop = nil
                    return
                }
                try? await Task.sleep(nanoseconds: 1_000_000_000)
            }
        }
    }

    /// Cancel-on-face-leave — call from `.onDisappear`, alongside
    /// `stopObserving()`.
    public func stopFind() {
        find.stop()
        findLoop?.cancel()
        findLoop = nil
    }

    /// Feed a PONG in — `AppGraph.observePrivatePackets()` calls this in
    /// production, off the client's `incomingPrivate()` stream (portnum
    /// 269) once `FireflyPacket.decode` has turned the frame into a
    /// `.pong(nonce:rssiDbm:snrDb:)`. Appends to the replies list and
    /// fires haptics on a warmer/colder crossing.
    public func handlePong(fromNodeID: UInt32, nonce: UInt32, rssiDbm: Int16, hasSNR: Bool, snrDb: Double) {
        let now = clock()
        let verdict = find.recordPong(fromNodeID: fromNodeID, nonce: nonce, rssiDbm: rssiDbm,
                                       hasSNR: hasSNR, snrDb: snrDb, now: now)
        findReplyCounter += 1
        findReplies.append(FindReply(
            id: findReplyCounter, rssiOfUs: Int(rssiDbm), hasSNR: hasSNR, snrOfUs: snrDb,
            tier: SignalTierPresentation.tier(rssiDbm: rssiDbm), ageText: "0 SEC", receivedAt: now))
        findHaptic = verdict
        switch verdict {
        case .warmer: haptics.warmer()
        case .colder: haptics.colder()
        case .none: break
        }
    }

    /// FIND's own headline: WARMER while the trend is positive, COLDER
    /// while negative, STEADY otherwise — mirrors CLOSE's identical
    /// three-way vocabulary (S29: "a trend chip identical in spirit to
    /// CLOSE's").
    public var findTrendHeadline: String {
        switch findHaptic {
        case .warmer: return "WARMER"
        case .colder: return "COLDER"
        case .none: return "STEADY"
        }
    }

    // MARK: - Display strings (computed HERE, not in the view — MVVM convention #5)

    /// Turns a puck-formatted age (`CrewStore.formatAge`, which wraps
    /// the firmware's own `ff_fmt_age` — under a minute reads "now",
    /// e.g. `ff_crew.c`) into the "X ago" phrase these display strings
    /// build sentences out of. "now ago" is not English (M1 review
    /// follow-up, #267: "their puck GPS, now ago" / "heard now ago" in
    /// the M1 screenshots) — the honest, natural reading of "now" glued
    /// to "ago" is "just now", so that substitution happens in exactly
    /// this one place rather than at each of this file's several call
    /// sites. Every other age ("12 MIN", "2 HR") already reads fine
    /// suffixed with " ago" and passes through unchanged.
    static func agoPhrase(for ageText: String) -> String {
        ageText == "now" ? "just now" : "\(ageText) ago"
    }

    /// The selection's chip text for whichever mode is active. Every
    /// string a UI is allowed to show, in one place, so it is testable.
    public var chipText: String {
        switch snapshot.mode {
        case .noSel: return "SELECT A FRIEND"
        case .noFix: return "NO FIX \u{00B7} RADIO ONLY"
        case .noHdg: return "NO COMPASS"
        case .live: return distanceAreaSuffix.isEmpty ? "LIVE" : "LIVE\(distanceAreaSuffix)"
        case .stale: return "LAST SEEN \(snapshot.ageText)\(distanceAreaSuffix)"
        case .lost:
            if !snapshot.ageText.isEmpty { return "LAST SEEN \(snapshot.ageText)\(distanceAreaSuffix)" }
            switch snapshot.heardPresence {
            case .heard, .stale: return "NEAR, NO FIX"
            case .lost, .never: return "NO FIX YET"
            }
        case .place: return distanceAreaSuffix.isEmpty ? "FIXED POSITION" : "FIXED POSITION\(distanceAreaSuffix)"
        case .close: return "CLOSE RANGE"
        case .signal:
            if snapshot.signalTier != .none { return "\(snapshot.signalTier.label) SIGNAL" }
            if snapshot.signalViaRelay { return "VIA RELAY" }
            // Should be unreachable per S29's mode-resolution rule (a
            // never-heard member always falls back to NOFIX/LOST), but
            // the renderer stays honest rather than assuming.
            return "RADIO SILENT"
        }
    }

    /// LIVE/STALE/PLACE/LOST's " - AREA" suffix (issue #47): an
    /// approximate-area distance must never look like a confident point
    /// reading. CLOSE has its own "NEARBY" substitution instead (above).
    private var distanceAreaSuffix: String {
        snapshot.distanceImprecise && snapshot.mode != .close ? " - AREA" : ""
    }

    /// The mode's subheadline / second line, or nil when there is
    /// nothing honest to add beyond the chip.
    public var subheadline: String? {
        switch snapshot.mode {
        case .noFix: return "Looking for \(snapshot.name)"
        case .lost where snapshot.ageText.isEmpty:
            switch snapshot.heardPresence {
            case .heard, .stale: return "Heard recently, no GPS fix yet"
            case .lost, .never: return "Waiting for their first GPS fix"
            }
        case .signal:
            var line = "heard \(Self.agoPhrase(for: snapshot.signalAgeText))"
            if snapshot.arrowValid {
                // The ghost sub-case (S29): a real, if very old, fix
                // exists for this member alongside the live signal
                // reading — "last known" is a DIFFERENT fact from
                // "heard", and both are shown.
                let compass = snapshot.bearingValid ? " \(CompassPoint.name(forBearingDegrees: snapshot.bearingDegrees))" : ""
                line += "\nLAST KNOWN \(Self.agoPhrase(for: snapshot.ageText)), \(displayDistanceText)\(compass)"
            }
            return line
        default: return nil
        }
    }

    /// The primary distance readout. LOST/SIGNAL-ghost's "don't fully
    /// trust this" framing gets a `~` prefix when the core hasn't
    /// already supplied one — a typographic re-emphasis of a fact the
    /// mode already carries (`mode == .lost`, or SIGNAL's ghost
    /// sub-case, `arrowValid`), never a new number.
    public var displayDistanceText: String {
        let isGhost = snapshot.mode == .lost || (snapshot.mode == .signal && snapshot.arrowValid)
        guard isGhost, !snapshot.distanceText.isEmpty, !snapshot.distanceText.hasPrefix("~") else {
            return snapshot.distanceText
        }
        return "~\(snapshot.distanceText)"
    }

    /// The big central readout the ring's text stack shows below the
    /// chip — "" for modes with nothing geometric to show at all
    /// (NOSEL/NOFIX; SIGNAL's own non-ghost sub-case, which has no
    /// distance by definition). CLOSE substitutes "NEARBY" for a
    /// degraded-precision fix rather than showing a fabricated big
    /// number (S29: "shows 'NEARBY' instead of a fabricated big
    /// number").
    public var primaryReadoutText: String {
        switch snapshot.mode {
        case .noSel, .noFix:
            return ""
        case .signal:
            return snapshot.arrowValid ? displayDistanceText : ""
        case .close:
            return snapshot.distanceImprecise ? "NEARBY" : snapshot.distanceText
        default:
            return displayDistanceText
        }
    }

    /// The trend chip's word — CLOSE's hot/cold and SIGNAL's WARMER/
    /// COLDER/STEADY share this vocabulary and this field.
    public var trendLabel: String {
        switch snapshot.trend {
        case ..<0: return "COLDER"
        case 0: return "STEADY"
        default: return "WARMER"
        }
    }

    /// True only where the spec says a trend chip may be shown at all —
    /// SIGNAL only draws it when there is a real tier to refine ("a trend
    /// on top of 'no direct reading' would be a fabricated refinement of
    /// nothing").
    public var showsTrendChip: Bool {
        switch snapshot.mode {
        case .close: return true
        case .signal: return snapshot.signalTier != .none
        default: return false
        }
    }

    /// NOHDG's bearing hint: "BEARING 180° · S".
    public var bearingHintText: String? {
        guard snapshot.mode == .noHdg, snapshot.bearingValid else { return nil }
        let point = CompassPoint.name(forBearingDegrees: snapshot.bearingDegrees)
        return "BEARING \(Int(snapshot.bearingDegrees.rounded()))\u{00B0} \u{00B7} \(point)"
    }

    /// "The ring is signal order, not direction" — S29's own explicit
    /// warning that the inner signal ring is laid out by tier, over a
    /// fixed arc, and carries no bearing meaning whatsoever.
    public static let signalRingDisclaimer = "Ring is signal order, not direction"

    /// Every position line names its source and its age (A01 slice D
    /// acceptance criterion). The SELECTED member's line — nil when
    /// there is nothing honestly knowable to say about them yet.
    public var theirPositionLine: String? {
        let who = snapshot.name.isEmpty ? "they" : snapshot.name
        switch snapshot.mode {
        case .noSel:
            return nil
        case .noFix:
            // MY position is unknown, so distance/bearing are not
            // honestly computable — but a last-known age for THEIR fix
            // may still be, and dropping it entirely would under-report
            // what's actually known (S06: RADAR_NOFIX still carries a
            // true age_str for the selection's last fix).
            guard !snapshot.ageText.isEmpty else { return nil }
            return "\(who)'s last known position: their puck GPS, \(Self.agoPhrase(for: snapshot.ageText)) "
                + "(your distance unknown — no fix of your own)"
        case .place:
            return "\(who)'s position: fixed position, asserted (no age given)"
        case .noHdg, .live, .stale:
            guard !snapshot.ageText.isEmpty else { return nil }
            return "\(who)'s position: their puck GPS, \(Self.agoPhrase(for: snapshot.ageText))"
        case .lost:
            guard !snapshot.ageText.isEmpty else { return nil }
            return "\(who)'s position: their puck GPS, last seen \(Self.agoPhrase(for: snapshot.ageText))"
        case .close:
            guard !snapshot.ageText.isEmpty else { return nil }
            return "\(who)'s position: their puck GPS, \(Self.agoPhrase(for: snapshot.ageText))"
        case .signal:
            guard snapshot.arrowValid, snapshot.bearingValid else { return nil }
            let compass = CompassPoint.name(forBearingDegrees: snapshot.bearingDegrees)
            return "\(who)'s last known position: their puck GPS, \(Self.agoPhrase(for: snapshot.ageText)), \(compass)"
        }
    }

    /// The RADIO evidence line (S29) — distinct from `theirPositionLine`:
    /// how we're hearing them right now, never dressed up as distance.
    public var theirSignalLine: String? {
        guard snapshot.signalHeard else { return nil }
        let who = snapshot.name.isEmpty ? "they" : snapshot.name
        let path = snapshot.signalViaRelay ? "via relay" : "direct"
        let tierPart = snapshot.signalTier != .none ? "\(snapshot.signalTier.label.lowercased()) signal, " : ""
        return "\(who)'s radio: \(tierPart)\(path), heard \(Self.agoPhrase(for: snapshot.signalAgeText))"
    }

    /// The phone's OWN line: "your position: phone GPS ±4 m; heading:
    /// phone compass" — always namable, never blank, even when both
    /// halves are "unavailable".
    public var myPositionLine: String {
        let posText: String
        if let fix = lastFix {
            if let accuracy = fix.horizontalAccuracyMeters {
                posText = "your position: phone GPS \u{00B1}\(Int(accuracy.rounded())) m"
            } else {
                posText = "your position: phone GPS"
            }
        } else {
            // Finding 3: WHY, not a bare "no fix" — the exact reported
            // bug ("your position: no fix; heading: unavailable" on
            // macOS, with nothing ever having asked for permission).
            posText = "your position: \(location.authorization.noFixReasonText)"
        }
        let headingText: String
        if let lastHeading, lastHeading.isValid {
            headingText = "heading: phone compass"
        } else {
            headingText = "heading: unavailable"
        }
        return "\(posText); \(headingText)"
    }
}
