//
//  NodeDBTests.swift — the three absence rules, pinned directly against
//  `NodeDB`'s wire conversion (docs/specs/A01-companion-app.md,
//  "NodeDB"): location_source, RSSI/SNR attribution via hop path, and
//  precision_bits. `@testable` because `NodeDB`/`RxPath` are internal —
//  the wire-conversion boundary itself, not `MeshtasticClientProtocol`'s
//  public surface.
//
import MeshtasticProto
@testable import FireflyMesh
import XCTest

final class NodeDBTests: XCTestCase {

    // MARK: - Rule 1: location_source

    func testLocationSourceAbsentUnsetAndUnrecognizedAllReadUnknown() {
        XCTAssertEqual(NodeDB.locationSource(.locUnset), .unknown)
        XCTAssertEqual(NodeDB.locationSource(.UNRECOGNIZED(99)), .unknown)
        XCTAssertEqual(NodeDB.locationSource(.locManual), .manual)
        XCTAssertEqual(NodeDB.locationSource(.locInternal), .internalGPS)
        XCTAssertEqual(NodeDB.locationSource(.locExternal), .externalGPS)
    }

    /// MANUAL is asserted, not measured — never confusable with
    /// `.internalGPS`, the case a bug here would most plausibly collapse
    /// into.
    func testManualIsNeverInternalGPS() {
        XCTAssertNotEqual(NodeDB.locationSource(.locManual), .internalGPS)
    }

    // MARK: - Rule 3: precision_bits

    func testPrecisionBitsZeroAndAboveThirtyTwoReadAbsent() {
        XCTAssertNil(NodeDB.precisionBits(0))
        XCTAssertNil(NodeDB.precisionBits(33))
        XCTAssertNil(NodeDB.precisionBits(1000))
    }

    func testPrecisionBitsOneThroughThirtyTwoArePresent() {
        XCTAssertEqual(NodeDB.precisionBits(1), 1)
        XCTAssertEqual(NodeDB.precisionBits(13), 13)
        XCTAssertEqual(NodeDB.precisionBits(32), 32)
    }

    // MARK: - Rule 2 (hop half): rxPath — ported from mc_client.c's
    // mc_rx_path_from_pkt, table for table.

    func testRxPathViaMqttIsAlwaysIndirect() {
        XCTAssertEqual(NodeDB.rxPath(hopStart: 3, hopLimit: 3, hasDecodedBitfield: true, viaMqtt: true), .indirect)
        XCTAssertEqual(NodeDB.rxPath(hopStart: 0, hopLimit: 0, hasDecodedBitfield: true, viaMqtt: true), .indirect)
    }

    func testRxPathHopStartPositiveEqualHopLimitIsDirect() {
        XCTAssertEqual(NodeDB.rxPath(hopStart: 3, hopLimit: 3, hasDecodedBitfield: false, viaMqtt: false), .direct)
    }

    func testRxPathHopStartPositiveLessHopLimitIsIndirect() {
        XCTAssertEqual(NodeDB.rxPath(hopStart: 3, hopLimit: 1, hasDecodedBitfield: false, viaMqtt: false), .indirect)
    }

    /// hop_limit exceeding hop_start cannot happen honestly — malformed,
    /// reads UNKNOWN rather than any confident guess.
    func testRxPathHopLimitExceedingHopStartIsUnknown() {
        XCTAssertEqual(NodeDB.rxPath(hopStart: 1, hopLimit: 3, hasDecodedBitfield: false, viaMqtt: false), .unknown)
    }

    /// hop_start == 0 alone is UNKNOWN, never DIRECT — the core rule
    /// this whole function exists to enforce (pre-2.3.0 firmware never
    /// set hop_start at all).
    func testRxPathHopStartZeroWithoutBitfieldIsUnknown() {
        XCTAssertEqual(NodeDB.rxPath(hopStart: 0, hopLimit: 0, hasDecodedBitfield: false, viaMqtt: false), .unknown)
    }

    /// hop_start == 0 is DIRECT only once the sender's decoded bitfield
    /// (2.5.0+) proves the zero is real, AND hop_limit is also 0.
    func testRxPathHopStartZeroWithBitfieldAndZeroHopLimitIsDirect() {
        XCTAssertEqual(NodeDB.rxPath(hopStart: 0, hopLimit: 0, hasDecodedBitfield: true, viaMqtt: false), .direct)
    }

    func testRxPathHopStartZeroWithBitfieldButNonzeroHopLimitIsUnknown() {
        XCTAssertEqual(NodeDB.rxPath(hopStart: 0, hopLimit: 2, hasDecodedBitfield: true, viaMqtt: false), .unknown)
    }

    // MARK: - NodeDB instance behaviour

