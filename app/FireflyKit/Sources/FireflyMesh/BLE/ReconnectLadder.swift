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
import CoreBluetooth
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
    /// A03 §3.5 — whether this ladder is a backstop BEHIND a pending
    /// `central.connect()` (the ordinary case, `true`) or the only
    /// mechanism running (`false`).
    ///
    /// The second case is exactly one row of §3.5's table: Bluetooth
    /// came back on, `retrievePeripherals(withIdentifiers:)` did not
    /// resolve the remembered identifier, so there is no `CBPeripheral`
    /// to issue a connect against at all and "arm the §3.6 ladder
    /// instead" is the whole recovery. Keeping the distinction explicit
    /// — rather than letting the transport write the identifier into
    /// `pendingConnectPeripheralID` without having issued anything —
    /// is what stops a ladder armed this way from silently suppressing
    /// the next real `issueConnect(_:)` for the same peripheral
    /// (`BLETransport.shouldIssueConnect(for:pendingConnectPeripheralID:)`).
    public private(set) var requiresPendingConnect: Bool = true

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
                             requiresPendingConnect: Bool = true,
                             jitterFraction: Double = ReconnectLadder.randomJitterFraction()) {
        if self.target == target, self.disconnectedAt == disconnectedAt,
           self.requiresPendingConnect == requiresPendingConnect, !isExpectedReboot { return }
        self.target = target
        self.disconnectedAt = disconnectedAt
        self.requiresPendingConnect = requiresPendingConnect
        isExpectedReboot = false
        attempt = 1
        scanStartedAt = nil
        nextFireAt = disconnectedAt.addingTimeInterval(
            Self.ladderDelaySeconds(forAttempt: 1, jitterFraction: jitterFraction))
    }

    /// A03 §3.6 **amendment, 2026-09-14** — arm for the ONE disconnect
    /// this app can see coming: the reboot a `commit_edit_settings`
    /// triggers (`MeshTransport.noteExpectedReboot()`).
    ///
    /// Identical to `arm(target:disconnectedAt:…)` in every respect but
    /// the first rung's delay, which is **zero**: the first
    /// `evaluate(now:…)` at or after `disconnectedAt` opens the
    /// rediscovery window immediately instead of 20 s (±20 %) later.
    /// That 20 s is §3.6's own "a Heltec's own boot time" — the right
    /// answer for a radio that vanished for an unknown reason, and the
    /// wrong one for a radio we just told to reboot and are actively
    /// waiting for: the bench measured a crew join spending the whole
    /// first rung doing nothing while the puck had already finished
    /// booting (2026-09-14, `-FireflyDebugJoinCrew FIRE-8MNTT2`).
    ///
    /// Only the FIRST rung is special. If the immediate window closes
    /// without a sighting, `evaluate` climbs the ordinary table from
    /// attempt 2 (60 s, 2 min, 5 min, 10 min, 15 min capped), so the
    /// duty-cycle bound §3.6 exists to enforce is unchanged — one extra
    /// 30 s window, once, per commit.
    ///
    /// Jitter is deliberately NOT applied to a zero delay: the reason
    /// for jitter is two phones that lost the same radio scanning in
    /// lockstep (§3.6), and two phones do not commit to the same radio
    /// at the same moment — each one's expected reboot is its own
    /// deliberate act, not a shared event.
    public mutating func armExpectingReboot(target: UUID, disconnectedAt: Date,
                                            requiresPendingConnect: Bool = true) {
        if self.target == target, self.disconnectedAt == disconnectedAt,
           self.requiresPendingConnect == requiresPendingConnect, isExpectedReboot { return }
        self.target = target
        self.disconnectedAt = disconnectedAt
        self.requiresPendingConnect = requiresPendingConnect
        isExpectedReboot = true
        attempt = 1
        scanStartedAt = nil
        nextFireAt = disconnectedAt
    }

    /// Whether this ladder was armed by `armExpectingReboot` — read only
    /// for diagnostics and by the tests that pin the distinction. It
    /// stops being true the moment the ladder is cancelled or re-armed
    /// ordinarily.
    public private(set) var isExpectedReboot: Bool = false

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
        // always checked — with the second one waived for the one §3.5
        // row that has no pending connect to check against (see
        // `requiresPendingConnect`'s own doc comment). A ladder armed
        // that way still stands down the moment auto-reconnect does.
        guard shouldAutoReconnect,
              !requiresPendingConnect || pendingConnectPeripheralID == target else {
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

/// A03 §3.6 amendment (2026-09-14) — "did WE cause this disconnect?",
/// as a pure, bounded, one-shot marker.
///
/// `MeshtasticClient` sends `commit_edit_settings` and the firmware
/// answers by disabling Bluetooth, saving to flash and rebooting. That
/// is the ONE disconnect this app can see coming, and the whole value of
/// knowing it is that the recovery can be prompt instead of waiting out
/// §3.6's first rung (20 s, sized for a radio that vanished for reasons
/// unknown).
///
/// Two bounds, both deliberate:
/// * **one-shot** — `consume(now:)` answers `true` at most once per
///   `arm(at:)`. One commit produces one reboot, so a second disconnect
///   is an ordinary loss and gets the ordinary ladder.
/// * **time-bounded** — a notice older than `lifetime` is stale and
///   answers `false`. Without this, a commit whose reboot never came
///   would leave the marker standing and silently re-label some
///   unrelated drop, minutes later, as expected. The bench's own
///   measurement is the bound's justification: the disconnect follows
///   the commit write within a second or two, never tens of seconds.
///
/// Pure and CoreBluetooth-free for the same reason every other decision
/// in this file is: a `CBCentralManager` cannot be constructed outside a
/// signed `.app`, so a rule only reachable through one is a rule no unit
/// test can check.
public struct ExpectedRebootWindow: Sendable, Equatable {
    /// How long a commit notice stays good for. 30 s is generous
    /// against the measured gap (about a second) and far short of the
    /// post-commit ready budget (`MeshtasticClient
    /// .defaultPostCommitReadyTimeout`), so a radio that is genuinely
    /// slow to drop is still covered while a drop minutes later is not.
    public static let defaultLifetime: TimeInterval = 30

    public private(set) var armedAt: Date?

    public init() {}

    public var isArmed: Bool { armedAt != nil }

    /// Record that a `commit_edit_settings` has just gone out.
    public mutating func arm(at: Date) { armedAt = at }

    /// Stand the marker down without consuming it — a user-initiated
    /// disconnect, a different peripheral, Bluetooth off. Nothing after
    /// any of those is the reboot we asked for.
    public mutating func cancel() { armedAt = nil }

    /// `true` exactly once, for a disconnect that arrives within
    /// `lifetime` of the commit. Clears the marker either way: a stale
    /// notice has no second chance, and a consumed one has had its.
    public mutating func consume(now: Date, lifetime: TimeInterval = ExpectedRebootWindow.defaultLifetime) -> Bool {
        guard let armedAt else { return false }
        self.armedAt = nil
        let elapsed = now.timeIntervalSince(armedAt)
        return elapsed >= 0 && elapsed <= lifetime
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

/// A03 §3.6's other decision — "Any `.poweredOff` cancels the ladder;
/// `.poweredOn` restarts it at attempt 1" — extracted for exactly the
/// reason `BLEDisconnectAction` above is (REVIEW FIX, PR #310: the rule
/// lived inline in `handleCentralStateUpdate`, which needs a live
/// `CBCentralManager` to reach, so deleting the cancel outright broke no
/// test at all — and a scan window left open across a Bluetooth power
/// cycle is the battery bug 2.2.6 in its worst form).
///
/// The `CBManagerState` -> action mapping itself is
/// `BLETransport.ladderAction(forCentralState:shouldAutoReconnect:
/// hasPendingConnect:)`; this enum is the pure half, same as every other
/// decision type in this file. It is the ladder's share of §3.5 and
/// nothing more — the SESSION's share (`retrievePeripherals`, the
/// terminal-versus-transient question, the restore-ordering fix) is
/// `BLEPowerStateAction` below, and `handleCentralStateUpdate` applies
/// both: this one first, because a scan window left open across a power
/// cycle is the 2.2.6 battery bug whatever the session is doing.
public enum BLELadderPowerAction: Sendable, Equatable {
    /// Stand the ladder down, closing any scan window it still owes a
    /// `stopScan()` for.
    case cancel
    /// Bluetooth is back with a connect still outstanding: cancel and
    /// re-arm at attempt 1, rather than resuming a ladder that spent the
    /// outage climbing.
    case restartAtAttemptOne
    /// Powered on with nothing for the ladder to do.
    case leaveAsIs
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
    /// A03 §3.1 (S1b) — how many CoreBluetooth state RESTORATIONS this
    /// process has adopted. `0` here is a real observation ("this
    /// process was not relaunched into a restored session"), which is
    /// why it is a count and not an optional; `lastRestoreAt` below is
    /// the one that must stay nil until something actually happened.
    public var restores: Int
    /// When the most recent restore was adopted. `nil` — never a
    /// fabricated date — when this process has adopted none.
    public var lastRestoreAt: Date?
    /// What that restore restored INTO — the restored peripheral's own
    /// `CBPeripheralState`, mapped onto the branch it took. `nil` until
    /// there has been one.
    public var lastRestoreAction: BLERestoreAction?

    public init(scanStarts: Int = 0, reconnects: Int = 0, lastReconnectAt: Date? = nil,
                lastDisconnectAt: Date? = nil, isSystemReconnecting: Bool = false,
                restores: Int = 0, lastRestoreAt: Date? = nil,
                lastRestoreAction: BLERestoreAction? = nil) {
        self.scanStarts = scanStarts
        self.reconnects = reconnects
        self.lastReconnectAt = lastReconnectAt
        self.lastDisconnectAt = lastDisconnectAt
        self.isSystemReconnecting = isSystemReconnecting
        self.restores = restores
        self.lastRestoreAt = lastRestoreAt
        self.lastRestoreAction = lastRestoreAction
    }
}

/// The seam Diagnostics reads those counters through, so the screen
/// depends on a protocol rather than on `BLETransport` itself (the app
/// target can then hand it whatever transport the composition root
/// actually built, or nothing at all on a stack that has no BLE).
public protocol BLELinkDiagnosticsProviding: Sendable {
    func linkDiagnostics() async -> BLELinkDiagnostics
}

/// A03 §3.5's table — what a `CBManagerState` transition means for the
/// SESSION (as opposed to `BLELadderPowerAction` above, which is the
/// ladder's share of the same callback).
///
/// Extracted for the reason every other decision in this file is: a
/// `CBCentralManager` cannot be constructed outside a signed `.app`, so
/// a rule only reachable through a live manager is a rule no unit test
/// can check. `BLEStateRestorationTests` pins every row (A03_AC4).
public enum BLEPowerStateAction: Sendable, Equatable {
    /// Nothing to do — and deliberately the answer for `.poweredOn`
    /// while a restore is pending (§3.1's ordering fix), for
    /// `.poweredOn` with auto-reconnect off (the user disconnected;
    /// Bluetooth coming back is not them asking to reconnect), for
    /// `.poweredOn` with no remembered peripheral (there is nothing to
    /// reconnect TO — and connecting to whatever Meshtastic node
    /// happens to be advertising would be connecting to a stranger's
    /// radio), and for `.unknown` (transient, says nothing yet).
    case doNothing
    /// `.poweredOn`, auto-reconnect wanted, a remembered peripheral:
    /// `retrievePeripherals(withIdentifiers:)` then a pending
    /// `connect()`. NEVER a scan first — §3.5's own wording, and the
    /// battery reason §4.1 gives.
    ///
    /// Whether that retrieve actually RESOLVES is not knowable from
    /// this function's inputs (it needs the live manager), so §3.5's
    /// "if the identifier no longer resolves, arm the §3.6 ladder
    /// instead" is handled at the call site — see
    /// `BLETransport.retrieveAndReconnectPreferred()`.
    case retrieveAndConnect
    /// `.poweredOff`: cancel the ladder, end any scan window, drop the
    /// characteristic references, clear the pending connect
    /// (CoreBluetooth has invalidated it), fail every outstanding
    /// continuation and publish `.disconnected(reason: "bluetooth-off")`.
    /// `shouldAutoReconnect` is deliberately PRESERVED — the user
    /// turning Bluetooth off is not the user asking us never to
    /// reconnect.
    case bluetoothOff
    /// `.resetting`: a transient loss, not a terminal one. Publish the
    /// honest reason and wait for the next transition.
    case transientLoss
    /// `.unauthorized`: terminal, and published as its OWN reason so the
    /// status line can say "Firefly can't use Bluetooth" rather than a
    /// generic failure (§3.10).
    case unauthorized
    /// `.unsupported`: terminal, same reasoning.
    case unsupported
}

/// A03 §3.1's other table: what a RESTORED peripheral's own
/// `CBPeripheralState` means (the Meshtastic-Apple pattern — branch on
/// the state rather than assume one).
///
/// Pure, and `public`, for the same reason as everything else here:
/// `willRestoreState` can only be reached with a live manager and a
/// relaunch, so the DECISION has to be checkable without one.
public enum BLERestoreAction: Sendable, Equatable {
    /// `.connected` — already up at the GATT level. Adopt it: rediscover
    /// services (THIS process has no characteristic references), let the
    /// ordinary connect chain run to `completeConnect`, and let the
    /// client re-run the handshake, because A01 deliberately does not
    /// persist the nodeDB. No `central.connect()`: it already is
    /// connected.
    case adoptConnected
    /// `.connecting` — a pending connect from before the relaunch, which
    /// CoreBluetooth resumes on its own. Record it as pending so an
    /// explicit `connect()` racing this relaunch does not issue a
    /// SECOND native connect for the same peripheral.
    case keepPendingConnect
    /// `.disconnected`/`.disconnecting` — re-arm a pending connect the
    /// same way a mid-session drop does (§3.4/§3.6).
    case reconnect
}

public extension BLERestoreAction {
    /// §3.1's branch, as a total function of the restored peripheral's
    /// own state.
    static func action(forPeripheralState state: CBPeripheralState) -> BLERestoreAction {
        switch state {
        case .connected: return .adoptConnected
        case .connecting: return .keepPendingConnect
        case .disconnected, .disconnecting: return .reconnect
        @unknown default: return .reconnect
        }
    }
}
