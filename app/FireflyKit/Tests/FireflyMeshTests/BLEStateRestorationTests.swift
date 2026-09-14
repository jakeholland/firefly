//
//  BLEStateRestorationTests.swift — A03 §3.1/§3.5, pinned with no radio.
//
//  `FireflyMesh` is imported WITHOUT `@testable`, exactly as
//  `BLEContractTests` and `BLEReconnectLadderTests` do, and for the
//  reason A03_AC2 itself calls out: constructing a `CBCentralManager`
//  outside a signed `.app` bundle ABORTS the process (`BLETransport
//  .swift`'s own header, B1), so a rule only reachable through a live
//  manager is a rule no unit test can ever check. Every decision this
//  file exercises is therefore a pure function of its inputs, and the
//  glue that calls them is what §6's iPhone protocol measures.
//
import CoreBluetooth
import FireflyMesh
import XCTest

final class BLEStateRestorationTests: XCTestCase {

    private let known = UUID(uuidString: "5C2A1E40-0001-11D3-9A0C-0305E82C3301")!
    private let stranger = UUID(uuidString: "5C2A1E40-0002-11D3-9A0C-0305E82C3302")!

    // MARK: - A03_AC2 — the central-manager options

    /// A03_AC2: the iOS options carry a FIXED restore identifier and
    /// `ShowPowerAlert: false`; the macOS options carry neither.
    ///
    /// The restore identifier is the whole mechanism: Apple's own
    /// requirement is that it "must be identical across executions of
    /// the app" (§1.2), so a literal that drifts is a restoration that
    /// silently stops working with no error anywhere.
    func testA03_AC2_CentralManagerOptionsCarryTheFixedRestoreIdentifierAndNoPowerAlert() {
        let options = BLETransport.centralManagerOptions
        #if os(iOS)
        XCTAssertEqual(options[CBCentralManagerOptionRestoreIdentifierKey] as? String,
                       "com.jakeholland.Firefly.ble-central",
                       "the restore identifier must be identical across executions of the app (§1.2)")
        XCTAssertEqual(options[CBCentralManagerOptionRestoreIdentifierKey] as? String,
                       BLETransport.restoreIdentifier)
        XCTAssertEqual(options[CBCentralManagerOptionShowPowerAlertKey] as? Bool, false,
                       "§3.5: the system power alert fights our own Connect-screen wording")
        #else
        XCTAssertNil(options[CBCentralManagerOptionRestoreIdentifierKey],
                     "macOS apps are not relaunched in the background; there is nothing to restore INTO")
        XCTAssertNil(options[CBCentralManagerOptionShowPowerAlertKey])
        XCTAssertTrue(options.isEmpty, "an empty dict is the honest 'nothing extra requested' default")
        #endif
    }

    /// A03_AC1's authorization gate, as its own rule: constructing the
    /// manager is what raises the system Bluetooth prompt, so a LAUNCH
    /// must not do it before the user has ever been asked. Every other
    /// authorization value is a launch that already has an answer, and
    /// therefore possibly a session to restore.
    func testA03_AC1_LaunchNeverConstructsAManagerWhileAuthorizationIsNotDetermined() {
        XCTAssertFalse(BLETransport.shouldConstructCentralManagerAtLaunch(authorization: .notDetermined))
        XCTAssertTrue(BLETransport.shouldConstructCentralManagerAtLaunch(authorization: .allowedAlways))
        XCTAssertTrue(BLETransport.shouldConstructCentralManagerAtLaunch(authorization: .denied))
        XCTAssertTrue(BLETransport.shouldConstructCentralManagerAtLaunch(authorization: .restricted))
    }

    // MARK: - A03_AC4 — §3.5's power-state table, every row

    /// A03_AC4: every `CBManagerState` returns exactly §3.5's row.
    func testA03_AC4_PowerStateActionIsExactlySection3_5sTable() {
        // .poweredOn, nothing pending, everything we need: reconnect.
        XCTAssertEqual(
            BLETransport.powerStateAction(for: .poweredOn, shouldAutoReconnect: true,
                                          hasPreferred: true, restorePending: false),
            .retrieveAndConnect,
            "§3.5: retrievePeripherals(withIdentifiers:) then a pending connect — never a scan first")

        // .poweredOn with auto-reconnect off: the user disconnected, and
        // Bluetooth coming back is not them asking to reconnect.
        XCTAssertEqual(
            BLETransport.powerStateAction(for: .poweredOn, shouldAutoReconnect: false,
                                          hasPreferred: true, restorePending: false),
            .doNothing)

        // .poweredOn with nothing remembered: there is nothing to
        // reconnect TO, and connecting to whatever Meshtastic node is
        // advertising would be connecting to a stranger's radio.
        XCTAssertEqual(
            BLETransport.powerStateAction(for: .poweredOn, shouldAutoReconnect: true,
                                          hasPreferred: false, restorePending: false),
            .doNothing)

        XCTAssertEqual(
            BLETransport.powerStateAction(for: .poweredOff, shouldAutoReconnect: true,
                                          hasPreferred: true, restorePending: false),
            .bluetoothOff)
        XCTAssertEqual(
            BLETransport.powerStateAction(for: .resetting, shouldAutoReconnect: true,
                                          hasPreferred: true, restorePending: false),
            .transientLoss,
            "§3.5: .resetting is a transient loss, not a terminal one")
        XCTAssertEqual(
            BLETransport.powerStateAction(for: .unauthorized, shouldAutoReconnect: true,
                                          hasPreferred: true, restorePending: false),
            .unauthorized)
        XCTAssertEqual(
            BLETransport.powerStateAction(for: .unsupported, shouldAutoReconnect: true,
                                          hasPreferred: true, restorePending: false),
            .unsupported)
        XCTAssertEqual(
            BLETransport.powerStateAction(for: .unknown, shouldAutoReconnect: true,
                                          hasPreferred: true, restorePending: false),
            .doNothing,
            ".unknown says nothing yet, and saying nothing is the honest answer")
    }

