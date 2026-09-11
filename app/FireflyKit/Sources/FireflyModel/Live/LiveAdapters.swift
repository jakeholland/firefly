//
//  LiveAdapters.swift — the thin conformances that let the REAL client
//  (slice A) and the REAL C-core bridges (slice B) fill the narrow
//  seams the other slices defined against stand-ins, with no slice
//  having to depend on another's concrete types.
//
//  Each type here is an adapter and nothing more: no policy, no
//  fabrication, no second source of truth. Where a seam and the real
//  thing disagree about vocabulary — `LocationFix` vs.
//  `ExternalPositionFix`, `RadarView` vs. `RadarSnapshot` — the
//  translation happens HERE, in one place, and the disagreement is
//  documented rather than papered over.
//
//  Threading: every adapter that touches a `ff_*` context is confined to
//  `@MainActor`, per the threading model ("every `ff_*` context in the
//  app lives behind ONE isolation domain"). Two of the seams they
//  conform to (`RadarComputing`, `FindPinging`) are THEMSELVES
//  `@MainActor`-isolated protocols (M3 / Swift 6 — see `RadarViewModel
//  .swift`'s doc comment on `RadarComputing` for the full reasoning),
//  so `CoreRadarComputing`/`CoreFindSession` below are plain `@MainActor`
//  classes with no `nonisolated`/`MainActor.assumeIsolated`/`@unchecked
//  Sendable` of their own — the compiler enforces the confinement this
//  file used to assert by hand.
//
import FireflyCore
import FireflyMesh
import Foundation

// MARK: - Phone GPS -> node

/// `PositionPushSending` over the real client — the last mile of A01's
/// "Phone GPS -> node". `PhoneGPSUplink` owns the cadence and the
/// on/off setting; this owns nothing but the translation from the
/// CoreLocation-shaped `LocationFix` into the wire-shaped
/// `ExternalPositionFix`, and the client owns the protobuf.
///
/// That three-way split is deliberate (`PositionPushSending`'s own doc
/// comment): it is what keeps the cadence policy testable with no
/// client, no radio and no protobuf import at all.
public final class MeshPositionSink: PositionPushSending {
    private let client: any MeshtasticClientProtocol

    public init(client: any MeshtasticClientProtocol) {
        self.client = client
    }

    public func sendPosition(_ fix: LocationFix, to destination: UInt32) async throws {
        // `horizontalAccuracyMeters` has no home on the wire: Meshtastic's
        // `Position` has no horizontal-accuracy field, and
        // `precision_bits` means something else entirely (the CHANNEL's
        // truncation grid, issue #47). Dropped rather than mapped onto a
        // field that would misreport it.
        try await client.sendPosition(
            ExternalPositionFix(
                latitude: fix.latitude,
                longitude: fix.longitude,
                altitudeMeters: fix.altitude,
                time: fix.time,
                groundSpeedMetersPerSecond: fix.groundSpeedMetersPerSecond,
                groundTrackDegrees: fix.groundTrackDegrees),
            to: destination)
    }
}

// MARK: - Firefly's own portnum (269)

/// The real `FireflyPacketSending` conformance slice E's FLARE control
/// has been waiting on ("wiring a real conformance onto a portnum-269
/// send is slice A's client — neither has landed in this worktree").
/// `FireflyPacket.encode()` (ff_proto, slice B) makes the bytes; the
/// client (slice A) puts them on portnum 269. Nothing here falls back to
/// `sendText`: a FLARE transmitted as plain text is worse than failing
/// honestly, and this type cannot do it even by accident — it has no
/// reference to a text send at all.
public final class MeshFireflyPacketSender: FireflyPacketSending {
    private let client: any MeshtasticClientProtocol

    public init(client: any MeshtasticClientProtocol) {
        self.client = client
    }

    /// A body `ff_proto` refuses to encode (an over-long string, a case
    /// with no encoder) is a real failure, surfaced as a thrown error
    /// the caller renders as `ImmediateSendFailure.transportError` —
    /// never a silent no-op that looks like a successful send.
    public enum SendFailure: Error, Equatable, Sendable {
        case encodingFailed(FireflyPacket)
    }

    /// S04: FLARE is type `0x02`, body `[dur_s:2]`, `want_ack = true`.
    /// `to: nil` is a whole-crew broadcast — and a broadcast is never
    /// `want_ack`'d (nothing acks one), a rule the client re-applies for
    /// itself rather than trusting this call site.
    public func sendFlare(to: NodeID?, durationSeconds: UInt16) async throws {
        try await send(.flare(durationS: durationSeconds), to: to ?? meshBroadcastAddress, wantAck: true)
    }

    /// S04: RALLY is type `0x04`. `want_ack = false` — "RALLY/STATUS
    /// broadcast likewise" (S04's Addressing table; FLARE is the one
    /// `want_ack` type).
    public func sendRally(to: NodeID?, latitude: Double, longitude: Double, name: String) async throws {
        try await send(.rally(latitude: latitude, longitude: longitude, name: name),
                        to: to ?? meshBroadcastAddress, wantAck: false)
    }

