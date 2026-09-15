//
//  BLEReconnectLadderTests.swift — A03 §3.4/§3.6, pinned with no radio.
//
//  `FireflyMesh` is imported WITHOUT `@testable` on purpose, exactly as
//  `BLEContractTests` does: these decisions are supposed to be reachable
//  from the package's public surface, because constructing a
//  `CBCentralManager` outside a signed `.app` aborts the process and a
//  decision only reachable through a live manager is a decision no unit
//  test can ever check.
//
import CoreBluetooth
import FireflyMesh
import XCTest

final class BLEReconnectLadderTests: XCTestCase {

    private let target = UUID(uuidString: "3F2504E0-4F89-11D3-9A0C-0305E82C3301")!
    private let other = UUID(uuidString: "3F2504E0-4F89-11D3-9A0C-0305E82C3302")!
    private let t0 = Date(timeIntervalSince1970: 1_700_000_000)

    // MARK: - A03_AC5 — the interval table

    /// A03_AC5: monotonically non-decreasing, reaches the 15-minute cap,
    /// and STAYS there for every later attempt.
    func testA03_AC5_LadderDelayIsMonotonicAndCaps() {
        var previous: TimeInterval = 0
        for attempt in 1...50 {
            let delay = ReconnectLadder.ladderDelaySeconds(forAttempt: attempt)
            XCTAssertGreaterThanOrEqual(delay, previous, "attempt \(attempt) went backwards")
            XCTAssertLessThanOrEqual(delay, ReconnectLadder.cappedDelaySeconds)
            previous = delay
        }
        XCTAssertEqual(ReconnectLadder.ladderDelaySeconds(forAttempt: 1), 20, "the Heltec's own boot time")
        XCTAssertEqual(ReconnectLadder.ladderDelaySeconds(forAttempt: 2), 60)
        XCTAssertEqual(ReconnectLadder.ladderDelaySeconds(forAttempt: 3), 120)
        XCTAssertEqual(ReconnectLadder.ladderDelaySeconds(forAttempt: 4), 300)
        XCTAssertEqual(ReconnectLadder.ladderDelaySeconds(forAttempt: 5), 600)
        XCTAssertEqual(ReconnectLadder.ladderDelaySeconds(forAttempt: 6), 900, "the cap")
        XCTAssertEqual(ReconnectLadder.ladderDelaySeconds(forAttempt: 7), 900)
        XCTAssertEqual(ReconnectLadder.ladderDelaySeconds(forAttempt: 1_000), 900)
    }

    /// A03_AC5: jitter stays within ±20 % — including for a caller that
    /// hands in a fraction outside the band, which is clamped rather
    /// than trusted. Checked against the RANDOM generator too, not only
    /// against hand-picked fractions: a proxy check that only ever tests
    /// the fractions the test itself chose would pass for an
    /// implementation that ignored its own bound.
    func testA03_AC5_JitterStaysWithinTwentyPercent() {
        for attempt in 1...8 {
            let base = ReconnectLadder.ladderDelaySeconds(forAttempt: attempt)
            for _ in 0..<200 {
                let jittered = ReconnectLadder.ladderDelaySeconds(
                    forAttempt: attempt, jitterFraction: ReconnectLadder.randomJitterFraction())
                XCTAssertGreaterThanOrEqual(jittered, base * 0.8)
                XCTAssertLessThanOrEqual(jittered, base * 1.2)
            }
            XCTAssertEqual(ReconnectLadder.ladderDelaySeconds(forAttempt: attempt, jitterFraction: 5),
                            base * 1.2, accuracy: 0.001, "an out-of-band fraction is clamped, not trusted")
            XCTAssertEqual(ReconnectLadder.ladderDelaySeconds(forAttempt: attempt, jitterFraction: -5),
                            base * 0.8, accuracy: 0.001)
        }
    }

    /// Two phones that lost the same radio must not scan in lockstep —
    /// which means the jitter has to actually VARY, not merely be
    /// bounded. (A constant 0 would satisfy the band check above.)
    func testA03_AC5_JitterActuallyVaries() {
        let values = Set((0..<200).map { _ in ReconnectLadder.randomJitterFraction() })
        XCTAssertGreaterThan(values.count, 100, "jitter that never varies is not jitter")
    }

    // MARK: - A03_AC6 — a clock, not a sleeping task

