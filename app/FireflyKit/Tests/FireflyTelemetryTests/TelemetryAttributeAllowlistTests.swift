//
//  TelemetryAttributeAllowlistTests.swift — A04: "no coordinates, no
//  text" as a test, not just a comment. Two things are pinned: the
//  PREDICATE itself (`isClean`/`strip`), and that `TelemetryRecorder`
//  actually calls it on the way to disk — a guard that only protects
//  the predicate and never checks it is wired in would pass while the
//  real recording path still leaked a coordinate.
//
import Foundation
import XCTest
@testable import FireflyTelemetry

final class TelemetryAttributeAllowlistTests: XCTestCase {
    func testForbidsCoordinateKeys() {
        for key in ["lat", "latitude", "lon", "lng", "longitude", "coordinate", "coordinates", "position",
                    "gps_lat", "gps_lon", "gps_latitude", "gps_longitude"] {
            XCTAssertFalse(TelemetryAttributeAllowlist.isClean([key: .double(47.6)]), "\(key) must be forbidden")
        }
    }

    func testForbidsMessageTextKeys() {
        for key in ["text", "body", "message", "message_text", "content"] {
            XCTAssertFalse(TelemetryAttributeAllowlist.isClean([key: .string("hey crew")]),
                           "\(key) must be forbidden")
        }
    }

    func testForbidsCrewCodeKeys() {
        for key in ["code", "crew_code", "join_code", "invite_code"] {
            XCTAssertFalse(TelemetryAttributeAllowlist.isClean([key: .string("FIRE-4K9M7X")]),
                           "\(key) must be forbidden")
        }
    }

    func testForbidsRawNodeIdentifierKeys() {
        for key in ["node_id", "node_num", "from", "to"] {
            XCTAssertFalse(TelemetryAttributeAllowlist.isClean([key: .int(123456)]), "\(key) must be forbidden")
        }
    }

    func testForbidsCaseInsensitively() {
        XCTAssertFalse(TelemetryAttributeAllowlist.isClean(["LATITUDE": .double(47.6)]))
        XCTAssertFalse(TelemetryAttributeAllowlist.isClean(["Message": .string("hi")]))
    }

    func testAllowsTheRealCatalogueKeys() {
        let realAttributes: [String: TelemetryValue] = [
            TelemetryAttributeKey.rssi: .int(-60),
            TelemetryAttributeKey.trigger: .string("manual"),
            TelemetryAttributeKey.idHash: .string(TelemetryHash.nodeID(42)),
            TelemetryAttributeKey.ageS: .double(12.5),
            TelemetryAttributeKey.accuracyBucket: .string("100m-1km"),
            TelemetryAttributeKey.outcome: .string("ok"),
        ]
        XCTAssertTrue(TelemetryAttributeAllowlist.isClean(realAttributes))
    }

    func testStripRemovesOnlyForbiddenKeys() {
        let mixed: [String: TelemetryValue] = [
            "rssi": .int(-60),
            "lat": .double(47.6),
            "text": .string("do not record me"),
        ]
        let cleaned = TelemetryAttributeAllowlist.strip(mixed)
        XCTAssertEqual(cleaned, ["rssi": .int(-60)])
    }

    /// Belt AND braces: `TelemetryRecorder.record(_:)` must call
    /// `strip(_:)` itself, so a call site that violates the allowlist
    /// never reaches disk even if nobody remembered to check `isClean`
    /// first.
    func testRecorderStripsForbiddenAttributesBeforePersisting() async throws {
        let dir = FileManager.default.temporaryDirectory.appending(path: "AllowlistGuard-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }

        let recorder = TelemetryRecorder(directory: dir)
        await recorder.record(TelemetryEvent(name: "gps.uplink", attributes: [
            "outcome": .string("ok"),
            "lat": .double(47.6062),
            "lon": .double(-122.3321),
        ]))

        let files = await recorder.exportFiles()
        let content = try String(contentsOf: try XCTUnwrap(files.first), encoding: .utf8)
        XCTAssertFalse(content.contains("47.6062"), "a raw latitude must never reach disk")
        XCTAssertFalse(content.contains("-122.3321"), "a raw longitude must never reach disk")
        XCTAssertTrue(content.contains("\"outcome\""), "the allowed attribute must still be recorded")
    }
}
