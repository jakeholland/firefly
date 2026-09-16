//
//  QRScannerRectOfInterestGuardTests.swift — a SOURCE-level invariant
//  guard, the same genre as `FindLifecycleWiringGuardTests`/
//  `RootDeepLinkWiringGuardTests` next door: it reads
//  `QRScannerSheet.swift` and asserts a structural property, because
//  the behaviour it protects — whether `AVCaptureMetadataOutput
//  .rectOfInterest` conversion happens before or after the capture
//  session has a running `AVCaptureConnection` — needs real camera
//  hardware to observe directly, and no unit test in this
//  AVFoundation-free macOS logic-test bundle can drive it.
//
//  The bug this pins (owner report, iPhone 17 Pro Max, TestFlight 347,
//  2026-09-16). PR #325 added `updateRectOfInterest()`, called from
//  `viewDidLayoutSubviews()` — which fires from `viewDidLoad()`'s
//  initial layout pass, BEFORE `viewDidAppear()` starts
//  `session.startRunning()`. `AVCaptureVideoPreviewLayer
//  .metadataOutputRectConverted(fromLayerRect:)` converts through the
//  preview layer's `AVCaptureConnection`, which does not exist until
//  the session is running, so every pre-`viewDidAppear` call produced
//  an empty/degenerate rect. `rectOfInterest` latched to that and
//  nothing ever recomputed it, because layout does not fire again on
//  its own — every `AVMetadataObject` was silently discarded
//  regardless of how crisp or well-framed the code was. Reviewed and
//  passed at PR #325 time (see that PR's comment thread) on the
//  strength of a zero-size guard that addresses a DIFFERENT failure
//  mode (a layout pass with `view.bounds` still `.zero`) — the real
//  bug is the missing capture connection, which a non-zero bounds does
//  not fix.
//
//  The fix: `rectOfInterest` is no longer assigned at all —
//  `QRScannerSheet.swift`'s own header comment ("Empty-`rectOfInterest`
//  fix") explains why a full-frame decode with a purely visual guide
//  box is strictly more robust here than a narrowed region that can
//  come out empty and silently kill every detection. This guard pins
//  that nobody reintroduces the assignment.
//
import XCTest

final class QRScannerRectOfInterestGuardTests: XCTestCase {

    // MARK: - Source reading

    private func source(_ relativePath: String) throws -> String {
        let url = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent() // this file -> Tests
            .deletingLastPathComponent() // Tests -> Firefly
            .appending(path: relativePath)
        return try String(contentsOf: url, encoding: .utf8)
    }

    /// Comments stripped before any search — this file's own prose
    /// above (and `QRScannerSheet.swift`'s header comment) both quote
    /// `rectOfInterest` and `updateRectOfInterest` at length, so a
    /// guard that matched its own explanation would read the wrong
    /// thing. Same rule `FindLifecycleWiringGuardTests` follows.
    private func codeLines(_ text: String) -> [String] {
        text.split(separator: "\n", omittingEmptySubsequences: false)
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.hasPrefix("//") && !$0.hasPrefix("///") }
    }

    /// The property under test: `metadataOutput.rectOfInterest` (or
    /// any local/property named similarly) is never ASSIGNED anywhere
    /// in this file. Reading the default is fine (the DEBUG
    /// diagnostics do exactly that, via `\(effectiveRectOfInterest)`
    /// string interpolation, which is why the pattern below requires
    /// the dot-member-access shape a real Swift assignment has —
    /// `<expr>.rectOfInterest = <value>` — rather than a bare
    /// substring match, which would also flag that diagnostic's own
    /// `"rectOfInterest=\(...)"` log key).
    func testRectOfInterestIsNeverAssignedInQRScannerSheet() throws {
        let lines = codeLines(try source("Sources/Connect/QRScannerSheet.swift"))
        let assignments = lines.filter {
            $0.contains(".rectOfInterest =") || $0.contains(".rectOfInterest=")
        }
        XCTAssertTrue(assignments.isEmpty,
                      "QRScannerSheet.swift must never assign `rectOfInterest`: the conversion "
                      + "(`previewLayer.metadataOutputRectConverted(fromLayerRect:)`) is only valid "
                      + "once the capture session has a running `AVCaptureConnection`, i.e. after "
                      + "`session.startRunning()` — and the only place that used to call it "
                      + "(`viewDidLayoutSubviews()`) runs at initial layout, well before that. A wrong "
                      + "ROI does not throw or log anything by itself; it just silently discards every "
                      + "`AVMetadataObject`. Found: \(assignments)")
    }

    /// The specific method PR #325 added and this fix removes. Its
    /// return would mean someone re-added the exact call chain that
    /// caused the original bug, even if this guard's `rectOfInterest`
    /// search above were somehow satisfied some other way (e.g. a
    /// computed property instead of a plain assignment).
    func testUpdateRectOfInterestMethodDoesNotExist() throws {
        let lines = codeLines(try source("Sources/Connect/QRScannerSheet.swift"))
        XCTAssertFalse(lines.contains { $0.contains("func updateRectOfInterest") },
                       "`updateRectOfInterest()` must stay removed — see this file's header comment "
                       + "for why it silently broke decoding on every real device.")
    }

    /// `viewDidLayoutSubviews()`/`layoutPreviewAndGuideBox()` must not
    /// gain a NEW call into `metadataOutputRectConverted(fromLayerRect:)`
    /// under a different name either — the property being guarded is
    /// "never converts through the preview layer before the session is
    /// running," not just "no function named `updateRectOfInterest`."
    func testLayoutPathNeverConvertsAMetadataOutputRect() throws {
        let lines = codeLines(try source("Sources/Connect/QRScannerSheet.swift"))
        XCTAssertFalse(lines.contains { $0.contains("metadataOutputRectConverted") },
                       "no code path in QRScannerSheet.swift may call "
                       + "`metadataOutputRectConverted(fromLayerRect:)` — see this file's header "
                       + "comment: it is only valid once `AVCaptureConnection` exists, i.e. after "
                       + "`session.startRunning()`, and every layout-time call site this file has ever "
                       + "had ran earlier than that.")
    }
}