    /// A03_AC6: the first window opens at the first rung, and only then.
    func testA03_AC6_NoScanBeforeTheFirstRungIsDue() {
        var ladder = ReconnectLadder()
        ladder.arm(target: target, disconnectedAt: t0, jitterFraction: 0)
        XCTAssertEqual(evaluate(&ladder, at: t0), .doNothing)
        XCTAssertEqual(evaluate(&ladder, at: at(19)), .doNothing)
        XCTAssertEqual(evaluate(&ladder, at: at(20)), .startScan(attempt: 1))
    }

    /// A03_AC6, the load-bearing one: a `now` that jumps forward by an
    /// hour — the suspended-process case §1.7 describes, where no timer
    /// fired for any of the rungs in between — opens exactly ONE window,
    /// not one per skipped rung.
    func testA03_AC6_AnHourLongJumpFiresOnceNotOncePerSkippedRung() {
        var ladder = ReconnectLadder()
        ladder.arm(target: target, disconnectedAt: t0, jitterFraction: 0)

        var starts = 0
        var action = evaluate(&ladder, at: at(3600))
        if case .startScan = action { starts += 1 }
        // Evaluate repeatedly at the SAME instant, the way a burst of
        // delegate callbacks on one wake would.
        for _ in 0..<10 {
            action = evaluate(&ladder, at: at(3600))
            if case .startScan = action { starts += 1 }
        }
        XCTAssertEqual(starts, 1, "one wake, one window — never one window per rung the sleep never served")
        XCTAssertTrue(ladder.isScanning)
    }

    /// A03_AC6: the decision is a pure function of its inputs — it never
    /// consults a clock of its own. Two ladders driven with identical
    /// inputs produce identical actions and identical state.
    func testA03_AC6_TheDecisionIsPureInItsInputs() {
        var a = ReconnectLadder(), b = ReconnectLadder()
        a.arm(target: target, disconnectedAt: t0, jitterFraction: 0)
        b.arm(target: target, disconnectedAt: t0, jitterFraction: 0)
        for offset in stride(from: 0.0, through: 400.0, by: 7.0) {
            XCTAssertEqual(evaluate(&a, at: at(offset)), evaluate(&b, at: at(offset)))
            XCTAssertEqual(a, b)
        }
    }

    // MARK: - A03_AC7 — the window always closes

    /// A03_AC7, and the single most important battery line in A03: a
    /// scan window closes at or before 30 s of elapsed evaluated time
    /// EVEN WHEN THE PERIPHERAL IS NEVER DISCOVERED. This is the exact
    /// failure audit 2.2.6 describes — "a radio whose battery died at
    /// 2 am leaves the phone scanning continuously until morning".
    func testA03_AC7_AScanWindowClosesEvenWhenNothingIsEverSeen() {
        var ladder = ReconnectLadder()
        ladder.arm(target: target, disconnectedAt: t0, jitterFraction: 0)
        XCTAssertEqual(evaluate(&ladder, at: at(20)), .startScan(attempt: 1))
        XCTAssertEqual(evaluate(&ladder, at: at(49)), .doNothing, "still inside the 30 s window")
        XCTAssertEqual(evaluate(&ladder, at: at(50)), .endScan(.windowElapsed))
        XCTAssertFalse(ladder.isScanning)
    }

    /// The duty cycle §4.2 claims — 30 s of scanning per 15 min at the
    /// cap, i.e. 3.3 % — measured rather than asserted: drive eight
    /// hours of wall clock through the ladder one minute at a time and
    /// add up the seconds it was actually scanning.
    func testA03_AC7_DutyCycleFallsToTheBudgetedThreePointThreePercent() {
        var ladder = ReconnectLadder()
        ladder.arm(target: target, disconnectedAt: t0, jitterFraction: 0)
        var scanningSeconds = 0.0
        var scanStartedAt: Date?
        var step = 0.0
        let horizon = 8 * 3600.0
        while step <= horizon {
            let at = self.at(step)
            switch evaluate(&ladder, at: at, jitter: 0) {
            case .startScan: scanStartedAt = at
            case .endScan:
                if let scanStartedAt { scanningSeconds += at.timeIntervalSince(scanStartedAt) }
                scanStartedAt = nil
            case .doNothing: break
            }
            step += 5 // a 5 s evaluation cadence, generous to the ladder
        }
        let dutyCycle = scanningSeconds / horizon
        XCTAssertLessThan(dutyCycle, 0.05, "A03 §4.2 budgets 3.3 %; anything near 100 % is audit 2.2.6 again")
        XCTAssertGreaterThan(scanningSeconds, 0, "a ladder that never scans is not a backstop")
    }

