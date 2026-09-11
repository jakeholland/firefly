//
//  SerialHardwareTests.swift — the real hardware rig for the serial
//  transport (docs/specs/A01-companion-app.md, "Test strategy" >
//  "Serial and TCP — HardwareTests, plain swift test", and Slice F).
//
//  Gated by TWO env vars, both required, and skips CLEANLY (not
//  failing, not hanging) without them — a routine `swift test` on a
//  laptop with nothing plugged in stays green:
//
//    FIREFLY_HARDWARE=1 FIREFLY_SERIAL_PORT=/dev/cu.usbserial-4 \
//      swift test --filter Hardware
//
//  Slice A's real `MeshtasticClient` is not merged as of this PR, so
//  this drives the minimal handshake directly through `SerialTransport`
//  + `StreamFramer` with hand-built `ToRadio` protobufs from
//  `MeshtasticProto` — exactly the "slice-A-agnostic" path the task
//  describes. Once slice A lands, these can be rewritten against the
//  real client; the transport and framing underneath do not change.
//
//  BOARD SAFETY, read before touching this file: this suite talks to a
//  real, shared bench board (Firefly 1, docs/hardware/heltec-v3.md's
//  "Bench boards for the phone app" table) over the ONE serial port
//  slice F is allowed to open. It only ever READS config/state off the
//  radio (want_config, node db) and sends an inert `Heartbeat` — it
//  never sends an admin message, never changes a device setting, and
//  deliberately does NOT exercise the phone-GPS `LOC_EXTERNAL` push
//  against this board: Firefly 1's whole bench role is an ASSERTED
//  fixed position for Radar testing, and pushing a measured position at
//  it risks exactly the provenance corruption
//  docs/hardware/heltec-v3.md's "Use 2" trap warns about. That push
//  path is covered by `LocationProviderTests` (no radio) instead.
//
import FireflyMesh
import MeshtasticProto
import XCTest

/// Meshtastic's own sentinels for `ToRadio.want_config_id` — NOT
/// arbitrary (docs/specs/A01-companion-app.md, "Meshtastic client" >
/// "Handshake", which cites Meshtastic-Apple's `AccessoryManager.swift`
/// and this repo's own archived app for the same two constants).
/// Defined locally here, private to this test file, rather than
/// depended on from `FireflyMesh`: slice A's real `MeshtasticClient`
/// (not yet merged) is where this belongs as a shared symbol, and this
/// suite is explicitly the "slice-A-agnostic" fallback path until then.
private enum MeshtasticConfigNonce {
    static let onlyConfig: UInt32 = 69420
    static let onlyNodeDB: UInt32 = 69421
}

final class SerialHardwareTests: XCTestCase {

    /// Firefly 1's own numbers, confirmed read-only against the bench
    /// board itself (`meshtastic --info`, 2026-09-11) and pinned in
    /// docs/hardware/heltec-v3.md's bench table (asserted position) —
    /// not invented here.
    private enum Bench {
        static let nodeNum: UInt32 = 48_629_424
        static let ownerLongName = "Firefly 1"
        static let ownerShortName = "06b0"
        static let latitudeI: Int32 = 477_081_350   // 47.708135 * 1e7
        static let longitudeI: Int32 = -1_222_820_993 // -122.2820993 * 1e7
        static let altitude: Int32 = 40
        static let minimumFirmware = "2.7.26"
    }

    private func requireHardware() throws -> String {
        guard ProcessInfo.processInfo.environment["FIREFLY_HARDWARE"] == "1" else {
            throw XCTSkip("set FIREFLY_HARDWARE=1 (+ FIREFLY_SERIAL_PORT) to run against a real board")
        }
        guard let port = ProcessInfo.processInfo.environment["FIREFLY_SERIAL_PORT"], !port.isEmpty else {
            throw XCTSkip("FIREFLY_HARDWARE=1 but FIREFLY_SERIAL_PORT is unset")
        }
        return port
    }

    // MARK: - the handshake, driven by hand-built ToRadio protobufs

