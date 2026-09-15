//
//  QRScannerCameraConfigTests.swift — device-preference-order and
//  zoom-clamping tests for `QRScannerCameraConfig.swift` (the fix for
//  the 2026-09-15 owner report: the QR scanner going blurry up close on
//  the Join-a-crew screen). Run via
//  `xcodebuild test -only-testing:FireflyAppTests` (see project.yml) —
//  same macOS logic-test bundle every other file in `Firefly/Tests`
//  runs under.
//
//  What this file CANNOT cover, and says so rather than pretending
//  otherwise: opening a real `AVCaptureDevice.DiscoverySession`,
//  whether `primaryConstituentDeviceSwitchingBehavior` actually
//  switches to the ultra-wide constituent for a close subject, whether
//  `autoFocusRangeRestriction = .near` actually focuses closer, and
//  whether the chosen zoom actually decodes the puck's QR at the hinted
//  distance. None of that runs without real camera hardware — not the
//  Simulator (no camera at all) and not this macOS bundle. Only a
//  device proves it; see the PR body for exactly what to try.
//
import XCTest

final class QRScannerCameraConfigTests: XCTestCase {
    // MARK: - Device preference order

    func testKindOrderIsTripleThenDualWideThenWideAngle() {
        XCTAssertEqual(QRScannerCameraConfig.backCameraKindOrder, [.triple, .dualWide, .wideAngle])
    }

    func testKindOrderCoversEveryKindExactlyOnce() {
        XCTAssertEqual(Set(QRScannerCameraConfig.backCameraKindOrder), Set(QRScannerCameraKind.allCases))
        XCTAssertEqual(QRScannerCameraConfig.backCameraKindOrder.count, QRScannerCameraKind.allCases.count)
    }

    /// A phone with every kind available (e.g. an iPhone with a
    /// triple-camera system) prefers the triple camera — the most
    /// capable virtual device, so it has the most constituent lenses to
    /// switch among.
    func testPrefersTripleWhenAllAvailable() {
        XCTAssertEqual(QRScannerCameraConfig.preferredKind(among: [.wideAngle, .dualWide, .triple]), .triple)
    }

    /// No triple camera (e.g. an iPhone with only a dual-wide system):
    /// falls through to the next-most-capable virtual device.
    func testFallsBackToDualWideWhenNoTriple() {
        XCTAssertEqual(QRScannerCameraConfig.preferredKind(among: [.wideAngle, .dualWide]), .dualWide)
    }

    /// Only the plain wide camera (e.g. an older or budget iPhone):
    /// falls all the way through, still returns a usable choice.
    func testFallsBackToWideAngleWhenOnlyWideAngleAvailable() {
        XCTAssertEqual(QRScannerCameraConfig.preferredKind(among: [.wideAngle]), .wideAngle)
    }

    /// No back camera the discovery session recognizes at all (e.g. the
    /// Simulator, which has no camera devices): returns `nil` so the
    /// caller falls back to `AVCaptureDevice.default(for: .video)`
    /// rather than crashing or scanning nothing.
    func testReturnsNilWhenNoneAvailable() {
        XCTAssertNil(QRScannerCameraConfig.preferredKind(among: []))
    }

    /// Order in the input doesn't matter — only priority does. Also
    /// covers duplicate entries (defensive; `DiscoverySession.devices`
    /// should never contain the same kind twice, but the algorithm
    /// shouldn't depend on that being true).
    func testInputOrderAndDuplicatesDontAffectResult() {
        XCTAssertEqual(QRScannerCameraConfig.preferredKind(among: [.dualWide, .wideAngle, .dualWide]), .dualWide)
    }

    // MARK: - Zoom clamping

    func testDefaultZoomIsAppliedWhenRoomAllows() {
        XCTAssertEqual(QRScannerCameraConfig.clampedZoomFactor(maximum: 5.0), 1.75)
    }

    /// A device/format whose max zoom is below the default (e.g. a
    /// plain wide-angle-only device with a conservative format): clamps
    /// down to what is actually supported, never passes an
    /// out-of-range value to `videoZoomFactor`.
    func testClampsDownToDeviceMaximum() {
        XCTAssertEqual(QRScannerCameraConfig.clampedZoomFactor(maximum: 1.2), 1.2)
    }

    /// A non-positive or sub-1.0 maximum (a degenerate/fabricated
    /// format) never produces a zoom below 1.0 — 1.0 is always the safe
    /// floor.
    func testNonPositiveMaximumFallsBackToNoZoom() {
        XCTAssertEqual(QRScannerCameraConfig.clampedZoomFactor(maximum: 0), 1.0)
        XCTAssertEqual(QRScannerCameraConfig.clampedZoomFactor(maximum: -1), 1.0)
    }

