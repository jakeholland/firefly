//
//  AdminWriteHardwareTests.swift — the real hardware rig for M3's
//  channel write-back (docs/specs/A01-companion-app.md, M3: "Channel
//  write-back (admin messages) behind an explicit confirmation").
//
//  Gated by TWO env vars, BOTH required, and skips CLEANLY (not
//  failing, not hanging) without them:
//
//    FIREFLY_HARDWARE=1 FIREFLY_ALLOW_WRITE=1 FIREFLY_SERIAL_PORT=/dev/cu.usbserial-4 \
//      swift test --filter AdminWriteHardwareTests
//
//  `FIREFLY_HARDWARE=1` alone (`SerialHardwareTests`'s own gate) is NOT
//  enough — this suite additionally sends admin writes to a real node,
//  which `SerialHardwareTests` deliberately never does (see that file's
//  own "BOARD SAFETY" header). `FIREFLY_ALLOW_WRITE=1` is a second,
//  explicit opt-in specifically for that: a routine `FIREFLY_HARDWARE=1`
//  CI/bench run must never accidentally start writing to a board.
//
//  DO NOT RUN THIS SUITE. It is added, gated, and left unrun by design
//  — the task that added it says so explicitly. A human who later
//  chooses to run it locally MUST point FIREFLY_SERIAL_PORT at
//  **Firefly 2** (docs/hardware/heltec-v3.md's bench table: the mobile
//  end node, fixed position already removed) — NEVER Firefly 1, which
//  carries an asserted bench position Radar testing depends on and
//  which `SerialHardwareTests` already treats as read-only for exactly
//  that reason.
//
//  BOARD SAFETY: this suite is SAFE BY CONSTRUCTION even though it
//  writes. It reads channel 0 off the connected node with
//  `MeshtasticClient.currentChannel(index:)`, then writes that EXACT
//  `Channel` — byte for byte what the node just reported, nothing
//  invented or guessed — back with `applyChannelSet`, and verifies the
//  read-back matches. Nothing about the channel's content differs
//  before and after. It never touches `setOwner`/`setRegion`
//  (those change something) and never touches Firefly 1's position.
//  The one real-world side effect is unavoidable and expected: a
//  `commit_edit_settings` reboots the node and drops Bluetooth
//  (`MeshtasticClient.commitEditSettingsBestEffort`'s own doc comment,
//  cross-checked against Meshtastic-Apple's `commitEditSettings` and
//  firmware's `AdminModule.cpp`) — the confirmation sheet's own warning
//  text says exactly this.
//
import FireflyMesh
import MeshtasticProto
import XCTest

final class AdminWriteHardwareTests: XCTestCase {

    private func requireHardwareWriteConsent() throws -> String {
        guard ProcessInfo.processInfo.environment["FIREFLY_HARDWARE"] == "1" else {
            throw XCTSkip("set FIREFLY_HARDWARE=1 (+ FIREFLY_ALLOW_WRITE=1, FIREFLY_SERIAL_PORT) to run against a real board")
        }
        guard ProcessInfo.processInfo.environment["FIREFLY_ALLOW_WRITE"] == "1" else {
            throw XCTSkip(
                "FIREFLY_HARDWARE=1 but FIREFLY_ALLOW_WRITE is unset — this suite sends real admin writes " +
                "(reboots the node); opt in explicitly and point FIREFLY_SERIAL_PORT at Firefly 2, never Firefly 1")
        }
        guard let port = ProcessInfo.processInfo.environment["FIREFLY_SERIAL_PORT"], !port.isEmpty else {
            throw XCTSkip("FIREFLY_HARDWARE=1 FIREFLY_ALLOW_WRITE=1 but FIREFLY_SERIAL_PORT is unset")
        }
        return port
    }

    /// Writes the CURRENT primary channel back to the connected node
    /// UNCHANGED — safe by construction (this file's own header) —
    /// and proves the whole begin/set_channel/commit/read-back path
    /// against a real `AdminModule` without ever changing what the
    /// board actually runs.
    func testWritingTheCurrentChannelBackUnchangedReadsBackIdentical() async throws {
        let port = try requireHardwareWriteConsent()
        let transport = SerialTransport(path: port)
        let client = MeshtasticClient(transport: transport)

        try await client.connect()
        defer { Task { await client.disconnect() } }

        guard await client.currentMyNodeNum != nil else {
            return XCTFail("handshake completed but connectedNodeNum is nil")
        }

        // Read channel 0 — whatever it actually is right now. Never
        // asserted against a hardcoded expectation: this suite does not
        // know or care what Firefly 2's channel is configured to, only
        // that writing it back must not change it.
        let before = try await client.currentChannel(index: 0)

        let report = try await client.applyChannelSet(ChannelWriteRequest(channels: [before], loraConfig: nil))

        XCTAssertEqual(report.channels, [before], "the read-back after writing channel 0 back unchanged must match what was read before the write")

        // Re-read independently (not just trusting applyChannelSet's own
        // report) as a second, redundant confirmation that the node's
        // channel 0 is unchanged.
        let after = try await client.currentChannel(index: 0)
        XCTAssertEqual(after, before, "channel 0 must be byte-for-byte unchanged after writing it back to itself")
    }
}