    /// A03_AC4, the ordering half (§3.1): `.poweredOn` with a restore
    /// pending is "do nothing" for EVERY combination of the other two
    /// inputs — including the one that would otherwise reconnect, which
    /// is the whole point. `retrievePeripherals` hands back a DIFFERENT
    /// `CBPeripheral` instance than the restore dictionary's, and
    /// releasing the restored object implicitly cancels the very
    /// connection the restore was adopting (§1.2).
    func testA03_AC4_PoweredOnDoesNothingWhileARestoreIsPending() {
        for autoReconnect in [true, false] {
            for hasPreferred in [true, false] {
                XCTAssertEqual(
                    BLETransport.powerStateAction(for: .poweredOn, shouldAutoReconnect: autoReconnect,
                                                  hasPreferred: hasPreferred, restorePending: true),
                    .doNothing,
                    "restorePending must win over (autoReconnect: \(autoReconnect), hasPreferred: \(hasPreferred))")
            }
        }
    }

    /// A03_AC4's other explicit clause: `.poweredOff` never clears
    /// `shouldAutoReconnect`. The function cannot — it returns a value —
    /// but the row it returns must be the one whose documented side
    /// effects preserve the flag, and it must be the SAME row whether or
    /// not a restore was pending: Bluetooth going off invalidates
    /// whatever was being restored anyway.
    func testA03_AC4_PoweredOffIsTheSameRowRegardlessOfTheOtherInputs() {
        for autoReconnect in [true, false] {
            for hasPreferred in [true, false] {
                for restorePending in [true, false] {
                    XCTAssertEqual(
                        BLETransport.powerStateAction(for: .poweredOff, shouldAutoReconnect: autoReconnect,
                                                      hasPreferred: hasPreferred, restorePending: restorePending),
                        .bluetoothOff)
                }
            }
        }
    }

    /// The reasons §3.5 publishes are distinct strings, because §3.10's
    /// status line says different sentences for them ("Bluetooth is off.
    /// Turn it on to reach your puck." vs "Firefly can't use Bluetooth.
    /// Turn it on in Settings."). A single generic failure reason cannot
    /// produce two different sentences.
    func testSection3_5LinkReasonsAreDistinct() {
        let reasons = [
            BLETransport.bluetoothOffReason,
            BLETransport.bluetoothResettingReason,
            BLETransport.bluetoothUnauthorizedReason,
            BLETransport.bluetoothUnsupportedReason,
            BLETransport.systemReconnectingReason,
        ]
        XCTAssertEqual(Set(reasons).count, reasons.count, "a reason that collides with another is not a reason")
        XCTAssertFalse(reasons.contains(where: \.isEmpty))
    }

    // MARK: - A03_AC1/§3.1 — the restoration decision table

    /// §3.1's branch, over every `CBPeripheralState` there is: the
    /// Meshtastic-Apple pattern is to branch on the restored
    /// peripheral's own state rather than assume one, and each branch is
    /// a genuinely different action.
    func testRestoreActionBranchesOnEveryPeripheralState() {
        XCTAssertEqual(BLERestoreAction.action(forPeripheralState: .connected), .adoptConnected,
                       "already connected at the GATT level: rediscover services, never a second connect")
        XCTAssertEqual(BLERestoreAction.action(forPeripheralState: .connecting), .keepPendingConnect,
                       "a pending connect from before the relaunch is still live (§1.3)")
        XCTAssertEqual(BLERestoreAction.action(forPeripheralState: .disconnected), .reconnect)
        XCTAssertEqual(BLERestoreAction.action(forPeripheralState: .disconnecting), .reconnect)
    }

    /// Which restored peripheral is adopted: the remembered one wins
    /// wherever it appears in the dictionary — not "the first one", and
    /// not "the only one", because the restore dictionary can carry more
    /// than one and the one we care about need not be first.
    func testTheRememberedPeripheralIsTheOneRestored() {
        XCTAssertEqual(
            BLETransport.indexOfPeripheralToRestore(identifiers: [stranger, known], preferred: known), 1)
        XCTAssertEqual(
            BLETransport.indexOfPeripheralToRestore(identifiers: [known, stranger], preferred: known), 0)
    }

    /// …and with nothing remembered, or a remembered id that is not in
    /// the dictionary at all, the first entry is adopted rather than the
    /// restore being dropped: iOS handed us a live session, and a
    /// session we refuse to adopt is one we have silently torn down
    /// (§1.2 — releasing the peripheral cancels its connection).
    func testAnUnmatchedPreferredIdStillAdoptsTheRestoredSession() {
        XCTAssertEqual(BLETransport.indexOfPeripheralToRestore(identifiers: [stranger], preferred: known), 0)
        XCTAssertEqual(BLETransport.indexOfPeripheralToRestore(identifiers: [stranger], preferred: nil), 0)
    }

    /// An empty dictionary is the one case with nothing to do. The
    /// dictionary "can be sparse" (§1.2), so this is a real arrival, not
    /// a theoretical one.
    func testAnEmptyRestoreDictionaryRestoresNothing() {
        XCTAssertNil(BLETransport.indexOfPeripheralToRestore(identifiers: [], preferred: known))
        XCTAssertNil(BLETransport.indexOfPeripheralToRestore(identifiers: [], preferred: nil))
    }
}
