//
//  AdminReadSerialHardwareTests.swift — the real hardware regression
//  rig for the `get_channel_request` index+1 bug (fix/app-get-channel-
//  request-index): confirms `MeshtasticClient.currentChannel(index:)`,
//  `currentChannelTable()`, and `currentLoRaConfig()` all answer
//  promptly against a real board, where the pre-fix code (bare
//  `UInt32(index)`) hung `currentChannel(index: 0)` and
//  `currentChannelTable()` until `sendAdminRequest`'s 30s timeout
//  (confirmed against this exact board before the fix — see the PR
//  body for the verbatim before/after timings).
//
//  Gated by TWO env vars, BOTH required, and skips CLEANLY (not
//  failing, not hanging) without them — a routine `swift test` on a
//  laptop with nothing plugged in stays green:
//
//    FIREFLY_HARDWARE=1 FIREFLY_SERIAL_PORT=/dev/cu.usbserial-0001 \
//      swift test --filter AdminReadSerialHardwareTests
//
//  BOARD SAFETY: this suite is READ-ONLY by construction — every call
//  here is a `get_*_request` (`currentChannel`, `currentChannelTable`,
//  `currentLoRaConfig`); none of `MeshtasticClient`'s write paths
//  (`applyChannelSet`, `setOwner`, `setRegion`, ...) are exercised.
//  Modeled directly on `AdminWriteHardwareTests`'s gating pattern and
//  the scratch probe this task started from
//  (`/private/tmp/claude-501/bench-serial/app/FireflyKit/Tests/
//  HardwareTests/ScratchAdminSerialTests.swift`), but assertive rather
//  than print-only, and deliberately does NOT assert the board's owner
//  name/short name the way `SerialHardwareTests.Bench` does — this
//  board has since been renamed ("Taylor"/"TAY"), which is out of scope
//  for this PR; run this suite with `--filter AdminReadSerialHardwareTests`
//  specifically, not a bare `--filter Hardware`, to avoid also running
//  (and failing on) `SerialHardwareTests`'s now-stale owner-name
//  assertions.
//
import FireflyMesh
import MeshtasticProto
import XCTest

final class AdminReadSerialHardwareTests: XCTestCase {

    /// This bench Heltec's own identity (docs/hardware/heltec-v3.md /
    /// `!02e606b0`), confirmed read-only — same pattern as
    /// `AdminWriteHardwareTests.expectedMyNodeNum` and
    /// `SerialHardwareTests.Bench.nodeNum`, just against the board this
    /// bug was reproduced and fixed against ("Firefly 1", now renamed
    /// "Taylor" — node num is unaffected by the rename).
    private static let expectedMyNodeNum: UInt32 = 48_629_424

    private func requireHardware() throws -> String {
        guard ProcessInfo.processInfo.environment["FIREFLY_HARDWARE"] == "1" else {
            throw XCTSkip("set FIREFLY_HARDWARE=1 (+ FIREFLY_SERIAL_PORT) to run against a real board")
        }
        guard let port = ProcessInfo.processInfo.environment["FIREFLY_SERIAL_PORT"], !port.isEmpty else {
            throw XCTSkip("FIREFLY_HARDWARE=1 but FIREFLY_SERIAL_PORT is unset")
        }
        return port
    }

    /// The headline regression check: `currentChannel(index: 0)` must
    /// return quickly (well under `sendAdminRequest`'s 30s timeout —
    /// asserted here at 2s, generous for a single reliable admin
    /// round-trip over serial) and report the real primary channel.
    /// Before this fix, this call sent `get_channel_request = 0` (the
    /// bare index) and hung the full 30s because the real `AdminModule`
    /// never answers that value.
    func testCurrentChannelIndexZeroAnswersQuicklyWithThePrimaryChannel() async throws {
        let port = try requireHardware()
        let client = MeshtasticClient(transport: SerialTransport(path: port))
        try await client.connect()
        defer { Task { await client.disconnect() } }

        let myNodeNum = await client.currentMyNodeNum
        XCTAssertEqual(myNodeNum, Self.expectedMyNodeNum, "connected to an unexpected board — check FIREFLY_SERIAL_PORT")

        let start = Date()
        let channel = try await client.currentChannel(index: 0)
        let elapsed = Date().timeIntervalSince(start)
        print("HW currentChannel(0) ok in \(elapsed)s name=\(channel.settings.name) role=\(channel.role)")

        XCTAssertLessThan(elapsed, 2.0, "currentChannel(index: 0) took \(elapsed)s — should be a single fast admin round-trip, not anywhere near the 30s pre-fix timeout")
        XCTAssertEqual(channel.index, 0)
        XCTAssertEqual(channel.role, .primary)
        XCTAssertEqual(channel.settings.name, "Firefly")
    }

    /// `currentChannelTable()` walks every index `0..<maxChannelSlots`
    /// with one `get_channel_request` each — before this fix, EVERY one
    /// of those per-index reads timed out (index 0 hung outright;
    /// indexes >= 1 asked for the wrong slot even when they didn't
    /// time out). Confirms the full table comes back with correct,
    /// 0-based indexes and the primary channel included.
    func testCurrentChannelTableReturnsOccupiedSlotsWithCorrectZeroBasedIndexes() async throws {
        let port = try requireHardware()
        let client = MeshtasticClient(transport: SerialTransport(path: port))
        try await client.connect()
        defer { Task { await client.disconnect() } }

        let myNodeNum = await client.currentMyNodeNum
        XCTAssertEqual(myNodeNum, Self.expectedMyNodeNum, "connected to an unexpected board — check FIREFLY_SERIAL_PORT")

        let start = Date()
        let table = try await client.currentChannelTable()
        let elapsed = Date().timeIntervalSince(start)
        print("HW currentChannelTable() ok in \(elapsed)s count=\(table.count) entries=\(table.map { "\($0.index):\($0.settings.name):\($0.role)" })")

        XCTAssertLessThan(elapsed, Double(maxChannelSlots) * 2.0, "currentChannelTable() took \(elapsed)s across \(maxChannelSlots) slots — should be well under the pre-fix per-slot 30s timeout")
        XCTAssertFalse(table.isEmpty, "expected at least the primary channel")
        XCTAssertTrue(table.contains { $0.index == 0 && $0.role == .primary && $0.settings.name == "Firefly" })
        for entry in table {
            XCTAssertTrue((0..<maxChannelSlots).contains(entry.index), "channel table entry index \(entry.index) out of range")
        }
    }

    /// `currentLoRaConfig()` doesn't touch `get_channel_request` at
    /// all, so it was never broken by this bug — included as the
    /// control this task's own bench probe used
    /// (`ScratchAdminSerialTests`: "ok in 0.45s" pre-fix), to prove the
    /// serial link and admin round-trip mechanism are healthy
    /// independent of the fix under test.
    func testCurrentLoRaConfigAnswersQuickly() async throws {
        let port = try requireHardware()
        let client = MeshtasticClient(transport: SerialTransport(path: port))
        try await client.connect()
        defer { Task { await client.disconnect() } }

        let start = Date()
        let lora = try await client.currentLoRaConfig()
        let elapsed = Date().timeIntervalSince(start)
        print("HW currentLoRaConfig() ok in \(elapsed)s region=\(lora.region)")

        XCTAssertLessThan(elapsed, 2.0)
    }
}