    func testNonFiniteMaximumFallsBackToNoZoom() {
        XCTAssertEqual(QRScannerCameraConfig.clampedZoomFactor(maximum: .infinity), 1.0)
        XCTAssertEqual(QRScannerCameraConfig.clampedZoomFactor(maximum: .nan), 1.0)
    }

    /// A caller can ask for a lower preferred zoom than the default
    /// (not exercised by the shipping call site today, but the clamp
    /// itself doesn't assume `defaultZoomFactor` is the only input) —
    /// still floored at 1.0 and capped at the maximum.
    func testCustomPreferredZoomIsStillClamped() {
        XCTAssertEqual(QRScannerCameraConfig.clampedZoomFactor(preferred: 0.5, maximum: 5.0), 1.0)
        XCTAssertEqual(QRScannerCameraConfig.clampedZoomFactor(preferred: 3.0, maximum: 2.0), 2.0)
    }

    // MARK: - Zoom vs. macro switching (`targetZoomFactor`)
    //
    // The property under test: a virtual (multi-lens) device must NOT
    // get the digital `defaultZoomFactor` punch-in, because
    // `videoZoomFactor` is itself the input Apple's
    // `virtualDeviceSwitchOverVideoZoomFactors` uses to decide which
    // physical constituent is active — pinning it to a fixed value could
    // sit at/above that device's own (unread, device-specific)
    // switch-over threshold and rule out the ultra-wide constituent
    // entirely, defeating the macro switch this PR exists to enable.
    // Only a plain single-lens device — with no constituent to switch
    // to — gets the punch-in.

    /// Virtual device: stays at its own minimum (which, per Apple's
    /// docs, already represents the ultra-wide constituent's native
    /// FOV on a `.triple`/`.dualWide` device), never the digital
    /// `defaultZoomFactor` punch-in — that is what leaves `.auto`
    /// constituent switching free to engage for a close subject.
    func testVirtualDeviceKeepsMinimumZoomSoAutoSwitchingCanEngage() {
        XCTAssertEqual(
            QRScannerCameraConfig.targetZoomFactor(isVirtualDevice: true, minimum: 1.0, maximum: 6.0),
            1.0)
    }

    /// Plain single-lens device (no constituent to switch to): gets the
    /// digital `defaultZoomFactor` (1.75) punch-in — zoom is the only
    /// lever available to make the code occupy more of the frame.
    func testPlainWideDeviceGetsDigitalZoomPunchIn() {
        XCTAssertEqual(
            QRScannerCameraConfig.targetZoomFactor(isVirtualDevice: false, minimum: 1.0, maximum: 6.0),
            1.75)
    }

    /// Plain single-lens device whose own maximum is below the default
    /// (e.g. a budget device with a conservative format): still clamps
    /// down rather than passing an out-of-range value to
    /// `videoZoomFactor`.
    func testPlainWideDeviceZoomClampsToDeviceMaximum() {
        XCTAssertEqual(
            QRScannerCameraConfig.targetZoomFactor(isVirtualDevice: false, minimum: 1.0, maximum: 1.2),
            1.2)
    }

    /// Virtual device whose reported minimum (degenerate/fabricated
    /// format) is above the device's own maximum: still clamps into
    /// range rather than requesting an unsupported zoom.
    func testVirtualDeviceMinimumClampsToDeviceMaximum() {
        XCTAssertEqual(
            QRScannerCameraConfig.targetZoomFactor(isVirtualDevice: true, minimum: 3.0, maximum: 2.0),
            2.0)
    }

    /// Virtual device with a non-finite/degenerate reported minimum:
    /// never trusts it, falls back to the same safe 1.0 floor as the
    /// non-virtual path.
    func testVirtualDeviceNonFiniteMinimumFallsBackToOne() {
        XCTAssertEqual(
            QRScannerCameraConfig.targetZoomFactor(isVirtualDevice: true, minimum: .nan, maximum: 5.0),
            1.0)
        XCTAssertEqual(
            QRScannerCameraConfig.targetZoomFactor(isVirtualDevice: true, minimum: 0, maximum: 5.0),
            1.0)
    }

    /// Either path: a non-finite/non-positive maximum never produces a
    /// zoom request — the same degenerate-format guard as
    /// `clampedZoomFactor`.
    func testTargetZoomFactorNonPositiveMaximumFallsBackToOne() {
        XCTAssertEqual(
            QRScannerCameraConfig.targetZoomFactor(isVirtualDevice: true, minimum: 1.0, maximum: 0),
            1.0)
        XCTAssertEqual(
            QRScannerCameraConfig.targetZoomFactor(isVirtualDevice: false, minimum: 1.0, maximum: .nan),
            1.0)
    }
}
