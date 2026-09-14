//
//  BLERestorationHostTests.swift — A03_AC1's **[app-host]** half.
//
//  A03_AC1 says `prepareForRestoration()` "constructs the central
//  manager exactly once, is safe to call repeatedly, never issues a
//  `connect()` or a scan, and returns with the manager already built".
//  The synchronous property is pinned by the signature itself (it is
//  `nonisolated` and non-`async`, so a caller CANNOT be handed a
//  manager later), and the decision table around it is pinned with no
//  radio at all in `BLEStateRestorationTests`. What is left — that a
//  real `CBCentralManager` is genuinely constructed, once, and that a
//  second call does not produce a second one — needs a real manager,
//  and a real manager needs a signed, LaunchServices-launched `.app`:
//  macOS aborts (`__TCC_CRASHING_DUE_TO_PRIVACY_VIOLATION__`) any
//  CoreBluetooth process that is not one (`BLEHardwareTests`' own
//  header, B1). Hence this file, hosted inside Firefly.app:
//
//      xcodebuild test -scheme Firefly -destination 'platform=macOS' \
//          -only-testing:FireflyHardwareTests/BLERestorationHostTests
//
//  Gated by FIREFLY_HARDWARE=1 like every other suite here — and here
//  the gate is doing MORE than keeping a boardless run quiet: an
//  UNSIGNED host app (CI's default, `CODE_SIGNING_ALLOWED = NO`) is
//  exactly the case TCC aborts, so this must never run there. It needs
//  no radio, only a signed local build with Bluetooth granted.
//
//  Nothing here touches a board: `prepareForRestoration()` is defined by
//  what it does NOT do.
//
import CoreBluetooth
import XCTest
@testable import FireflyMesh

final class BLERestorationHostTests: XCTestCase {

    /// **A03_AC1.** A real manager, built by the time the call returns,
    /// and exactly one of them however many times we ask.
    ///
    /// "Exactly one" is the load-bearing half: §3.1 has TWO launch paths
    /// on purpose (the `UIApplicationDelegate` and `FireflyApp.init()`),
    /// so the idempotence is not a nicety — a second `CBCentralManager`
    /// would be a second delegate, a second restore identity, and two
    /// objects racing to connect the same radio.
    func testA03_AC1_PrepareForRestorationBuildsExactlyOneManagerSynchronously() async throws {
        try XCTSkipUnless(
            ProcessInfo.processInfo.environment["FIREFLY_HARDWARE"] == "1",
            "needs a SIGNED host app with Bluetooth granted — constructing a CBCentralManager " +
            "outside one aborts the process (B1). No radio required.")
        try XCTSkipIf(
            CBCentralManager.authorization == .notDetermined,
            "authorization is .notDetermined, so §3.1 deliberately constructs nothing — " +
            "grant Bluetooth to Firefly.app once and re-run")

        let transport = BLETransport()
        let before = await transport.hasCentralManagerForTesting
        XCTAssertFalse(before, "constructing a BLETransport must not construct a manager (AppDependencies.live())")

        transport.prepareForRestoration()
        let after = await transport.hasCentralManagerForTesting
        XCTAssertTrue(after, "the manager must exist by the time the call RETURNS, not on some later executor")

        let first = await transport.centralManagerIdentityForTesting
        transport.prepareForRestoration()
        transport.prepareForRestoration()
        let second = await transport.centralManagerIdentityForTesting
        XCTAssertNotNil(first)
        XCTAssertEqual(first, second, "a second call must not produce a second CBCentralManager")

        // A03_AC1's other clause: no connect, no scan. `isScanning` is
        // CoreBluetooth's own observation of itself, not a flag of ours
        // that could simply be wrong.
        let scanning = await transport.isScanningForTesting
        XCTAssertFalse(scanning, "restoration must adopt a session, never start a scan")
        let peripheralID = await transport.currentPeripheralID
        XCTAssertNil(peripheralID, "restoration must not have picked a peripheral to connect to")

        await transport.disconnect()
    }
}