    /// The ladder never ENDS while auto-reconnect is wanted — "a radio
    /// that is dead overnight must still be found at breakfast" — it
    /// only slows down.
    func testLadderKeepsTryingOvernight() {
        var ladder = ReconnectLadder()
        ladder.arm(target: target, disconnectedAt: t0, jitterFraction: 0)
        var windows = 0
        var step = 0.0
        while step <= 12 * 3600 {
            if case .startScan = evaluate(&ladder, at: at(step), jitter: 0) { windows += 1 }
            step += 5
        }
        XCTAssertGreaterThan(windows, 40, "twelve hours at a 15 min cap is ~48 windows, not zero")
    }

    /// Auto-reconnect switched off, or a connect outstanding for a
    /// DIFFERENT peripheral, stands the ladder down — and closes the
    /// window it had open, rather than leaking a scan.
    func testLadderStandsDownAndClosesItsWindowWhenSuperseded() {
        var ladder = ReconnectLadder()
        ladder.arm(target: target, disconnectedAt: t0, jitterFraction: 0)
        XCTAssertEqual(evaluate(&ladder, at: at(20)), .startScan(attempt: 1))
        XCTAssertEqual(ladder.evaluate(now: at(25), shouldAutoReconnect: false,
                                        pendingConnectPeripheralID: target, jitterFraction: 0),
                        .endScan(.cancelled))
        XCTAssertFalse(ladder.isArmed)

        var second = ReconnectLadder()
        second.arm(target: target, disconnectedAt: t0, jitterFraction: 0)
        XCTAssertEqual(evaluate(&second, at: at(20)), .startScan(attempt: 1))
        XCTAssertEqual(second.evaluate(now: at(25), shouldAutoReconnect: true,
                                        pendingConnectPeripheralID: other, jitterFraction: 0),
                        .endScan(.cancelled), "a newer loss for a different radio supersedes this ladder")
    }

    /// Arming twice for the same loss must not restart the clock — two
    /// delegate callbacks for one disconnect are ordinary, and a ladder
    /// that re-armed on each would never reach its first window.
    func testArmingTwiceForTheSameLossDoesNotRestartTheClock() {
        var ladder = ReconnectLadder()
        ladder.arm(target: target, disconnectedAt: t0, jitterFraction: 0)
        XCTAssertEqual(evaluate(&ladder, at: at(19)), .doNothing)
        ladder.arm(target: target, disconnectedAt: t0, jitterFraction: 0)
        XCTAssertEqual(evaluate(&ladder, at: at(20)), .startScan(attempt: 1),
                        "the first window is still due 20 s after the LOSS, not 20 s after the last callback")
    }

    /// A sighting inside the window ends it — and the ladder with it.
    func testDiscoveryEndsTheWindow() {
        var ladder = ReconnectLadder()
        ladder.arm(target: target, disconnectedAt: t0, jitterFraction: 0)
        XCTAssertEqual(evaluate(&ladder, at: at(20)), .startScan(attempt: 1))
        XCTAssertTrue(ladder.noteDiscovered(), "the caller still owes a stopScan()")
        XCTAssertFalse(ladder.isArmed)
        XCTAssertEqual(evaluate(&ladder, at: at(600)), .doNothing)
    }

    // MARK: - A03 §3.4 — the iOS 17 disconnect decision

    /// §3.4's table, both rows, plus the terminal cases. `isReconnecting`
    /// changes exactly one decision, and it is the one that matters:
    /// while the system is reconnecting we must NOT issue a second
    /// connect and must NOT arm a scan.
    func testA03_3_4_DisconnectActionTable() {
        XCTAssertEqual(BLEDisconnectAction.action(isReconnecting: true, isBondLost: false, shouldAutoReconnect: true),
                        .systemIsReconnecting)
        XCTAssertEqual(BLEDisconnectAction.action(isReconnecting: false, isBondLost: false, shouldAutoReconnect: true),
                        .reconnectOurselves)
        // A lost bond is terminal whatever the system says it is doing:
        // no retry can fix it (the user must forget the device).
        XCTAssertEqual(BLEDisconnectAction.action(isReconnecting: true, isBondLost: true, shouldAutoReconnect: true),
                        .stop)
        XCTAssertEqual(BLEDisconnectAction.action(isReconnecting: false, isBondLost: true, shouldAutoReconnect: true),
                        .stop)
        // A user-initiated disconnect cleared `shouldAutoReconnect`
        // first; nothing here may undo that.
        for isReconnecting in [true, false] {
            for isBondLost in [true, false] {
                XCTAssertEqual(
                    BLEDisconnectAction.action(isReconnecting: isReconnecting, isBondLost: isBondLost,
                                                shouldAutoReconnect: false),
                    .stop, "auto-reconnect off means off")
            }
        }
    }