    func testWantConfigReachesFireflyOneWithExpectedIdentity() async throws {
        let port = try requireHardware()
        let transport = SerialTransport(path: port)

        let eventStream = transport.events()
        var framer = StreamFramer()
        let decodedFrames = FrameCollector()

        // Pump raw transport bytes -> StreamFramer -> decoded FromRadio,
        // for the whole lifetime of this test.
        let pump = Task {
            for await event in eventStream {
                if case .received(let raw) = event {
                    for frame in framer.feed(raw) {
                        if let fromRadio = try? MeshtasticProto.FromRadio(serializedBytes: frame) {
                            await decodedFrames.append(fromRadio)
                        }
                    }
                }
            }
        }
        defer { pump.cancel() }

        try await transport.connect()
        defer { Task { await transport.disconnect() } }

        // 1. A keepalive heartbeat with ITS OWN random nonce (>= 2,
        // never 1 — spec: "Meshtastic client" > "Handshake" step 2).
        // This is a DIFFERENT field from `want_config_id` below and
        // carries no firmware-recognized meaning beyond "still here";
        // harmless, read-only in effect.
        var heartbeat = MeshtasticProto.Heartbeat()
        heartbeat.nonce = UInt32.random(in: 2...UInt32.max)
        var heartbeatToRadio = MeshtasticProto.ToRadio()
        heartbeatToRadio.heartbeat = heartbeat
        try await send(heartbeatToRadio, over: transport)

        // 2. Phase A: want_config(onlyConfig) — my_info, metadata,
        // channels, config, module_config, terminated by
        // config_complete_id == onlyConfig. Spec timeout: 30s.
        var phaseA = MeshtasticProto.ToRadio()
        phaseA.wantConfigID = MeshtasticConfigNonce.onlyConfig
        try await send(phaseA, over: transport)

        let configComplete = await decodedFrames.waitFor(timeoutSeconds: 30) { frame in
            if case .configCompleteID(let id) = frame.payloadVariant, id == MeshtasticConfigNonce.onlyConfig {
                return true
            }
            return false
        }
        XCTAssertNotNil(configComplete, "never saw config_complete_id == onlyConfig (69420) within 30s")

        let myInfo = await decodedFrames.first { frame in
            if case .myInfo = frame.payloadVariant { return true }
            return false
        }
        let metadata = await decodedFrames.first { frame in
            if case .metadata = frame.payloadVariant { return true }
            return false
        }

        let unwrappedMyInfo = try XCTUnwrap(myInfo?.myInfo, "no MyNodeInfo in phase A")
        XCTAssertEqual(unwrappedMyInfo.myNodeNum, Bench.nodeNum, "Firefly 1's own node num has changed — update Bench.nodeNum if this board was re-flashed")

        if let firmware = metadata?.metadata.firmwareVersion {
            XCTAssertTrue(
                firmware.hasPrefix(Bench.minimumFirmware),
                "firmware \(firmware) is below the spec's floor (\(Bench.minimumFirmware))")
        } else {
            XCTFail("no DeviceMetadata in phase A")
        }

        // 3. Phase B: want_config(onlyNodeDB) — the node database dump,
        // terminated by config_complete_id == onlyNodeDB. Spec timeout:
        // 120s; capped lower here since a two/three-node bench mesh
        // dumps in well under a second in practice.
        var phaseB = MeshtasticProto.ToRadio()
        phaseB.wantConfigID = MeshtasticConfigNonce.onlyNodeDB
        try await send(phaseB, over: transport)

        let nodeDBComplete = await decodedFrames.waitFor(timeoutSeconds: 60) { frame in
            if case .configCompleteID(let id) = frame.payloadVariant, id == MeshtasticConfigNonce.onlyNodeDB {
                return true
            }
            return false
        }
        XCTAssertNotNil(nodeDBComplete, "never saw config_complete_id == onlyNodeDB (69421) within 60s")

        let selfNodeInfo = await decodedFrames.first { frame in
            if case .nodeInfo(let info) = frame.payloadVariant, info.num == Bench.nodeNum { return true }
            return false
        }
        guard case .nodeInfo(let node)? = selfNodeInfo?.payloadVariant else {
            return XCTFail("Firefly 1 (num \(Bench.nodeNum)) never appeared in its own node db dump")
        }

        // The one thing this suite asserts about identity — read-only,
        // never asserted BY this test onto the radio (see file header).
        XCTAssertEqual(node.user.longName, Bench.ownerLongName)
        XCTAssertEqual(node.user.shortName, Bench.ownerShortName)
        XCTAssertEqual(node.position.latitudeI, Bench.latitudeI)
        XCTAssertEqual(node.position.longitudeI, Bench.longitudeI)
        XCTAssertEqual(node.position.altitude, Bench.altitude)
    }

    // MARK: - helpers

    private func send(_ toRadio: MeshtasticProto.ToRadio, over transport: SerialTransport) async throws {
        let payload = try toRadio.serializedData()
        guard let framed = StreamFramer.frame(payload) else {
            throw XCTSkip("payload too large to frame — should never happen for a handshake message")
        }
        try await transport.send(framed)
    }

    /// Thread-safe accumulator for decoded `FromRadio` frames, with a
    /// poll-based wait — the hardware equivalent of the
    /// `collectFixes`/`withTimeout` helpers the no-hardware tests use,
    /// just against a real, unbounded stream instead of a fixed count.
    private actor FrameCollector {
        private var frames: [MeshtasticProto.FromRadio] = []

        func append(_ frame: MeshtasticProto.FromRadio) {
            frames.append(frame)
        }

        func first(where predicate: (MeshtasticProto.FromRadio) -> Bool) -> MeshtasticProto.FromRadio? {
            frames.first(where: predicate)
        }

        func waitFor(timeoutSeconds: TimeInterval, _ predicate: @escaping (MeshtasticProto.FromRadio) -> Bool) async -> MeshtasticProto.FromRadio? {
            let deadline = Date().addingTimeInterval(timeoutSeconds)
            while Date() < deadline {
                if let match = frames.first(where: predicate) { return match }
                try? await Task.sleep(nanoseconds: 50_000_000)
            }
            return frames.first(where: predicate)
        }
    }
}
