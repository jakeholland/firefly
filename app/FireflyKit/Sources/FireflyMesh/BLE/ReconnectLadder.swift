//
//  ReconnectLadder.swift — A03 §3.6, the bounded rediscovery ladder.
//
//  This is the correction A03 §1.7 forces: `Timer`,
//  `DispatchQueue.asyncAfter` and `Task.sleep` DO NOT FIRE while the
//  process is suspended, so `BLETransport.armReconnectFallback`'s old
//  sleeping task fired 20 seconds after the app was next WOKEN, not 20
//  seconds after the loss. Everything below is therefore a clock delta
//  evaluated at each wake — `evaluate(now:...)`, called from whichever
//  CoreBluetooth callback the OS actually gives us — never a sleeping
//  task. A `Task.sleep` survives in `BLETransport` only as an
//  opportunistic nudge for the case where the app happens to still be
//  running; correctness never depends on it.
//
//  It is a pure value type with no CoreBluetooth in it at all, for the
//  same reason `BLETransport.shouldIssueConnect(for:pending:)` already
//  is: constructing a `CBCentralManager` outside a signed `.app` aborts
//  the process, so the DECISIONS have to be testable without one
//  (`BLEReconnectLadderTests`, plain `swift test`, no radio).
//
import Foundation

/// The §3.6 ladder: when to open a rediscovery scan window for a
/// peripheral we have lost, and — the battery half, and the bug 2.2.6
/// names — when to CLOSE one.
///
/// Two bounds, both of them the point:
/// * a scan window **always** ends (`scanWindowSeconds`), whether or not
///   the peripheral was ever seen;
/// * the interval between windows climbs to a 15-minute cap and stays
///   there, so a radio that died at 2 am costs 30 s of scanning per
///   15 min — a 3.3 % duty cycle — instead of the 100 % the unbounded
///   scan cost before this type existed.
///
/// The ladder never ENDS while auto-reconnect is wanted (a radio dead
/// overnight must still be found at breakfast); only its duty cycle
/// falls.
public struct ReconnectLadder: Sendable, Equatable {
    /// §3.6's table, right-hand column: every window is 30 s.
    public static let scanWindowSeconds: TimeInterval = 30
    /// §3.6's table, middle column — attempt 1 is 20 s, "today's
    /// `reconnectFallbackDelay`, a Heltec's own boot time".
    static let baseDelaysSeconds: [TimeInterval] = [20, 60, 120, 300, 600]
    /// Attempt 6 and every attempt after it: 15 minutes, capped.
    public static let cappedDelaySeconds: TimeInterval = 15 * 60
    /// "±20 % jitter on every interval, so two phones that lost the same
    /// radio do not scan in lockstep."
    public static let maxJitterFraction: Double = 0.2

    /// What the transport should do about the ladder right now. Never
    /// "the peripheral is gone": §1.5 means a window may yield one
    /// sighting, late, or none at all, so "no sighting in this window"
    /// is no information — never evidence.
    public enum Action: Sendable, Equatable {
        case doNothing
        /// Start a `scanForPeripherals(withServices:)` window.
        case startScan(attempt: Int)
        /// Call `stopScan()`. `.windowElapsed` is the 30 s bound (the
        /// fix for 2.2.6); `.cancelled` is the ladder itself being
        /// stood down (auto-reconnect switched off, a different target,
        /// Bluetooth powered off).
        case endScan(EndReason)

        public enum EndReason: Sendable, Equatable {
            case windowElapsed
            case cancelled
        }
    }

    public private(set) var target: UUID?
    /// The moment the link was actually lost — from the iOS 17
    /// `timestamp:` parameter where we have it (§3.4), which is a real
    /// observation and may predate the callback by the whole span the
    /// process was suspended. Never our own clock at callback time when
    /// a measured one is available.
    public private(set) var disconnectedAt: Date?
    public private(set) var attempt: Int = 0
    private(set) var nextFireAt: Date?
    private(set) var scanStartedAt: Date?

    public init() {}

    public var isArmed: Bool { target != nil }
    public var isScanning: Bool { scanStartedAt != nil }

    /// §3.6's interval table, without jitter — pure, total, and pinned
    /// by `A03_AC5`. Attempts below 1 are read as attempt 1 rather than
    /// trapping: a caller that has not armed yet asks for the first
    /// rung.
    public static func ladderDelaySeconds(forAttempt attempt: Int) -> TimeInterval {
        let index = max(1, attempt) - 1
        guard index < baseDelaysSeconds.count else { return cappedDelaySeconds }
        return baseDelaysSeconds[index]
    }

    /// The same table with jitter applied. `jitterFraction` is clamped
    /// to ±20 % so no caller — including a future one that computes it
    /// from something other than `randomJitterFraction()` — can push an
    /// interval outside the band `A03_AC5` pins.
    public static func ladderDelaySeconds(forAttempt attempt: Int, jitterFraction: Double) -> TimeInterval {
        let clamped = min(max(jitterFraction, -maxJitterFraction), maxJitterFraction)
        return ladderDelaySeconds(forAttempt: attempt) * (1 + clamped)
    }

    public static func randomJitterFraction() -> Double {
        Double.random(in: -maxJitterFraction...maxJitterFraction)
    }

    /// Arm (or re-arm) the ladder at attempt 1 for a peripheral we have
    /// just lost. Idempotent for the SAME target and disconnect: two
    /// callbacks for one loss must not restart the clock, or the first
    /// window would never arrive.
    public mutating func arm(target: UUID, disconnectedAt: Date,
                             jitterFraction: Double = ReconnectLadder.randomJitterFraction()) {
        if self.target == target, self.disconnectedAt == disconnectedAt { return }
        self.target = target
        self.disconnectedAt = disconnectedAt
        attempt = 1
        scanStartedAt = nil
        nextFireAt = disconnectedAt.addingTimeInterval(
            Self.ladderDelaySeconds(forAttempt: 1, jitterFraction: jitterFraction))
    }

