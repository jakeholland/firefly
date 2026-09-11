//
//  FindBridge.swift — the Swift-safe wrapper over
//  `firmware/core/ff_find` (docs/specs/A01-companion-app.md, slice B).
//
//  Heap-owns one `ff_find_t` — the FIND session's state, which must
//  persist across many `tick()` calls over the session's lifetime (the
//  10 s ping cadence, the 30-ping/5-minute cap). One `FindBridge` per
//  FIND session — the app creates a fresh one (or calls `stop()`/
//  `start()` again) each time the user starts a new FIND from the
//  Radar/Signal face.
//
import FireflyCore
import Foundation

/// Heap-owns one `ff_find_t`.
public final class FindBridge {
    private let context: UnsafeMutablePointer<ff_find_t>

    public init() {
        context = UnsafeMutablePointer<ff_find_t>.allocate(capacity: 1)
        context.initialize(to: ff_find_t())
        ff_find_stop(context) // safe on a zeroed struct; makes "no session" explicit
    }

    deinit {
        context.deinitialize(count: 1)
        context.deallocate()
    }

    public var isActive: Bool { context.pointee.active }
    public var targetNodeID: UInt32? { isActive ? context.pointee.target_node_id : nil }
    public var pingCount: UInt32 { context.pointee.ping_count }

    /// Cancels any prior session outright — single active target, no
    /// queuing.
    public func start(targetNodeID: UInt32, now: UInt32) {
        ff_find_start(context, targetNodeID, now)
    }

    public func stop() { ff_find_stop(context) }

    /// S29: "FIND stops on leaving the Radar face" — same effect as
    /// `stop()`, named for the call site's own intent.
    public func leaveFace() { ff_find_leave_face(context) }

    public enum Intent: Sendable, Equatable {
        case none
        case sendPing(nonce: UInt32)
    }

    /// Periodic pump. No-op (`.none`) if not active; auto-stops the
    /// session once its own cap is reached (see ff_find.h's doc
    /// comment); otherwise returns `.sendPing` at most once per 10 s,
    /// a hard floor enforced inside the core itself.
    public func tick(now: UInt32) -> Intent {
        let r = ff_find_tick(context, now)
        return r.intent == FF_FIND_INTENT_SEND_PING ? .sendPing(nonce: r.nonce) : .none
    }

    public enum Haptic: Sendable, Equatable {
        case none, warmer, colder
    }

    /// Records a PONG's payload as our fresh "how do they hear us"
    /// reading, and evaluates the trend-haptic crossing. A no-op unless
    /// the session is active, `fromNodeID` is the current target, and
    /// `nonce` matches the most recently sent ping's nonce.
    @discardableResult
    public func onPong(fromNodeID: UInt32, nonce: UInt32, rssiDbm: Int16, snrDb: Float?, now: UInt32) -> Haptic {
        let h = ff_find_on_pong(context, fromNodeID, nonce, rssiDbm, snrDb != nil, snrDb ?? 0, now)
        switch h {
        case FF_FIND_HAPTIC_WARMER: return .warmer
        case FF_FIND_HAPTIC_COLDER: return .colder
        default: return .none
        }
    }

    public struct TheirReading: Sendable, Equatable {
        public let rssiDbm: Int16
        public let snrDb: Float?
        public let ageMs: UInt32
    }

    /// "They hear us at -xx dBm" — the one new fact FIND adds, absent
    /// until the first PONG of this session arrives.
    public var theirReading: TheirReading? {
        let f = context.pointee
        guard f.has_their_reading else { return nil }
        return TheirReading(rssiDbm: f.their_rssi_of_us,
                             snrDb: f.their_has_snr ? f.their_snr_of_us : nil,
                             ageMs: f.their_reading_age_ms)
    }
}
