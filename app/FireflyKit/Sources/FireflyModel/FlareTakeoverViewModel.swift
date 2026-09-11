//
//  FlareTakeoverViewModel.swift — M2: the inbound-FLARE takeover state
//  (docs/specs/S10-flare.md's "Receive" behavior, ported to the app).
//
//  The puck's own `ff_flare.h` state machine (SENDING/RECEIVED/LOCKED,
//  the multi-flare "newest wins" rule, the GO-locks-selection wiring) is
//  compiled into `firmware/core` but deliberately NOT bound in this app
//  (A01's "Compiled but not bound in M1" table lists `ff_flare`) — the
//  puck's LOCKED navigation-lock concept has no Radar-selection analogue
//  here yet. What this type DOES carry over faithfully from S10 is the
//  receiver-side shape that has nothing to do with that lock: a
//  full-screen takeover, sender identity + colour, an honest bearing/
//  distance when both positions are known (never fabricated otherwise),
//  auto-dismiss at `dur_s`, and manual dismiss.
//
//  "Newest wins the takeover" (S10, `## Behavior`): `show(...)` always
//  overwrites whatever was showing, exactly like the puck's takeover.
//  A `FLARE_END` only clears the takeover if it names the SAME sender
//  currently showing — a stale end for a flare that has already been
//  superseded must not clear the newer one (`end(from:)`'s own doc
//  comment).
//
//  Timing is TICK-DRIVEN, not a `Task.sleep` timer — the same
//  "tick-returns-an-action" shape `ff_flare_tick`/`ff_find_tick` use,
//  chosen here for the same reason: a caller-supplied clock makes
//  auto-dismiss exactly and cheaply testable (`FlareTakeoverViewModelTests`),
//  with no real multi-minute sleep anywhere in the test suite.
//
import FireflyCore
import Foundation
import Observation

@MainActor
@Observable
public final class FlareTakeoverViewModel {
    public private(set) var isActive = false
    public private(set) var senderNodeID: UInt32?
    public private(set) var senderName = ""
    public private(set) var senderColorIndex = 0
    /// nil = no bearing — S10's own vocabulary ("bearing when known,
    /// otherwise honest 'no bearing'"): true only when we have BOTH our
    /// own current fix AND the sender's last known crew position.
    public private(set) var bearingDegrees: Double?
    public private(set) var compassPoint: String?
    public private(set) var distanceText: String?
    /// Set only when the bearing/distance are missing BECAUSE our own
    /// fix has gone stale (`LocationFix.isStale`, PR #271 review,
    /// SHOULD-FIX 3) — nil for the other, pre-existing "no bearing"
    /// cause (either fix simply missing). Lets the view render a more
    /// specific, still-honest reason instead of the generic fallback.
    public private(set) var noBearingReason: String?
    public private(set) var totalDurationSeconds: Int = 0

    private let crew: CrewStore
    private var currentFix: () -> LocationFix?
    private var haptics: any HapticSignaling
    private let clock: () -> Date
    private var expiresAt: Date?

    public init(crew: CrewStore, currentFix: @escaping () -> LocationFix? = { nil },
                haptics: any HapticSignaling = NoHapticSignaling(), clock: @escaping () -> Date = Date.init) {
        self.crew = crew
        self.currentFix = currentFix
        self.haptics = haptics
        self.clock = clock
    }

    /// Set once the real haptic backend exists (`UIKitHapticSignaling`,
    /// app target) — mirrors `AppGraph.makeRadarViewModel(haptics:)`'s
    /// own late-injection shape rather than widening this type's `init`
    /// signature a second time.
    public func setHaptics(_ haptics: any HapticSignaling) { self.haptics = haptics }

    /// `AppGraph` can only capture `[weak self]` once every one of its
    /// OWN stored properties is set — i.e. after `init` finishes, not
    /// during it (`flareTakeover` itself is constructed mid-`init`, over
    /// `self.core.crew`). This late-injection setter, called once at the
    /// end of `AppGraph.init`, is how the phone's live fix reaches here
    /// without capturing a not-yet-fully-initialized `self`.
    public func setCurrentFix(_ currentFix: @escaping () -> LocationFix?) { self.currentFix = currentFix }

