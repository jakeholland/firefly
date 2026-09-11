//
//  BLEContractTests.swift — the borrowed BLE facts, pinned.
//
//  These constants and rules are Meshtastic's, cross-checked against
//  Meshtastic-Apple and this repo's archived iOS app; see
//  docs/specs/A01-companion-app.md's "Reuse assessment". Pinning them in
//  a test means a well-meaning edit has to argue with a failing build
//  rather than silently break pairing on a bench board.
//
import FireflyMesh
import XCTest

final class BLEContractTests: XCTestCase {

    func testGATTUUIDs() {
        XCTAssertEqual(MeshtasticBLE.serviceUUIDString, "6BA1B218-15A8-461F-9FA8-5DCAE273EAFD")
        XCTAssertEqual(MeshtasticBLE.toRadioUUIDString, "F75C76D2-129E-4DAD-A1DD-7866124401E7")
        XCTAssertEqual(MeshtasticBLE.fromRadioUUIDString, "2C55E69E-4993-11ED-B878-0242AC120002")
        XCTAssertEqual(MeshtasticBLE.fromNumUUIDString, "ED9DA18C-A800-4F66-A670-AA7547E34453")
    }

    /// An empty FROMRADIO read is the ONLY end-of-queue signal there is.
    func testEmptyReadIsTheDrainTerminator() {
        XCTAssertTrue(FromRadioDrainPolicy.isQueueDrained(read: Data()))
        XCTAssertFalse(FromRadioDrainPolicy.isQueueDrained(read: Data([0x00])))
    }

    /// All three drain triggers must stay enumerated. Dropping the
    /// post-write one is the subtle bug: the radio can queue a reply
    /// before the FROMNUM notification lands, and the app then waits
    /// forever for a nudge that already happened.
    func testAllThreeDrainTriggersArePresent() {
        XCTAssertEqual(Set(FromRadioDrainPolicy.Trigger.allCases),
                       [.subscriptionAcknowledged, .fromNumNotification, .toRadioWriteCompleted])
    }

    // MARK: - M2: peripheral-persistence closures (no CoreBluetooth
    // involved — merely constructing `BLETransport()` is safe anywhere,
    // per its own file-level doc comment and
    // `DemoRunnerTests.testLiveDependenciesNeverConstructTheDemoClient`,
    // which already does exactly that via `AppDependencies.live()`;
    // only `connect()`/`scan()` ever touch a real `CBCentralManager`,
    // and neither is called here).

    /// `AppDependencies.live()`'s composition-root wiring
    /// (`onPreferredPeripheralChanged`/`onBonded`) is a thin pass-through
    /// onto these two calls — this pins the calls themselves, which is
    /// where `SettingsKey.lastPeripheralID`/`.bondedPeripheralIDs`
    /// actually get kept current (docs/specs/A01-companion-app.md, M2:
    /// "remembering the last connected peripheral identifier").
    func testSetPreferredPeripheralFiresThePersistenceClosure() async {
        let id = UUID()
        let changed = Locked<UUID?>(nil)
        let transport = BLETransport(onPreferredPeripheralChanged: { changed.value = $0 })

        await transport.setPreferredPeripheral(id)

        XCTAssertEqual(changed.value, id)
        let stored = await transport.preferredPeripheralID
        XCTAssertEqual(stored, id)
    }

    /// Setting `nil` (e.g. "forget this node") must not fire the
    /// closure with a bogus value — there is nothing to persist.
    func testSetPreferredPeripheralToNilDoesNotFireTheClosure() async {
        let calls = Locked<Int>(0)
        let transport = BLETransport(onPreferredPeripheralChanged: { _ in calls.value += 1 })

        await transport.setPreferredPeripheral(nil)

        XCTAssertEqual(calls.value, 0)
    }

    // MARK: - SHOULD-FIX 4 (PR #272 review): connect-pending guard
    //
    // `BLETransport.performConnectSequence()` can no longer be exercised
    // with a real `CBCentralManager` in `swift test` (that would abort
    // the process — this type's own file-level doc comment), so this
    // pins the PURE decision the guard is built on
    // (`shouldIssueConnect(for:pendingConnectPeripheralID:)`) rather
    // than the CoreBluetooth call site itself: whether a NEW native
    // `central.connect()` should be issued for a peripheral, given
    // whichever identifier (if any) already has one outstanding — the
    // guard `issueConnect(_:)` uses at all three call sites that can
    // arm a connect (an explicit `connect()`'s own
    // `performConnectSequence()`, `handleDisconnected`'s
    // reconnect-on-loss re-arm, `handleWillRestoreState`'s re-arm).

