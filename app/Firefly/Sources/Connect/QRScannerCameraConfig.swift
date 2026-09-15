//
//  QRScannerCameraConfig.swift — AVFoundation-free camera-selection and
//  zoom policy for the QR scanner.
//
//  Owner report (iPhone 17 Pro Max, 2026-09-15): on the Join-a-crew
//  screen the scanner went blurry when the phone was brought close to
//  the puck's screen and only decoded from far away. Cause:
//  `QRScannerSheet`/`QRScannerViewController` picked `AVCaptureDevice
//  .default(for: .video)` — the plain wide camera, ~20cm minimum focus
//  distance — with no zoom and no near-focus restriction. The puck's QR
//  (`FF_CREWCODE_QR_PX` = 170px on a 412px / 1.46" round display,
//  `firmware/app/screens/scr_settings.c`) is only ~15mm across, so at
//  the wide camera's focus distance it was too small in frame to
//  decode, and any closer than that it was out of focus — there was no
//  distance at which it was both in frame and in focus.
//
//  This file holds the parts of the fix that are plain data/logic with
//  no camera framework involved, split out specifically so
//  `FireflyAppTests` (a macOS logic-test bundle — see that target's own
//  header comment in `project.yml`, which deliberately keeps
//  SwiftUI/AVFoundation out of its sources) can exercise them. The rest
//  of the fix — actually opening `AVCaptureDevice.DiscoverySession`,
//  switching to the ultra-wide constituent, restricting focus range,
//  applying zoom, and setting `rectOfInterest` — lives in
//  `QRScannerSheet.swift` under `#if os(iOS)`, because none of it can
//  run without real camera hardware: not the Simulator (no camera at
//  all) and not this macOS test bundle. See the PR body for exactly
//  what to verify on a device.
//
//  Deliberately does NOT hold `AVCaptureDevice.DeviceType`'s raw string
//  values (`"AVCaptureDeviceTypeBuiltInTripleCamera"` and friends) as
//  its own constants: those strings are an SDK implementation detail
//  this file cannot verify without linking AVFoundation, and getting
//  one wrong would silently break device selection while still
//  "gracefully" falling back — a bug that would only ever surface on a
//  real phone. `QRScannerCameraKind` below is this file's own small,
//  closed vocabulary; `QRScannerSheet.swift` maps the three real
//  `AVCaptureDevice.DeviceType` cases onto it with a plain `switch`.
//
import CoreGraphics

/// The back-camera shapes this scanner distinguishes, most capable
/// first. A `.triple`/`.dualWide` device is a *virtual* device backed by
/// multiple physical lenses, including an ultra-wide constituent with a
/// much shorter minimum focus distance than the primary wide lens —
/// and, per Apple's docs on `AVCaptureDevice
/// .PrimaryConstituentDeviceSwitchingBehavior` (available since iOS 15,
/// well under this app's iOS 17 deployment target), the system can
/// switch to that constituent automatically for a close, out-of-focus
/// subject. `.wideAngle` cannot do this at all; it is last, as the
/// fallback every back camera is guaranteed to have.
enum QRScannerCameraKind: CaseIterable, Hashable {
    case triple
    case dualWide
    case wideAngle
}

enum QRScannerCameraConfig {
    /// Priority order, most capable first. `QRScannerCameraKind
    /// .allCases`' declaration order already matches this, but the
    /// preference is spelled out explicitly here rather than leaned on
    /// implicitly, so a future reordering of the enum's cases (for
    /// unrelated reasons — `Equatable`/`Codable` synthesis, `switch`
    /// exhaustiveness ordering, anything) cannot silently change this
    /// scanner's device preference.
    static let backCameraKindOrder: [QRScannerCameraKind] = [.triple, .dualWide, .wideAngle]

    /// Given the camera kinds a `DiscoverySession` actually found on
    /// this phone (unordered — Apple's discovery session does not
    /// promise to honor the order its `deviceTypes:` argument was
    /// declared in), return the single kind to use: the first entry of
    /// `backCameraKindOrder` that is present. `nil` means none of the
    /// three were found at all (e.g. the Simulator, which has no camera
    /// devices at all), and the caller falls back to
    /// `AVCaptureDevice.default(for: .video)`.
    static func preferredKind(among discovered: [QRScannerCameraKind]) -> QRScannerCameraKind? {
        let discovered = Set(discovered)
        return backCameraKindOrder.first { discovered.contains($0) }
    }

    /// Default zoom applied once a device is selected, before any user
    /// pinch. Measured in reasoning, not on a bench — flagged, not
    /// hidden, because there is no device in this environment:
    ///
    /// `FF_CREWCODE_QR_PX` is 170px on a 412px / 1.46" round display, so
    /// the printed code (including its own quiet zone) is roughly
    /// 1.46in × (170/412) ≈ 0.60in ≈ 15mm across. The on-screen hint
    /// this fix adds asks for "about a hand's width" (~15-20cm). At
    /// 1.0x zoom on a wide or ultra-wide field of view, a 15mm target
    /// held 15-20cm away occupies only a small fraction of the frame —
    /// too few pixels across the code's own module grid for a reliable
    /// decode, which is the "only decodes from far away" half of the
    /// owner report. 1.75x roughly halves the field of view, which:
    ///   - makes the code large enough in frame to decode at the hinted
    ///     distance, without
    ///   - cropping so tight that a slightly off-center or tilted phone
    ///     loses a finder square off the edge of frame (a QR decoder
    ///     needs all three finder squares in frame).
    /// This is a starting point, not a bench-tuned constant — the PR
    /// body asks the owner to confirm the exact distance/zoom feel on a
    /// device and says this is the first thing to retune if it is off.
    static let defaultZoomFactor: CGFloat = 1.75

    /// Clamp `preferred` into `[1.0, maximum]`. `maximum` is the real
    /// device's `activeFormat.videoMaxZoomFactor`, which varies per
    /// device/format and — for a device that only ever offers
    /// `.wideAngle`, or a degenerate/simulated format — can be less
    /// than `defaultZoomFactor`, or even non-finite/zero on a
    /// fabricated format. Never trusts a non-positive or non-finite
    /// maximum; falls back to no zoom (1.0) rather than passing a bad
    /// value to `videoZoomFactor`, which would throw at runtime.
    static func clampedZoomFactor(preferred: CGFloat = defaultZoomFactor, maximum: CGFloat) -> CGFloat {
        guard maximum.isFinite, maximum >= 1.0 else { return 1.0 }
        return min(max(preferred, 1.0), maximum)
    }
}