    /// §3.4 — the iOS 17 auto-reconnect option is actually requested on
    /// iOS, and deliberately not on macOS (that platform is the
    /// hardware-test rig; see `BLETransport.connectOptions`' own doc
    /// comment). The literal key, not a re-spelling of it.
    func testA03_3_4_ConnectOptionsRequestAutoReconnectOnIOSOnly() {
        #if os(iOS)
        XCTAssertEqual(BLETransport.connectOptions["kCBConnectOptionEnableAutoReconnect"], true)
        XCTAssertEqual(BLETransport.connectOptions.count, 1)
        #else
        XCTAssertTrue(BLETransport.connectOptions.isEmpty, "macOS keeps `nil`, per §3.4")
        #endif
    }

    /// The `reason` string that tells "the system is handling it" apart
    /// from an ordinary loss. `TransportEvent` has no `.reconnecting`
    /// case and §3.4 explicitly does not add one — the client derives
    /// `LinkState.reconnecting(attempt:)` from this `.disconnected`.
    func testA03_3_4_SystemReconnectingHasItsOwnDisconnectReason() {
        XCTAssertEqual(BLETransport.systemReconnectingReason, "system-reconnecting")
        let event = TransportEvent.disconnected(reason: BLETransport.systemReconnectingReason)
        guard case .disconnected(let reason) = event else { return XCTFail("wrong case") }
        XCTAssertEqual(reason, "system-reconnecting")
    }

    // MARK: - A03 §3.6 — Bluetooth off, and back on

    /// REVIEW FIX (PR #310). §3.6's last rule — "Any `.poweredOff`
    /// cancels the ladder; `.poweredOn` restarts it at attempt 1" — was
    /// implemented inline in `handleCentralStateUpdate`, which cannot be
    /// reached without a live `CBCentralManager`. Deleting the cancel
    /// outright left all 927 tests green, and a rediscovery scan window
    /// left open across a Bluetooth power cycle is audit 2.2.6 in its
    /// worst form. Every state, both booleans, pinned here.
    func testA03_3_6_EveryNonPoweredOnStateCancelsTheLadder() {
        let states: [CBManagerState] = [.unknown, .resetting, .unsupported, .unauthorized, .poweredOff]
        for state in states {
            for shouldAutoReconnect in [true, false] {
                for hasPendingConnect in [true, false] {
                    XCTAssertEqual(
                        BLETransport.ladderAction(forCentralState: state,
                                                   shouldAutoReconnect: shouldAutoReconnect,
                                                   hasPendingConnect: hasPendingConnect),
                        .cancel,
                        "\(state.rawValue) must stand the ladder down — a window left open here is the battery bug")
                }
            }
        }
    }

    /// Powered on restarts at attempt 1, but only when there is actually
    /// something to reconnect to: auto-reconnect wanted AND a connect
    /// still outstanding. Anything else leaves the ladder alone rather
    /// than arming a scan nobody asked for.
    func testA03_3_6_PoweredOnRestartsAtAttemptOneOnlyWithAConnectOutstanding() {
        XCTAssertEqual(BLETransport.ladderAction(forCentralState: .poweredOn, shouldAutoReconnect: true,
                                                  hasPendingConnect: true),
                        .restartAtAttemptOne)
        XCTAssertEqual(BLETransport.ladderAction(forCentralState: .poweredOn, shouldAutoReconnect: true,
                                                  hasPendingConnect: false),
                        .leaveAsIs)
        XCTAssertEqual(BLETransport.ladderAction(forCentralState: .poweredOn, shouldAutoReconnect: false,
                                                  hasPendingConnect: true),
                        .leaveAsIs, "a user-initiated disconnect must not be undone by a power cycle")
        XCTAssertEqual(BLETransport.ladderAction(forCentralState: .poweredOn, shouldAutoReconnect: false,
                                                  hasPendingConnect: false),
                        .leaveAsIs)
    }

