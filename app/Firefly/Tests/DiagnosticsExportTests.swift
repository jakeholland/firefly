//
//  DiagnosticsExportTests.swift — A04 review gap-close: "Export
//  diagnostics" produces a readable file, tested end to end against a
//  real `TelemetryRecorder` over a temp directory (never a mock of the
//  file format) — the exact seam `DiagnosticsViewModel.exportDiagnosticsFiles()`
//  hands to the share sheet.
//
import FireflyMesh
import FireflyModel
import FireflyTelemetry
import XCTest

@MainActor
final class DiagnosticsExportTests: XCTestCase {
    private func tempDirectory() -> URL {
        let url = FileManager.default.temporaryDirectory.appending(path: "DiagnosticsExportTests-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    /// `.stub()`-shaped composition — no real recorder — hides the row
    /// entirely rather than offering an export with nothing behind it.
    func testCanExportDiagnosticsIsFalseWithNoRealRecorder() {
        let model = DiagnosticsViewModel(client: StubMeshtasticClient())
        XCTAssertFalse(model.canExportDiagnostics)
    }

    func testExportDiagnosticsFilesIsEmptyWithNoRealRecorder() async {
        let model = DiagnosticsViewModel(client: StubMeshtasticClient())
        let files = await model.exportDiagnosticsFiles()
        XCTAssertTrue(files.isEmpty)
    }

    /// The actual "produces a readable file" claim: record a few real
    /// events, export, and read every exported file back as valid
    /// JSON-lines — the phone's own file, un-uploaded, that a Bailey
    /// with no signal at all could hand over via AirDrop/USB.
    func testExportDiagnosticsFilesAreReadableJSONLinesInRecordedOrder() async throws {
        let dir = tempDirectory()
        defer { try? FileManager.default.removeItem(at: dir) }
        let recorder = TelemetryRecorder(directory: dir)
        await recorder.record(TelemetryEvent(name: "app.launch"))
        await recorder.record(TelemetryEvent(name: "ble.connected"))
        await recorder.record(TelemetryEvent(name: "ble.disconnected", attributes: ["reason": .string("user")]))

        let model = DiagnosticsViewModel(client: StubMeshtasticClient(), telemetryExporting: recorder)
        XCTAssertTrue(model.canExportDiagnostics)

        let files = await model.exportDiagnosticsFiles()
        XCTAssertEqual(files.count, 1)
        let content = try String(contentsOf: try XCTUnwrap(files.first), encoding: .utf8)
        let lines = content.split(separator: "\n", omittingEmptySubsequences: true)
        XCTAssertEqual(lines.count, 3, "every recorded event must be present as its own readable line")

        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let decoded = try lines.map { try decoder.decode(TelemetryEvent.self, from: Data($0.utf8)) }
        XCTAssertEqual(decoded.map(\.name), ["app.launch", "ble.connected", "ble.disconnected"],
                       "export order must match recorded order — oldest first")
    }
}