    func testApplyNodeInfoBuildsASnapshot() {
        var db = NodeDB()
        var info = NodeInfo()
        info.num = 0x02e6_06b0
        var user = User()
        user.shortName = "F1"
        user.longName = "Firefly 1"
        info.user = user
        var pos = Position()
        pos.latitudeI = Int32(47.708_135 * 1e7)
        pos.longitudeI = Int32(-122.282_0993 * 1e7)
        pos.locationSource = .locManual
        pos.precisionBits = 32
        info.position = pos

        let snapshot = db.apply(nodeInfo: info)
        XCTAssertEqual(snapshot.num, 0x02e6_06b0)
        XCTAssertEqual(snapshot.shortName, "F1")
        XCTAssertEqual(snapshot.longName, "Firefly 1")
        XCTAssertEqual(snapshot.position?.source, .manual)
        XCTAssertEqual(snapshot.position?.precisionBits, 32)
        XCTAssertEqual(snapshot.position?.latitude ?? 0, 47.708_135, accuracy: 1e-5)
    }

    /// A node whose position arrives with `precision_bits` absent must
    /// never render a metre-level distance — pinned here as "absent
    /// stays absent through NodeDB", the fact the Radar (slice D) relies
    /// on (A01_AC6).
    func testNodeInfoWithNoPrecisionBitsReadsAbsent() {
        var db = NodeDB()
        var info = NodeInfo()
        info.num = 1
        var pos = Position()
        pos.latitudeI = 1
        pos.longitudeI = 1
        // precisionBits left at the wire default (0).
        info.position = pos

        let snapshot = db.apply(nodeInfo: info)
        XCTAssertNil(snapshot.position?.precisionBits)
    }

    /// A MANUAL position must never age into looking like a stale
    /// measurement: NodeDB itself does not attach staleness, only
    /// source — this pins that the source survives untouched.
    func testManualPositionSourceSurvivesIntoTheSnapshot() {
        var db = NodeDB()
        var info = NodeInfo()
        info.num = 1
        var pos = Position()
        pos.latitudeI = 1
        pos.longitudeI = 1
        pos.locationSource = .locManual
        info.position = pos

        let snapshot = db.apply(nodeInfo: info)
        XCTAssertEqual(snapshot.position?.source, .manual)
    }

    func testApplyPositionReturnsNilWithNoFix() {
        var db = NodeDB()
        let pos = Position() // no latitudeI/longitudeI set at all
        XCTAssertNil(db.apply(position: pos, from: 1, rxTime: nil))
    }

    func testApplyPositionUpdatesAnExistingNode() {
        var db = NodeDB()
        var info = NodeInfo()
        info.num = 1
        var user = User()
        user.shortName = "F1"
        info.user = user
        db.apply(nodeInfo: info)

        var pos = Position()
        pos.latitudeI = Int32(1 * 1e7)
        pos.longitudeI = Int32(2 * 1e7)
        pos.locationSource = .locExternal
        let snapshot = db.apply(position: pos, from: 1, rxTime: Date())
        XCTAssertEqual(snapshot?.shortName, "F1") // preserved across the position-only update
        XCTAssertEqual(snapshot?.position?.source, .externalGPS)
    }

    /// RSSI/SNR must never be attributed to a node with no slot yet —
    /// "nobody to attribute the reading to" (mc_client.c's own reasoning
    /// for on_rx_meta's ordering guarantee).
    func testApplyRxMetaOnUnknownNodeDoesNothing() {
        var db = NodeDB()
        XCTAssertNil(db.applyRxMeta(from: 1, rssiDbm: -40, snrDb: 5, path: .direct))
    }

    /// Only `.direct` licenses attributing rssi/snr — `.indirect`/
    /// `.unknown` must leave the existing values untouched.
    func testApplyRxMetaOnlyAttributesWhenDirect() {
        var db = NodeDB()
        var info = NodeInfo()
        info.num = 1
        db.apply(nodeInfo: info)

        XCTAssertNil(db.applyRxMeta(from: 1, rssiDbm: -40, snrDb: 5, path: .indirect))
        XCTAssertNil(db.applyRxMeta(from: 1, rssiDbm: -40, snrDb: 5, path: .unknown))
        XCTAssertNil(db.node(1)?.rssiDbm)

        let snapshot = db.applyRxMeta(from: 1, rssiDbm: -40, snrDb: 5, path: .direct)
        XCTAssertEqual(snapshot?.rssiDbm, -40)
        XCTAssertEqual(snapshot?.snrDb, 5)
    }

    func testResetClearsEverything() {
        var db = NodeDB()
        var info = NodeInfo()
        info.num = 1
        db.apply(nodeInfo: info)
        XCTAssertEqual(db.all.count, 1)
        db.reset()
        XCTAssertTrue(db.all.isEmpty)
        XCTAssertNil(db.node(1))
    }

    func testHopsAwayAbsentWhenNodeInfoDoesNotStateIt() {
        var db = NodeDB()
        var info = NodeInfo()
        info.num = 1 // hopsAway left unset (implicit-presence style has_hops_away == false)
        let snapshot = db.apply(nodeInfo: info)
        XCTAssertNil(snapshot.hopsAway)
    }

    func testHopsAwayPresentWhenNodeInfoStatesIt() {
        var db = NodeDB()
        var info = NodeInfo()
        info.num = 1
        info.hopsAway = 2
        let snapshot = db.apply(nodeInfo: info)
        XCTAssertEqual(snapshot.hopsAway, 2)
    }
}