    /// And the restart really is attempt 1, not a resumed climb: a radio
    /// that was reachable the whole time the phone's Bluetooth was off
    /// must be found in 20 s, not in 15 min.
    func testA03_3_6_ARestartedLadderOpensItsFirstWindowAtTheFirstRung() {
        var ladder = ReconnectLadder()
        ladder.arm(target: target, disconnectedAt: t0, jitterFraction: 0)
        // Climb to the cap.
        var step = 0.0
        while step <= 3 * 3600 {
            _ = evaluate(&ladder, at: at(step), jitter: 0)
            step += 5
        }
        XCTAssertGreaterThan(ladder.attempt, 5, "the ladder really did climb")

        // Bluetooth off, then back on: the transport cancels and re-arms.
        ladder.cancel()
        ladder.arm(target: target, disconnectedAt: at(step), jitterFraction: 0)
        XCTAssertEqual(ladder.attempt, 1)
        XCTAssertEqual(evaluate(&ladder, at: at(step + 19), jitter: 0), .doNothing)
        XCTAssertEqual(evaluate(&ladder, at: at(step + 20), jitter: 0), .startScan(attempt: 1))
    }

    // MARK: - A03 §3.5 — a ladder with no pending connect behind it

    /// §3.5's "if the identifier no longer resolves, arm the §3.6 ladder
    /// instead" row: Bluetooth came back on,
    /// `retrievePeripherals(withIdentifiers:)` returned nothing, and
    /// there is therefore no `CBPeripheral` to issue a connect against
    /// at all. The ladder still has to run — it is the whole recovery —
    /// so its usual "is the pending connect still mine?" check is waived
    /// for exactly this arming.
    ///
    /// The default arming is unchanged and still cancels on a nil
    /// pending connect (the test below), which is what keeps this from
    /// being a quiet weakening of A03_AC6.
    func testALadderArmedWithoutAPendingConnectStillOpensItsWindow() {
        var ladder = ReconnectLadder()
        ladder.arm(target: target, disconnectedAt: t0, requiresPendingConnect: false, jitterFraction: 0)

        XCTAssertEqual(ladder.evaluate(now: at(19), shouldAutoReconnect: true,
                                        pendingConnectPeripheralID: nil, jitterFraction: 0),
                       .doNothing, "the rung is not due yet")
        XCTAssertEqual(ladder.evaluate(now: at(20), shouldAutoReconnect: true,
                                        pendingConnectPeripheralID: nil, jitterFraction: 0),
                       .startScan(attempt: 1))
        // And it still CLOSES on time — the 2.2.6 battery bound is not
        // waived along with the pending-connect check.
        XCTAssertEqual(ladder.evaluate(now: at(50), shouldAutoReconnect: true,
                                        pendingConnectPeripheralID: nil, jitterFraction: 0),
                       .endScan(.windowElapsed))
    }

    /// …and it stands down the moment auto-reconnect does, exactly like
    /// any other ladder. "No pending connect to check" is not "no
    /// conditions at all".
    func testALadderArmedWithoutAPendingConnectStillStandsDownWhenAutoReconnectDoes() {
        var ladder = ReconnectLadder()
        ladder.arm(target: target, disconnectedAt: t0, requiresPendingConnect: false, jitterFraction: 0)
        _ = ladder.evaluate(now: at(20), shouldAutoReconnect: true,
                             pendingConnectPeripheralID: nil, jitterFraction: 0)

        XCTAssertEqual(ladder.evaluate(now: at(25), shouldAutoReconnect: false,
                                        pendingConnectPeripheralID: nil, jitterFraction: 0),
                       .endScan(.cancelled))
        XCTAssertFalse(ladder.isArmed)
    }

    /// The ORDINARY arming is untouched: a ladder that is a backstop
    /// behind a pending connect is stale the moment that connect is not
    /// the one outstanding (already reconnected, or superseded by a loss
    /// on a different peripheral).
    func testTheOrdinaryLadderStillCancelsWithoutItsPendingConnect() {
        var ladder = ReconnectLadder()
        ladder.arm(target: target, disconnectedAt: t0, jitterFraction: 0)
        XCTAssertTrue(ladder.requiresPendingConnect, "the default is unchanged")

        XCTAssertEqual(ladder.evaluate(now: at(20), shouldAutoReconnect: true,
                                        pendingConnectPeripheralID: nil, jitterFraction: 0),
                       .doNothing)
        XCTAssertFalse(ladder.isArmed, "no pending connect means this backstop is stale")

        ladder.arm(target: target, disconnectedAt: t0, jitterFraction: 0)
        XCTAssertEqual(ladder.evaluate(now: at(20), shouldAutoReconnect: true,
                                        pendingConnectPeripheralID: other, jitterFraction: 0),
                       .doNothing)
        XCTAssertFalse(ladder.isArmed, "superseded by a loss on a different peripheral")
    }

