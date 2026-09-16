//
//  TelemetryRecorderDurabilityTests.swift — A04: "make sure we don't
//  miss any data" (the owner's own ask), pinned as tests. Every test
//  here uses a temp directory (`FileManager.default.temporaryDirectory`,
//  a fresh UUID subdirectory per test so tests never collide) and
//  cleans it up at the end of the test.
//
import Foundation
import XCTest
@testable import FireflyTelemetry

final class TelemetryRecorderDurabilityTests: XCTestCase {
    private func tempDirectory() -> URL {
        let url = FileManager.default.temporaryDirectory.appending(path: "TelemetryRecorderTests-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    private func lines(of url: URL) -> [String] {
        guard let content = try? String(contentsOf: url, encoding: .utf8) else { return [] }
        return content.split(separator: "\n", omittingEmptySubsequences: true).map(String.init)
    }

    func testAppendsOneLinePerEvent() async throws {
        let dir = tempDirectory()
        defer { try? FileManager.default.removeItem(at: dir) }
        let recorder = TelemetryRecorder(directory: dir)
        await recorder.record(TelemetryEvent(name: "ble.scan.start"))
        await recorder.record(TelemetryEvent(name: "ble.scan.stop"))
        await recorder.record(TelemetryEvent(name: "ble.discovered", attributes: ["rssi": .int(-62)]))

        let files = await recorder.exportFiles()
        XCTAssertEqual(files.count, 1)
        let first = try XCTUnwrap(files.first)
        XCTAssertEqual(lines(of: first).count, 3)
    }

    func testSeqIsMonotonicAndSessionIDIsShared() async throws {
        let dir = tempDirectory()
        defer { try? FileManager.default.removeItem(at: dir) }
        let recorder = TelemetryRecorder(directory: dir, sessionID: "session-abc")
        await recorder.record(TelemetryEvent(name: "app.launch"))
        await recorder.record(TelemetryEvent(name: "app.foreground"))

        let files = await recorder.exportFiles()
        let first = try XCTUnwrap(files.first)
        let decoded = try decodeEvents(from: first)
        XCTAssertEqual(decoded.map(\.seq), [1, 2])
        XCTAssertTrue(decoded.allSatisfy { $0.sessionID == "session-abc" })
    }

    func testRotatesAtTheByteThresholdAndKeepsOnlyTheLastNFiles() async throws {
        let dir = tempDirectory()
        defer { try? FileManager.default.removeItem(at: dir) }
        // A tiny threshold and a small `maxFiles` so this test rotates
        // in single-digit event counts rather than needing ~5 MB of
        // real events to exercise the real default.
        let recorder = TelemetryRecorder(directory: dir, maxFileBytes: 200, maxFiles: 3)
        // Each event's JSON line is comfortably under 200 bytes, so a
        // handful of them should force at least one rotation.
        for index in 0..<40 {
            await recorder.record(TelemetryEvent(name: "app.foreground", attributes: ["n": .int(index)]))
        }
        let allFiles = try FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)
            .filter { $0.pathExtension == "jsonl" }
        // maxFiles: 3 means "current + at most 2 rotated" — never more
        // than 3 files on disk, however many events were recorded.
        XCTAssertLessThanOrEqual(allFiles.count, 3)
        XCTAssertGreaterThan(allFiles.count, 1,
                              "40 small events at a 200-byte threshold must have rotated at least once")
    }

    func testSurvivesRelaunchByAppendingToTheSameCurrentFileRatherThanTruncatingIt() async throws {
        let dir = tempDirectory()
        defer { try? FileManager.default.removeItem(at: dir) }

        let firstProcess = TelemetryRecorder(directory: dir, sessionID: "session-1")
        await firstProcess.record(TelemetryEvent(name: "app.launch"))
        await firstProcess.record(TelemetryEvent(name: "ble.connected"))

        // Simulate a relaunch: a brand-new actor instance over the SAME
        // directory, as a real process restart would construct.
        let secondProcess = TelemetryRecorder(directory: dir, sessionID: "session-2")
        await secondProcess.record(TelemetryEvent(name: "app.launch"))

        let files = await secondProcess.exportFiles()
        let first = try XCTUnwrap(files.first)
        let decoded = try decodeEvents(from: first)
        XCTAssertEqual(decoded.count, 3, "the second process must APPEND, never truncate what the first left behind")
        XCTAssertEqual(decoded.map(\.name), ["app.launch", "ble.connected", "app.launch"])
        XCTAssertEqual(decoded.map(\.sessionID), ["session-1", "session-1", "session-2"])
    }

