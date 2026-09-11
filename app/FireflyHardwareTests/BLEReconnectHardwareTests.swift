//
//  BLEReconnectHardwareTests.swift — hardware verification for the
//  2026-09-11 bench power-cycle investigation
//  (docs/specs/A01-companion-app.md, M2): the real, root-cause failure
//  (`BLEHardwareTests.testReconnectsOnItsOwnAfterFirefly2IsPowerCycled`,
//  against Meshtastic_e7d4) needs a human at the bench to pull a
//  board's USB power — this file cannot do that, and does not attempt
//  to. What it CAN do on its own is induce a REAL, unexpected
//  CoreBluetooth-level drop (`BLETransport.simulateUnexpectedDisconnectForTesting()`,
//  `@testable import` — a local `cancelPeripheralConnection`, which
//  fires `didDisconnectPeripheral` the same way an out-of-range or
//  powered-off node would) and prove reconnect-on-loss actually re-arms
//  against a REAL radio, closing the gap
//  `ClientReconnectTests.testReconnectingIsPublishedWhileTheNodeIsStillAbsentNotOnlyAfterItReappears`
//  (mocked transport, no CoreBluetooth) cannot close on its own.
//
//  Board policy for THIS file, ONLY: `Meshtastic_06b0` (Firefly 1) —
//  the opposite of `BLEHardwareTests`' own e7d4-only policy,
//  deliberately: that suite already owns Meshtastic_e7d4 exclusively
//  (its own header comment), and this verification must never connect
//  to it or race it for the same board. Never changes board settings.
//
//  Same app-hosted requirement as `BLEHardwareTests` (this file's own
//  sibling) — macOS aborts (`__TCC_CRASHING_DUE_TO_PRIVACY_VIOLATION__`)
//  any CoreBluetooth process outside a signed `.app` bundle:
//
//      xcodebuild test -scheme Firefly -destination 'platform=macOS' \
//          -only-testing:FireflyHardwareTests/BLEReconnectHardwareTests
//
//  Gated by FIREFLY_HARDWARE=1, same as every other suite here, so a
//  routine run (no board, no env var) skips cleanly.
//
import XCTest
@testable import FireflyMesh

final class BLEReconnectHardwareTests: XCTestCase {

    private static let targetPeripheralName = "Meshtastic_06b0"

    /// 2026-09-11 bench investigation. FAILS on pre-fix `main` the same
    /// way the real bench run did: `handleDisconnected`'s reconnect-on-
    /// loss re-arm existed, but NOTHING was published between the loss
    /// and the transport fighting its way back to `.ready` — a UI (and
    /// this test) reading total silence, indistinguishable from having
    /// given up. Passes once `MeshtasticClient.consumeTransportEvents`
    /// publishes an honest `.reconnecting(attempt: 1)` the instant an
    /// unexpected loss is detected, AND `BLETransport` actually
    /// reconnects (`issueConnect(_:)` re-issuing `central.connect()`,
    /// backstopped by `armReconnectFallback(for:)`'s scan fallback if
    /// that pending connect does not complete on its own).
    func testReconnectsOnItsOwnAfterAnUnexpectedDisconnectFromFirefly1() async throws {
        try XCTSkipUnless(
            ProcessInfo.processInfo.environment["FIREFLY_HARDWARE"] == "1",
            "set FIREFLY_HARDWARE=1 with Meshtastic_06b0 reachable over BLE to run hardware tests")

        // A short fallback delay: `cancelPeripheralConnection` against a
        // board that is still physically present and advertising should
        // let CoreBluetooth's OWN pending `central.connect()` complete
        // almost immediately — this is not the "fully powered off"
        // scenario the fallback scan exists for — but arming it short
        // rather than the 20s production default keeps this test honest
        // about the backstop actually being wired up, without a slow
        // run being the ordinary case.
        let transport = BLETransport(reconnectFallbackDelay: .seconds(5))
        let targetID = try await discoverTarget(named: Self.targetPeripheralName, on: transport)
        await transport.setPreferredPeripheral(targetID)

        let client = MeshtasticClient(transport: transport)
        let states = client.linkState()
        try await client.connect()
        addTeardownBlock { await client.disconnect() }

        await transport.simulateUnexpectedDisconnectForTesting()

        var seenReconnecting = false
        var reachedReadyAgain = false
        let deadline = Date().addingTimeInterval(60)
        for await state in states {
            if case .reconnecting = state { seenReconnecting = true }
            if seenReconnecting, state == .ready { reachedReadyAgain = true; break }
            if Date() > deadline { break }
        }

        XCTAssertTrue(seenReconnecting,
                       "expected an honest .reconnecting(attempt:) after the simulated loss, not silence")
        XCTAssertTrue(reachedReadyAgain,
                       "expected the link to reach .ready again on its own within 60s of the simulated loss")
    }

    /// Scans on the service UUID (never a name prefix — MeshtasticBLE's
    /// own rule) and waits for the one board THIS file is allowed to
    /// touch, `Meshtastic_06b0`, ignoring anything else it sees —
    /// concretely `Meshtastic_e7d4` (Firefly 2), which may also be
    /// advertising on the same bench and belongs to `BLEHardwareTests`
    /// alone.
    private func discoverTarget(
        named name: String, on transport: BLETransport, timeout: Duration = .seconds(20)
    ) async throws -> UUID {
        let discoveries = await transport.scan()
        return try await withThrowingTaskGroup(of: UUID.self) { group in
            group.addTask {
                for await peripheral in discoveries where peripheral.name == name {
                    return peripheral.id
                }
                throw XCTSkip("BLE scan ended before \(name) was seen")
            }
            group.addTask {
                try await Task.sleep(for: timeout)
                throw XCTSkip("\(name) was not discovered within \(timeout) — is it powered and in range?")
            }
            guard let result = try await group.next() else {
                throw XCTSkip("no discovery result")
            }
            group.cancelAll()
            await transport.stopScanning()
            return result
        }
    }
}