    /// Re-arming the SAME loss with a different mode is not the same
    /// arming: `arm` is idempotent per loss (two callbacks for one drop
    /// must not restart the clock), and that idempotence must not
    /// silently swallow the mode change §3.5 depends on.
    func testReArmingWithADifferentModeTakesEffect() {
        var ladder = ReconnectLadder()
        ladder.arm(target: target, disconnectedAt: t0, jitterFraction: 0)
        XCTAssertTrue(ladder.requiresPendingConnect)
        ladder.arm(target: target, disconnectedAt: t0, requiresPendingConnect: false, jitterFraction: 0)
        XCTAssertFalse(ladder.requiresPendingConnect)
        // Still idempotent for a genuinely identical re-arm.
        ladder.arm(target: target, disconnectedAt: t0, requiresPendingConnect: false, jitterFraction: 0)
        XCTAssertEqual(ladder.attempt, 1)
    }

    // MARK: - A03 §3.6 amendment (2026-09-14) — an EXPECTED post-commit
    // reboot reconnects promptly; everything else still climbs the
    // ladder.
    //
    // The bench run this comes from (2026-09-14, Mac bench app against
    // Heltec `TAY_06b0`, fw 2.7.26): `applyChannelSet` wrote the crew
    // channel, `commit_edit_settings` rebooted the radio, and the app
    // then sat through the ladder's first rung — "first window in 20.0s
    // (±20%)" — before opening the scan that found the puck again. The
    // puck had finished booting long before that.

    /// (a) A disconnect the client SAW COMING opens its rediscovery
    /// window immediately — no 20 s rung, no jitter on zero.
    func testExpectedPostCommitRebootOpensItsWindowWithNoLadderDelay() {
        var ladder = ReconnectLadder()
        ladder.armExpectingReboot(target: target, disconnectedAt: t0)

        XCTAssertTrue(ladder.isExpectedReboot)
        XCTAssertEqual(evaluate(&ladder, at: t0), .startScan(attempt: 1),
                       "a commit-driven reboot must not wait out a rung sized for an unexplained loss")
        XCTAssertTrue(ladder.isScanning)
        // And the window is the SAME bounded 30 s window — the prompt
        // path buys an earlier start, never an unbounded scan.
        XCTAssertEqual(evaluate(&ladder, at: at(29)), .doNothing)
        XCTAssertEqual(evaluate(&ladder, at: at(30)), .endScan(.windowElapsed))
    }

    /// (d) The mirror image, and the reason the marker is one-shot: an
    /// ordinary, unexplained drop still waits out §3.6's first rung.
    /// Same ladder type, same evaluation, only the arming differs.
    func testAnUnexpectedDisconnectStillWaitsOutTheLaddersFirstRung() {
        var ladder = ReconnectLadder()
        ladder.arm(target: target, disconnectedAt: t0, jitterFraction: 0)

        XCTAssertFalse(ladder.isExpectedReboot)
        XCTAssertEqual(evaluate(&ladder, at: t0), .doNothing)
        XCTAssertEqual(evaluate(&ladder, at: at(19)), .doNothing)
        XCTAssertEqual(evaluate(&ladder, at: at(20)), .startScan(attempt: 1))
    }

    /// The duty-cycle bound A03 §4.2 budgets for is untouched: only the
    /// FIRST rung is skipped. If the immediate window finds nothing, the
    /// ladder climbs the ordinary table from attempt 2 and caps exactly
    /// where it always did.
    func testAfterTheImmediateWindowTheOrdinaryTableResumes() {
        var ladder = ReconnectLadder()
        ladder.armExpectingReboot(target: target, disconnectedAt: t0)
        XCTAssertEqual(evaluate(&ladder, at: t0), .startScan(attempt: 1))
        XCTAssertEqual(evaluate(&ladder, at: at(30)), .endScan(.windowElapsed))
        XCTAssertEqual(ladder.attempt, 2)
        // Attempt 2 is 60 s (jitter 0 here), exactly as for any other
        // ladder — not another immediate window.
        XCTAssertEqual(evaluate(&ladder, at: at(59)), .doNothing)
        XCTAssertEqual(evaluate(&ladder, at: at(90)), .startScan(attempt: 2))
    }

