//
//  TelemetryHashTests.swift — A04: `crew.member.seen`/`crew.member.lost`'s
//  `id_hash` must be deterministic (so "this member keeps dropping" is
//  answerable from the log), different node ids must not collide in
//  practice, and the hash must never simply BE the node number (that
//  would make it not a hash at all — the exact thing this exists to
//  avoid recording raw).
//
import XCTest
@testable import FireflyTelemetry

final class TelemetryHashTests: XCTestCase {
    func testDeterministic() {
        XCTAssertEqual(TelemetryHash.nodeID(0x1234_5678), TelemetryHash.nodeID(0x1234_5678))
    }

    func testDifferentNodesHashDifferently() {
        XCTAssertNotEqual(TelemetryHash.nodeID(1), TelemetryHash.nodeID(2))
    }

    func testHashIsNotTheRawNodeNumber() {
        let nodeNum: UInt32 = 0xDEAD_BEEF
        let hash = TelemetryHash.nodeID(nodeNum)
        XCTAssertNotEqual(hash, String(nodeNum))
        XCTAssertNotEqual(hash, String(format: "%08x", nodeNum))
        XCTAssertFalse(hash.contains(String(format: "%08x", nodeNum)))
    }

    func testHashLengthIsFixed() {
        XCTAssertEqual(TelemetryHash.nodeID(1).count, 16)
        XCTAssertEqual(TelemetryHash.nodeID(.max).count, 16)
    }

    func testHashIsLowercaseHex() {
        let hash = TelemetryHash.nodeID(42)
        XCTAssertTrue(hash.allSatisfy { $0.isHexDigit && !$0.isUppercase })
    }
}
