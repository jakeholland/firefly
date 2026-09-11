//
//  BLEHardwareTests.swift — CoreBluetooth integration tests that MUST
//  run hosted inside Firefly.app (docs/specs/A01-companion-app.md, B1).
//
//  macOS aborts (__TCC_CRASHING_DUE_TO_PRIVACY_VIOLATION__) any
//  CoreBluetooth process that is not inside a signed .app bundle
//  carrying NSBluetoothAlwaysUsageDescription and launched via
//  LaunchServices. A bare `swift test` xctest binary is none of those
//  three things — constructing a `CBCentralManager` there aborts the
//  whole test process instantly, not fails one test. So this suite
//  lives in its own app-hosted test target (`FireflyHardwareTests`,
//  host application = Firefly.app) and runs with:
//
//      xcodebuild test -scheme Firefly -destination 'platform=macOS' \
//          -only-testing:FireflyHardwareTests
//
//  Gated by FIREFLY_HARDWARE=1, same as FireflyKit's own HardwareTests
//  target, so a routine run (no board, no env var) skips cleanly rather
//  than failing or hanging. Serial and TCP hardware tests are NOT
//  affected by the TCC restriction above and stay under `swift test` in
//  app/FireflyKit/Tests/HardwareTests (slice F).
//
import XCTest

final class BLEHardwareTests: XCTestCase {

    /// Placeholder: proves the app-hosted test target itself builds,
    /// launches inside Firefly.app, and skips cleanly with no env var
    /// and no board — the thing `xcodebuild test` verifies on every
    /// machine, including one with no Heltec anywhere near it. Slice A
    /// replaces this with real CBCentralManager discovery, pairing and
    /// two-phase `want_config` tests against a Heltec V3, gated the
    /// same way.
    func testSkipsCleanlyWithoutTheHardwareFlag() throws {
        try XCTSkipUnless(
            ProcessInfo.processInfo.environment["FIREFLY_HARDWARE"] == "1",
            "set FIREFLY_HARDWARE=1 with a Heltec V3 reachable over BLE to run hardware tests")
        XCTFail("no board wiring yet — slice A replaces this placeholder with a real BLE handshake test")
    }
}