    /// S04: STATUS is type `0x06`. Same `want_ack = false` rule as RALLY.
    public func sendStatus(to: NodeID?, text: String) async throws {
        try await send(.status(text), to: to ?? meshBroadcastAddress, wantAck: false)
    }

    /// The general entry point — FIND's PING goes through this too.
    @discardableResult
    public func send(_ packet: FireflyPacket, to destination: UInt32, wantAck: Bool) async throws -> UInt32 {
        guard let payload = packet.encode() else { throw SendFailure.encodingFailed(packet) }
        return try await client.sendPrivate(payload, to: destination, wantAck: wantAck)
    }
}

// MARK: - Radar

/// `RadarComputing` over the real bridge: `CrewStore` (`ff_crew_t`) is
/// the roster, `RadarBridge` (`ff_radar_compute` + `ff_radar_smooth_t`)
/// is the geometry, and this type is the wire between them and
/// `RadarViewModel`'s `RadarSnapshot` vocabulary.
///
/// It computes NOTHING. Every field of the returned snapshot is either
/// copied straight out of `ff_radar_view_t` or is one of the three
/// fields `ff_radar_compute` documents as "NOT written by
/// ff_radar_compute" (`clockText`, `battPct`, `meshOK`), which the
/// caller is supposed to fill from the clock/battery/mesh-link
/// subsystems — and which this type fills honestly: a real clock
/// reading, a `nil` battery (the phone's own battery is not the crew's,
/// and the connected NODE's battery does not reach this app in M1), and
/// a mesh-OK flag read from the actual link state.
@MainActor
public final class CoreRadarComputing: RadarComputing {
    private let crew: CrewStore
    private let radar: RadarBridge
    /// Read at compute time, not cached: the link can drop between two
    /// frames and the chrome must say so on the next one.
    private let linkIsReady: @MainActor () -> Bool
    private var lastSelectedNodeID: UInt32?

    public init(crew: CrewStore, radar: RadarBridge, linkIsReady: @escaping @MainActor () -> Bool) {
        self.crew = crew
        self.radar = radar
        self.linkIsReady = linkIsReady
    }

    public var hasPairedMembers: Bool {
        crew.members(now: FireflyClock.nowMillis()).contains { $0.paired }
    }

    public var selectedNodeID: UInt32? {
        crew.selected(now: FireflyClock.nowMillis())?.nodeID
    }

    public func cycleSelection() {
        crew.selectNext()
        // A new selection means the smoothing filter's history belongs
        // to a different friend: keeping it would sweep the arrow from
        // the OLD member's bearing to the new one, an animation that
        // asserts a relationship between two unrelated readings
        // (`ff_radar_smooth_reset`'s own "snap rather than sweep" case).
        if crew.selected(now: FireflyClock.nowMillis())?.nodeID != lastSelectedNodeID {
            radar.resetSmoothing()
        }
        lastSelectedNodeID = crew.selected(now: FireflyClock.nowMillis())?.nodeID
    }

    public func compute(headingDegrees: Double?, myFix: LocationFix?, imperial: Bool,
                        now: Date) -> RadarSnapshot {
        let view = radar.compute(
            crew: crew,
            headingDeg: headingDegrees.map(Float.init),
            myPosition: myFix.map { (latitude: $0.latitude, longitude: $0.longitude) },
            imperial: imperial,
            now: FireflyClock.millis(since: now))
        return CoreRadarComputing.snapshot(from: view, now: now, meshOK: linkIsReady())
    }

    /// `ff_radar_view_t` (via `RadarBridge.RadarView`) -> `RadarSnapshot`,
    /// field for field. The two types exist separately only because
    /// slices B and D landed in parallel worktrees; nothing is added or
    /// dropped in between.
    static func snapshot(from view: RadarView, now: Date, meshOK: Bool) -> RadarSnapshot {
        RadarSnapshot(
            mode: view.mode,
            arrowDegrees: Double(view.arrowDeg),
            arrowValid: view.arrowValid,
            name: view.name,
            distanceText: view.distanceText,
            distanceImprecise: view.distanceImprecise,
            ageText: view.ageText,
            trend: trendValue(view.trend),
            bearingDegrees: Double(view.bearingDeg),
            bearingValid: view.bearingValid,
            place: view.place,
            stale: view.stale,
            heardPresence: view.heardPresence,
            dots: view.dots.enumerated().map { index, dot in
                RadarSnapshotDot(
                    id: index,
                    ringDegrees: Double(dot.ringDeg),
                    // A dot whose member has no name yet renders a BLANK
                    // letter, never a '?' — `ff_crew_member_t.initial`'s
                    // own "'\\0' until known" rule, carried through
                    // rather than substituted for.
                    initial: dot.initial ?? " ",
                    colorIndex: Int(dot.colorIndex),
                    stale: dot.stale,
                    place: dot.place,
                    imprecise: dot.imprecise)
            },
            signalTier: view.signalTier,
            signalHeard: view.signalHeard,
            signalViaRelay: view.signalViaRelay,
            signalAgeText: view.signalAgeText,
            signalDots: view.signalDots.enumerated().map { index, dot in
                RadarSnapshotSignalDot(
                    id: index,
                    initial: dot.initial ?? " ",
                    colorIndex: Int(dot.colorIndex),
                    tier: dot.tier,
                    viaRelay: dot.viaRelay)
            },
            clockText: clockText(now),
            // The phone's own battery is not what this chrome means (the
            // puck's face shows the PUCK's), and the connected node's
            // battery does not reach this app in M1 — telemetry has no
            // consumer through `MeshtasticClientProtocol` yet. nil is the
            // honest answer; a phone percentage here would be a lie about
            // whose battery it is.
            battPct: nil,
            meshOK: meshOK)
    }