    func testNoSinksMeansLocalRecordingStillSucceeds() async throws {
        // The "Share diagnostics" OFF path and the no-Firebase-plist
        // path both mean zero sinks — recording locally must not depend
        // on there being anywhere else for an event to go.
        let dir = tempDirectory()
        defer { try? FileManager.default.removeItem(at: dir) }
        let recorder = TelemetryRecorder(directory: dir, sinks: [])
        await recorder.record(TelemetryEvent(name: "app.launch"))
        let files = await recorder.exportFiles()
        let first = try XCTUnwrap(files.first)
        XCTAssertEqual(lines(of: first).count, 1)
    }

    func testFansOutToEveryAttachedSink() async throws {
        let dir = tempDirectory()
        defer { try? FileManager.default.removeItem(at: dir) }
        let sink = RecordingSink()
        let recorder = TelemetryRecorder(directory: dir, sinks: [sink])
        await recorder.record(TelemetryEvent(name: "app.launch"))
        let received = await sink.received
        XCTAssertEqual(received.map(\.name), ["app.launch"])
    }

    func testAddSinkAttachesAfterConstruction() async throws {
        let dir = tempDirectory()
        defer { try? FileManager.default.removeItem(at: dir) }
        let recorder = TelemetryRecorder(directory: dir)
        let sink = RecordingSink()
        await recorder.addSink(sink)
        await recorder.record(TelemetryEvent(name: "app.launch"))
        let received = await sink.received
        XCTAssertEqual(received.map(\.name), ["app.launch"])
    }

    /// A04 — `notifyBackground()`'s own fan-out, the "…and on
    /// background" flush trigger `TelemetryBatchPolicy` cannot notice on
    /// its own (`TelemetrySink.flushOnBackground()`'s own doc comment).
    func testNotifyBackgroundFansOutToEveryAttachedSinksFlushOnBackground() async throws {
        let dir = tempDirectory()
        defer { try? FileManager.default.removeItem(at: dir) }
        let sink = RecordingSink()
        let recorder = TelemetryRecorder(directory: dir, sinks: [sink])
        await recorder.notifyBackground()
        let backgroundCount = await sink.backgroundFlushCount
        XCTAssertEqual(backgroundCount, 1)
    }

    /// A sink that never overrides `flushOnBackground()` (every sink
    /// this PR ships except `FirebaseSink`, which is app-target-only and
    /// so cannot be exercised from this package) gets the protocol
    /// extension's default no-op — proven here by a sink with NO
    /// override at all still conforming and `notifyBackground()` still
    /// completing without error.
    func testNotifyBackgroundIsHarmlessForASinkWithNoOverrideOfItsOwn() async throws {
        let dir = tempDirectory()
        defer { try? FileManager.default.removeItem(at: dir) }
        let recorder = TelemetryRecorder(directory: dir, sinks: [NoopSink()])
        await recorder.notifyBackground() // must not throw/hang
    }

    private func decodeEvents(from url: URL) throws -> [TelemetryEvent] {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return try lines(of: url).map { try decoder.decode(TelemetryEvent.self, from: Data($0.utf8)) }
    }
}

private actor RecordingSink: TelemetrySink {
    private(set) var received: [TelemetryEvent] = []
    private(set) var backgroundFlushCount = 0
    func send(_ event: TelemetryEvent) async { received.append(event) }
    func flushOnBackground() async { backgroundFlushCount += 1 }
}