    /// Re-arming ordinarily over an expected-reboot ladder takes effect
    /// — the idempotence guard must not swallow a mode change, which is
    /// the same bug `testReArmingWithADifferentModeTakesEffect` pins for
    /// `requiresPendingConnect`.
    func testAnOrdinaryReArmOverAnExpectedRebootLadderTakesEffect() {
        var ladder = ReconnectLadder()
        ladder.armExpectingReboot(target: target, disconnectedAt: t0)
        XCTAssertTrue(ladder.isExpectedReboot)

        ladder.arm(target: target, disconnectedAt: t0, jitterFraction: 0)
        XCTAssertFalse(ladder.isExpectedReboot)
        XCTAssertEqual(evaluate(&ladder, at: t0), .doNothing, "back to the ordinary first rung")
        XCTAssertEqual(evaluate(&ladder, at: at(20)), .startScan(attempt: 1))
    }

    /// A cancelled ladder forgets it was ever an expected reboot.
    func testCancellingClearsTheExpectedRebootMode() {
        var ladder = ReconnectLadder()
        ladder.armExpectingReboot(target: target, disconnectedAt: t0)
        ladder.cancel()
        XCTAssertFalse(ladder.isExpectedReboot)
        XCTAssertFalse(ladder.isArmed)
    }

    // MARK: - ExpectedRebootWindow — one commit explains one disconnect

    /// One-shot: a second disconnect after the same commit is an
    /// ordinary loss and gets the ordinary ladder.
    func testAnExpectedRebootNoticeIsConsumedExactlyOnce() {
        var window = ExpectedRebootWindow()
        window.arm(at: t0)
        XCTAssertTrue(window.isArmed)
        XCTAssertTrue(window.consume(now: at(1)))
        XCTAssertFalse(window.consume(now: at(2)), "one commit, one expected disconnect")
        XCTAssertFalse(window.isArmed)
    }

    /// Time-bounded: a commit whose reboot never came must not silently
    /// re-label some unrelated drop minutes later as expected.
    func testAStaleExpectedRebootNoticeIsNotHonoured() {
        var window = ExpectedRebootWindow()
        window.arm(at: t0)
        XCTAssertFalse(window.consume(now: at(ExpectedRebootWindow.defaultLifetime + 0.5)))
        XCTAssertFalse(window.isArmed, "a stale notice is spent, not left standing for the next drop")

        window.arm(at: t0)
        XCTAssertTrue(window.consume(now: at(ExpectedRebootWindow.defaultLifetime)),
                      "the boundary itself is inside the window")
    }

    /// Never armed, or explicitly stood down (a user disconnect,
    /// Bluetooth off): nothing is expected.
    func testAnUnarmedOrCancelledWindowNeverReportsAnExpectedReboot() {
        var window = ExpectedRebootWindow()
        XCTAssertFalse(window.consume(now: t0))
        window.arm(at: t0)
        window.cancel()
        XCTAssertFalse(window.consume(now: at(1)))
    }

    /// A clock that ran backwards (an NTP correction between the commit
    /// and CoreBluetooth's own measured disconnect timestamp) is not
    /// evidence of anything — it is not treated as an expected reboot.
    func testANoticeFromTheFutureIsNotHonoured() {
        var window = ExpectedRebootWindow()
        window.arm(at: at(10))
        XCTAssertFalse(window.consume(now: t0))
    }

    // MARK: - Helpers

    /// `t0` plus a number of seconds. A helper rather than a `+`
    /// overload on `Date`: an operator declared in a test file is
    /// ambiguous against the stdlib's own `Date`/`TimeInterval`
    /// arithmetic and makes every expression that uses it unreadable in
    /// diagnostics.
    private func at(_ seconds: TimeInterval) -> Date { t0.addingTimeInterval(seconds) }

    private func evaluate(_ ladder: inout ReconnectLadder, at now: Date,
                          jitter: Double = 0) -> ReconnectLadder.Action {
        ladder.evaluate(now: now, shouldAutoReconnect: true, pendingConnectPeripheralID: target,
                         jitterFraction: jitter)
    }
}
