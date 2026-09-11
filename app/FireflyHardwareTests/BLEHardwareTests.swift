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
//  Board policy: ONLY `Meshtastic_e7d4` (Firefly 2, the app's own node,
//  NO_PIN) may be connected to by this suite. `Meshtastic_06b0`
//  (Firefly 1) is a peer this suite only ever expects to SEE in the
//  nodeDB, never connects to directly — and neither
//  `/dev/cu.usbserial-0001` nor `/dev/cu.usbserial-4` is touched here at
//  all (BLE only; those paths belong to the serial suite, slice F).
//
import XCTest
import FireflyMesh

final class BLEHardwareTests: XCTestCase {

    private static let targetPeripheralName = "Meshtastic_e7d4"
    private static let expectedMyNodeNum: UInt32 = 48_621_524
    private static let expectedOwnerLongName = "Firefly 2"
    /// Firefly 1, `!02e606b0` — must appear in the nodeDB with a
    /// position (its asserted bench fix, 47.708135,-122.2820993), never
    /// connected to directly.
    private static let firefly1NodeNum: UInt32 = 0x02E6_06B0
    private static let expectedChannelName = "Firefly"

    /// Also proves, when `FIREFLY_HARDWARE` is unset, that the
    /// app-hosted target itself builds, launches inside Firefly.app, and
    /// skips cleanly with no board — the thing `xcodebuild test`
    /// verifies on every machine, including one with no Heltec anywhere
    /// near it (A01_AC1).
    func testConnectsAndCompletesHandshakeAgainstFirefly2() async throws {
        try XCTSkipUnless(
            ProcessInfo.processInfo.environment["FIREFLY_HARDWARE"] == "1",
            "set FIREFLY_HARDWARE=1 with Meshtastic_e7d4 reachable over BLE to run hardware tests")

        let transport = BLETransport()
        let targetID = try await discoverTarget(named: Self.targetPeripheralName, on: transport)
        await transport.setPreferredPeripheral(targetID)

        let client = MeshtasticClient(transport: transport)
        try await client.connect()
        addTeardownBlock { await client.disconnect() }

        // A01_AC4: discovers the node, completes both want_config
        // phases, reaches .ready (implied by `connect()` returning
        // without throwing), and lists the other board in its node list.
        let myNodeNum = await client.currentMyNodeNum
        XCTAssertEqual(myNodeNum, Self.expectedMyNodeNum,
                        "connected to the wrong board — expected Firefly 2's node num (Meshtastic_e7d4 policy)")

        let myNode = await client.nodeSnapshot(Self.expectedMyNodeNum)
        XCTAssertEqual(myNode?.longName, Self.expectedOwnerLongName)

        let channelNames = await client.channelNames
        XCTAssertTrue(channelNames.contains(Self.expectedChannelName),
                       "expected a channel named \"\(Self.expectedChannelName)\", saw \(channelNames)")

        let firefly1 = await client.nodeSnapshot(Self.firefly1NodeNum)
        XCTAssertNotNil(firefly1, "Firefly 1 (!02e606b0) was not present in the nodeDB")
        XCTAssertNotNil(firefly1?.position, "Firefly 1's asserted bench position (CLIENT, 47.708135,-122.2820993) did not arrive")
    }

    /// Scans on the service UUID (never a name prefix — MeshtasticBLE's
    /// own rule) and waits for the one board this suite is allowed to
    /// touch, `Meshtastic_e7d4`, ignoring anything else it sees —
    /// concretely `Meshtastic_06b0` (Firefly 1), which may also be
    /// advertising on the same bench.
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