    /// Remaining whole seconds until auto-dismiss, honestly clamped to
    /// zero rather than going negative.
    public func remainingSeconds(now: Date? = nil) -> Int {
        guard isActive, let expiresAt else { return 0 }
        return max(0, Int((expiresAt.timeIntervalSince(now ?? clock())).rounded(.up)))
    }

    /// Inbound FLARE (S04 type 0x02, `[dur_s:2]`). "Newest wins the
    /// takeover" — always overwrites whatever was showing.
    public func show(senderNodeID: UInt32, durationSeconds: UInt16) {
        let now = clock()
        let nowMs = FireflyClock.millis(since: now)
        let member = crew.member(nodeID: senderNodeID, now: nowMs)
        self.senderNodeID = senderNodeID
        let name = member?.displayName ?? ""
        self.senderName = name.isEmpty ? "Someone" : name
        self.senderColorIndex = Int(member?.colorIndex ?? 0)

        if let fix = currentFix(), let pos = member?.position, !fix.isStale(now: now) {
            let from = ff_latlon_t(lat: fix.latitude, lon: fix.longitude)
            let to = ff_latlon_t(lat: pos.latitude, lon: pos.longitude)
            let bearing = Double(ff_geo_bearing_deg(from, to))
            bearingDegrees = bearing
            compassPoint = CompassPoint.name(forBearingDegrees: bearing)
            distanceText = FlareTakeoverViewModel.formatDistance(Double(ff_geo_distance_m(from, to)))
            noBearingReason = nil
        } else {
            // Honest "no bearing": either OUR fix or THEIR last known
            // position is missing — never a fabricated arrow. Same
            // "no ghost" rule S29's RADAR_SIGNAL/`arrow_valid` follows.
            bearingDegrees = nil
            compassPoint = nil
            distanceText = nil
            // A stale fix of our own gets its own, more specific reason
            // (PR #271 review, SHOULD-FIX 3) — a GPS reading that's gone
            // several minutes stale is not honest grounds for a
            // confident-looking bearing either, even when the sender's
            // position IS known.
            if let fix = currentFix(), fix.isStale(now: now) {
                noBearingReason = "your fix is \(fix.ageMinutesText(now: now)) min old"
            } else {
                noBearingReason = nil
            }
        }

        let duration = max(1, Int(durationSeconds))
        totalDurationSeconds = duration
        expiresAt = now.addingTimeInterval(TimeInterval(duration))
        isActive = true
        haptics.flareAlert()
    }

    /// FLARE_END — only clears the takeover if it names the sender
    /// CURRENTLY showing. A newer flare (from anyone) already overwrote
    /// this state in `show(...)`, so a stale END for a superseded
    /// sender must not reach in and clear the newer one.
    public func end(from nodeID: UInt32) {
        guard isActive, senderNodeID == nodeID else { return }
        dismiss()
    }

    /// The on-screen DISMISS button (S10: "DISMISS -> back, feed item
    /// remains" — the feed item itself is pushed by the caller, not
    /// this view model, which owns only the transient overlay).
    public func dismiss() {
        isActive = false
        expiresAt = nil
    }

    /// Periodic pump — `AppGraph`'s existing 1 Hz tick loop calls this
    /// alongside `CoreStore.tick(nowMs:)`. Auto-dismisses once `now`
    /// reaches the flare's own expiry.
    public func tick(now: Date? = nil) {
        guard isActive, let expiresAt else { return }
        if (now ?? clock()) >= expiresAt { dismiss() }
    }

    static func formatDistance(_ meters: Double) -> String {
        meters >= 1000 ? String(format: "%.1f km", meters / 1000) : String(format: "%.0f m", meters)
    }
}