    func testShouldIssueConnectWhenNothingIsPending() {
        let target = UUID()
        XCTAssertTrue(BLETransport.shouldIssueConnect(for: target, pendingConnectPeripheralID: nil))
    }

    func testShouldNotIssueConnectWhenTheSamePeripheralIsAlreadyPending() {
        let target = UUID()
        XCTAssertFalse(BLETransport.shouldIssueConnect(for: target, pendingConnectPeripheralID: target),
                        "a restore or a reconnect-on-loss re-arm already has this exact peripheral pending — " +
                        "an explicit connect() must not redundantly issue a second native central.connect()")
    }

    func testShouldIssueConnectForADifferentPeripheralEvenWhileAnotherIsPending() {
        let pending = UUID()
        let other = UUID()
        XCTAssertTrue(BLETransport.shouldIssueConnect(for: other, pendingConnectPeripheralID: pending),
                       "a pending connect for a DIFFERENT identifier must never suppress a fresh one")
    }

    // MARK: - Reconnect-fallback scan (2026-09-11 bench power-cycle
    // failure): the pure decision `armReconnectFallback(for:)`'s own
    // timer callback is built on, tested with no `CBCentralManager` at
    // all for the same reason `shouldIssueConnect` above is.

    func testShouldRunReconnectFallbackScanWhenStillPendingForTheSameTarget() {
        let target = UUID()
        XCTAssertTrue(BLETransport.shouldRunReconnectFallbackScan(
            for: target, pendingConnectPeripheralID: target, shouldAutoReconnect: true))
    }

    func testShouldNotRunReconnectFallbackScanOnceAlreadyReconnected() {
        let target = UUID()
        XCTAssertFalse(BLETransport.shouldRunReconnectFallbackScan(
            for: target, pendingConnectPeripheralID: nil, shouldAutoReconnect: true),
            "completeConnect(throwing:) clears pendingConnectPeripheralID unconditionally on success — " +
            "a fallback timer firing late after a fast reconnect must be a no-op")
    }

    func testShouldNotRunReconnectFallbackScanWhenSupersededByANewerDisconnect() {
        let staleTarget = UUID()
        let newTarget = UUID()
        XCTAssertFalse(BLETransport.shouldRunReconnectFallbackScan(
            for: staleTarget, pendingConnectPeripheralID: newTarget, shouldAutoReconnect: true),
            "a fallback armed for an OLDER disconnect must never scan for a target a newer one already replaced")
    }

    func testShouldNotRunReconnectFallbackScanAfterAnExplicitDisconnect() {
        let target = UUID()
        XCTAssertFalse(BLETransport.shouldRunReconnectFallbackScan(
            for: target, pendingConnectPeripheralID: target, shouldAutoReconnect: false),
            "disconnect() clears shouldAutoReconnect — a fallback timer outliving it must never start a scan " +
            "for a peripheral the user asked to leave")
    }

    func testMarkBondedFiresThePersistenceClosureAndRecordsTheID() async {
        let id = UUID()
        let bonded = Locked<[UUID]>([])
        let transport = BLETransport(onBonded: { bonded.value.append($0) })

        await transport.markBonded(id)

        XCTAssertEqual(bonded.value, [id])
        let stored = await transport.bondedPeripheralIDs
        XCTAssertEqual(stored, [id])
    }

    /// A test-local lock box — `NSLock`-backed, same shape as
    /// `FireflyMesh.LockedValue`, kept private to this file rather than
    /// widening that internal type's access level just for a test.
    private final class Locked<Value>: @unchecked Sendable {
        private let lock = NSLock()
        private var storage: Value
        init(_ initial: Value) { storage = initial }
        var value: Value {
            get { lock.lock(); defer { lock.unlock() }; return storage }
            set { lock.lock(); defer { lock.unlock() }; storage = newValue }
        }
    }
}
