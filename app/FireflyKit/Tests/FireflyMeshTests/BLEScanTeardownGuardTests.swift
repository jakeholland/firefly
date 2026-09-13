//
//  BLEScanTeardownGuardTests.swift — a SOURCE-level invariant guard,
//  the same genre as `ThemeTests`' ff_theme.h parse: it reads
//  `BLETransport.swift` and asserts a structural property, because the
//  behaviour it protects cannot be exercised without a real radio (BLE
//  needs a TCC grant and lives in `FireflyHardwareTests`, per A01 B1).
//
//  The property: `isFallbackScanning` is cleared in exactly ONE place,
//  `endFallbackScan()`, and that method stops the radio scan.
//
//  The bug this pins (hardening QA pass): `completeConnect(throwing:)`
//  cleared the flag with a bare assignment and never called
//  `central.stopScan()`, unlike the two other sites that did. Whenever
//  the original pending `central.connect()` resolved after
//  `armReconnectFallback(for:)` had already started its rediscovery
//  scan — a slow reconnect, which is the entire case that fallback
//  exists for — CoreBluetooth kept scanning for the rest of the
//  process. That is one of the most expensive things an iPhone can be
//  asked to do, on a device this project needs to survive three days in
//  a field.
//
//  A structural guard is weaker than a behavioural test and is not
//  pretending otherwise: it cannot prove the scan actually stops on a
//  radio. It CAN prove nobody re-introduces a fourth, unpaired
//  assignment — which is exactly how the bug got in.
//
import XCTest

final class BLEScanTeardownGuardTests: XCTestCase {

    private func transportSource() throws -> String {
        let url = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent() // FireflyMeshTests
            .deletingLastPathComponent() // Tests
            .deletingLastPathComponent() // FireflyKit
            .appending(path: "Sources/FireflyMesh/BLE/BLETransport.swift")
        return try String(contentsOf: url, encoding: .utf8)
    }

    /// Comment lines are excluded: this file's own fix is documented in
    /// prose that quotes the old assignment, and a guard that counted
    /// its own explanation would be unmaintainable.
    private func codeLines(_ source: String) -> [String] {
        source.split(separator: "\n", omittingEmptySubsequences: false)
            .map(String.init)
            .filter { !$0.trimmingCharacters(in: .whitespaces).hasPrefix("//") }
    }

    func testIsFallbackScanningIsClearedInExactlyOnePlace() throws {
        // The declaration (`private var isFallbackScanning = false`) is
        // not a clear — only assignments count.
        let clears = codeLines(try transportSource())
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { $0.contains("isFallbackScanning = false") && !$0.hasPrefix("private var") }
        XCTAssertEqual(clears.count, 1,
                       "`isFallbackScanning = false` must appear exactly once, inside endFallbackScan() — "
                       + "an unpaired clear leaves CoreBluetooth scanning for the rest of the process. Found: \(clears)")
    }

    func testEndFallbackScanClearsTheFlagAndStopsTheScan() throws {
        let source = try transportSource()
        let body = try XCTUnwrap(
            source.range(of: "private func endFallbackScan() {").map { String(source[$0.upperBound...].prefix(400)) },
            "endFallbackScan() must exist — it is the one place the fallback scan may be torn down")
        let end = try XCTUnwrap(body.range(of: "\n    }"))
        let scoped = String(body[..<end.lowerBound])

        XCTAssertTrue(scoped.contains("isFallbackScanning = false"), "endFallbackScan() must clear the flag")
        XCTAssertTrue(scoped.contains("stopScan()"),
                      "endFallbackScan() must actually stop the radio scan, not only clear the flag")
    }

    /// The site the bug was actually in. Named explicitly so a future
    /// reader sees which call site regressed, not just the aggregate.
    func testCompleteConnectTearsDownTheFallbackScan() throws {
        let source = try transportSource()
        let start = try XCTUnwrap(source.range(of: "private func completeConnect(throwing error: Error?) {"))
        let body = String(source[start.upperBound...].prefix(2000))
        XCTAssertTrue(body.contains("endFallbackScan()"),
                      "completeConnect(throwing:) must tear the fallback scan down — a connect chain that has "
                      + "finished has no use for a rediscovery scan, and leaving it running drains the battery "
                      + "for the rest of the session")
    }
}