    /// Stand the ladder down. Returns `true` when a scan window was
    /// open, so the caller knows it still owes a `stopScan()` — the
    /// caller is the only thing that can touch CoreBluetooth.
    @discardableResult
    public mutating func cancel() -> Bool {
        let wasScanning = isScanning
        self = ReconnectLadder()
        return wasScanning
    }

    /// The peripheral was rediscovered inside a window: the window's
    /// job is done and the ladder is stood down (the caller re-issues
    /// the connect). Returns `true` if a scan was running.
    @discardableResult
    public mutating func noteDiscovered() -> Bool {
        cancel()
    }

    /// The whole §3.6 decision, as a pure function of
    /// `(disconnectedAt, now, attempt, shouldAutoReconnect,
    /// pendingConnectPeripheralID)` and the window clock — `A03_AC6`.
    ///
    /// A `now` that jumps forward by an hour (the suspended-process
    /// case) opens exactly ONE window, not one per skipped rung: the
    /// pending fire time is consumed, not counted.
    public mutating func evaluate(now: Date,
                                  shouldAutoReconnect: Bool,
                                  pendingConnectPeripheralID: UUID?,
                                  jitterFraction: Double = ReconnectLadder.randomJitterFraction()) -> Action {
        guard let target else { return .doNothing }
        // The same two conditions `shouldRunReconnectFallbackScan` has
        // always checked: we still want to reconnect, and the pending
        // connect this ladder is a backstop FOR is still the one
        // outstanding. Either failing means the ladder is stale.
        guard shouldAutoReconnect, pendingConnectPeripheralID == target else {
            return cancel() ? .endScan(.cancelled) : .doNothing
        }
        if let scanStartedAt {
            guard now.timeIntervalSince(scanStartedAt) >= Self.scanWindowSeconds else { return .doNothing }
            self.scanStartedAt = nil
            attempt += 1
            nextFireAt = now.addingTimeInterval(
                Self.ladderDelaySeconds(forAttempt: attempt, jitterFraction: jitterFraction))
            return .endScan(.windowElapsed)
        }
        guard let nextFireAt, now >= nextFireAt else { return .doNothing }
        self.nextFireAt = nil
        scanStartedAt = now
        return .startScan(attempt: attempt)
    }

    /// The next moment `evaluate` could return anything but
    /// `.doNothing` — used ONLY to size the opportunistic `Task.sleep`
    /// nudge for a process that happens to still be running. Nothing is
    /// correct because of this value; the clock delta above is.
    public func nextDeadline() -> Date? {
        if let scanStartedAt { return scanStartedAt.addingTimeInterval(Self.scanWindowSeconds) }
        return nextFireAt
    }
}

/// A03 §3.4's one decision, extracted so `BLEContractTests` can pin it
/// with no `CBCentralManager`: what a disconnect means depends on
/// whether the SYSTEM is already reconnecting for us (iOS 17
/// auto-reconnect), and on whether the bond is gone.
public enum BLEDisconnectAction: Sendable, Equatable {
    /// `isReconnecting == true`: publish the honest link state and stop.
    /// The system owns the retry; a second `connect()` is duplicated
    /// radio work, and a fallback scan on top of it is worse.
    case systemIsReconnecting
    /// Today's behaviour: re-issue the pending connect and arm the
    /// (bounded, §3.6) ladder.
    case reconnectOurselves
    /// A lost bond, or an explicit disconnect: no retry can fix it.
    case stop

    public static func action(isReconnecting: Bool, isBondLost: Bool, shouldAutoReconnect: Bool) -> BLEDisconnectAction {
        guard shouldAutoReconnect, !isBondLost else { return .stop }
        return isReconnecting ? .systemIsReconnecting : .reconnectOurselves
    }
}

/// A03 §3.4/§3.6's diagnostics, all three of them observations rather
/// than estimates — Diagnostics renders these verbatim and says UNKNOWN
/// where there is no observation yet, never a zero standing in for one.
public struct BLELinkDiagnostics: Sendable, Equatable {
    /// How many rediscovery scan windows this transport has opened.
    public var scanStarts: Int
    /// How many connect chains completed with nobody awaiting them —
    /// i.e. the link came back on its own, without a CONNECT tap.
    public var reconnects: Int
    /// When the most recent of those completed. `nil` — never a
    /// fabricated date — when it has not happened in this process.
    public var lastReconnectAt: Date?
    /// The `timestamp:` of the most recent disconnect, as CoreBluetooth
    /// measured it (§3.4) — which can predate the callback by the whole
    /// span the process was suspended.
    public var lastDisconnectAt: Date?
    /// Whether the system told us it was reconnecting on its own at the
    /// most recent disconnect.
    public var isSystemReconnecting: Bool

    public init(scanStarts: Int = 0, reconnects: Int = 0, lastReconnectAt: Date? = nil,
                lastDisconnectAt: Date? = nil, isSystemReconnecting: Bool = false) {
        self.scanStarts = scanStarts
        self.reconnects = reconnects
        self.lastReconnectAt = lastReconnectAt
        self.lastDisconnectAt = lastDisconnectAt
        self.isSystemReconnecting = isSystemReconnecting
    }
}

/// The seam Diagnostics reads those counters through, so the screen
/// depends on a protocol rather than on `BLETransport` itself (the app
/// target can then hand it whatever transport the composition root
/// actually built, or nothing at all on a stack that has no BLE).
public protocol BLELinkDiagnosticsProviding: Sendable {
    func linkDiagnostics() async -> BLELinkDiagnostics
}