    private static func trendValue(_ trend: RSSITrend) -> Int {
        switch trend {
        case .rising: return 1
        case .falling: return -1
        case .flat: return 0
        }
    }

    private static let clockFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "HH:mm"
        return formatter
    }()

    static func clockText(_ now: Date) -> String { clockFormatter.string(from: now) }
}

// MARK: - FIND

/// `FindPinging` over the real `FindBridge` (`ff_find_t`) plus a real
/// portnum-269 send. `MockFindSession` reimplemented `ff_find`'s cadence
/// and cap bookkeeping in Swift as an honest stand-in; this replaces it
/// with the C core itself, so the phone and the puck enforce the 10 s
/// floor and the 30-ping/5-minute cap with the same object code.
///
/// `tick(now:)` is where the two halves meet: the bridge decides whether
/// a ping is due AND mints the nonce (`ff_find_tick` -> `.sendPing`),
/// this type puts that nonce on the wire as `FireflyPacket.ping`. The
/// send is fire-and-forget — a FIND ping never enters the outbox, the
/// same rule FLARE follows.
@MainActor
public final class CoreFindSession: FindPinging {
    private let find: FindBridge
    private let sender: MeshFireflyPacketSender
    /// Set by `start`, cleared by `stop` — the wire address every ping
    /// of this session goes to. `ff_find_t` holds the same value; this
    /// copy exists so a send never has to reach back into the C context
    /// from inside a `Task`.
    private var target: UInt32?

    public init(find: FindBridge, sender: MeshFireflyPacketSender) {
        self.find = find
        self.sender = sender
    }

    public var isActive: Bool { find.isActive }
    public var targetNodeID: UInt32? { find.targetNodeID }
    public var pingCount: Int { Int(find.pingCount) }

    public func start(targetNodeID: UInt32, now: Date) {
        find.start(targetNodeID: targetNodeID, now: FireflyClock.millis(since: now))
        target = targetNodeID
    }

    public func stop() {
        find.stop()
        target = nil
    }

    /// Returns true iff the core said a ping was due — i.e. iff one was
    /// actually put on the wire. The send itself is detached (this
    /// requirement is synchronous), so "sent" here means "handed to the
    /// client", which is the same thing `sendText`'s own SENT means.
    @discardableResult
    public func tick(now: Date) -> Bool {
        guard case .sendPing(let nonce) = find.tick(now: FireflyClock.millis(since: now)),
              let target else { return false }
        let sender = sender
        // PR #265 review, should-fix: a FIND ping's `try?` above
        // swallowed both `SendFailure.encodingFailed` (ff_proto
        // refused the body) and a transport-level throw with no
        // trace anywhere — a session sitting on "pinging..." with
        // no pong forever looked identical to a working session
        // whose replies just hadn't arrived yet. Diagnostic-only
        // (same `FileHandle.standardError.write` pattern
        // `BLETransport.log` uses, for the same reason: stdout is
        // fully block-buffered once `xcodebuild test` pipes it, so
        // a `print()` here could sit invisible for the whole run);
        // never surfaced to the UI — FIND has no per-ping failure
        // affordance, only the session-level trend/haptic path.
        Task {
            do {
                try await sender.send(.ping(nonce: nonce), to: target, wantAck: false)
            } catch {
                let line = "[CoreFindSession] FIND ping nonce=\(nonce) target=\(target) failed: \(error)\n"
                FileHandle.standardError.write(Data(line.utf8))
            }
        }
        return true
    }

    /// The nonce is load-bearing here in a way `MockFindSession`'s
    /// stand-in never modelled: `ff_find_on_pong` ignores a PONG unless
    /// it matches the most recently sent ping's nonce, which is what
    /// keeps a stale reply from a previous session out of the trend
    /// calculation.
    public func recordPong(fromNodeID: UInt32, nonce: UInt32, rssiDbm: Int16, hasSNR: Bool,
                           snrDb: Double, now: Date) -> FindHaptic {
        let haptic = find.onPong(fromNodeID: fromNodeID, nonce: nonce, rssiDbm: rssiDbm,
                                 snrDb: hasSNR ? Float(snrDb) : nil,
                                 now: FireflyClock.millis(since: now))
        switch haptic {
        case .warmer: return .warmer
        case .colder: return .colder
        case .none: return .none
        }
    }
}
