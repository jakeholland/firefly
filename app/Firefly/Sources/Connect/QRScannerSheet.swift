//
//  QRScannerSheet.swift — the camera sheet for scanning a Meshtastic
//  channel QR on iOS (docs/specs/A01-companion-app.md, Design language:
//  "QR scan on iOS via a camera sheet; paste on macOS"). macOS has no
//  camera sheet here at all — the Connect screen's paste field is the
//  whole story there, which is why everything in this file is
//  `#if os(iOS)`.
//
//  Macro/near-focus fix (owner report, iPhone 17 Pro Max, 2026-09-15):
//  on the Join-a-crew screen (`docs/specs/A02-crew-join.md` §3.1,
//  reusing this controller inline via `CrewJoinView`'s
//  `CrewScannerCard`) the scanner went blurry up close and only decoded
//  from far away. `AVCaptureDevice.default(for: .video)` picks the
//  plain wide camera, whose minimum focus distance (~20cm) put the
//  puck's QR (`FF_CREWCODE_QR_PX` = 170px on a 412px / 1.46" round
//  display, `firmware/app/screens/scr_settings.c` — ~15mm across on
//  glass) too small in frame at focus distance, and out of focus any
//  closer. `configureSession()` below now (1) prefers a virtual
//  multi-lens back camera and lets it switch to its ultra-wide
//  constituent for close subjects, (2) restricts autofocus to the near
//  range, (3) applies a modest default zoom, and (4) narrows the
//  decode region to the on-screen guide box. See `QRScannerCameraConfig
//  .swift` for the pure selection/zoom logic and its own reasoning
//  comments, and the PR body for what a device is needed to verify.
//
#if os(iOS)
import AVFoundation
import SwiftUI

struct QRScannerSheet: View {
    let onScanned: (String) -> Void
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            QRScannerRepresentable { payload in
                onScanned(payload)
                dismiss()
            }
            .background(Color.ffBackground)
            .navigationTitle("SCAN CHANNEL")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
            }
        }
    }
}

private struct QRScannerRepresentable: UIViewControllerRepresentable {
    let onScanned: (String) -> Void

    func makeUIViewController(context: Context) -> QRScannerViewController {
        QRScannerViewController(onScanned: onScanned)
    }

    func updateUIViewController(_ uiViewController: QRScannerViewController, context: Context) {}
}

/// A minimal `AVCaptureMetadataOutput` QR reader. No dependency beyond
/// AVFoundation — this is a real scanner, not a placeholder, but it is
/// deliberately narrow: one delegate callback, one payload, done.
///
/// M3 / Swift 6: `@preconcurrency` on the delegate conformance below —
/// `AVCaptureMetadataOutputObjectsDelegate` is a nonisolated
/// Objective-C protocol, but `configureSession()` registers this
/// delegate with `queue: .main` explicitly (not the default private
/// AVFoundation queue), so `metadataOutput(_:didOutput:from:)` always
/// actually runs on the main actor this `UIViewController` subclass is
/// already isolated to. `@preconcurrency` documents that guarantee at
/// the one place it is made rather than fighting the framework's
/// un-isolated protocol declaration.
final class QRScannerViewController: UIViewController, @preconcurrency AVCaptureMetadataOutputObjectsDelegate {
    private let onScanned: (String) -> Void
    private let session = AVCaptureSession()
    /// A02 §3.1: "A failed scan never dismisses the screen" — the inline
    /// Join scanner (`CrewJoinView`) keeps this controller on screen and
    /// calling `onScanned` again after an unrecognized payload, so this
    /// is a THROTTLE (same payload, or any payload within `rescanDelay`)
    /// rather than the one-shot latch this used to be. `QRScannerSheet`
    /// (the modal channel-link scanner) is unaffected: its own
    /// `onScanned` closure calls `dismiss()` on the very first delivery,
    /// so it never reaches a second one regardless.
    private var lastScan: (payload: String, at: Date)?
    private static let rescanDelay: TimeInterval = 1.5
    private var previewLayer: AVCaptureVideoPreviewLayer?
    /// The visible square the user is told to hold the code inside.
    /// Doubles as the source rect for `rectOfInterest` (below) so the
    /// decode region always matches what's drawn on screen.
    private var guideBoxLayer: CAShapeLayer?
    private var metadataOutput: AVCaptureMetadataOutput?

    init(onScanned: @escaping (String) -> Void) {
        self.onScanned = onScanned
        super.init(nibName: nil, bundle: nil)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black
        configureSession()
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        if !session.isRunning {
            DispatchQueue.global(qos: .userInitiated).async { [session] in
                session.startRunning()
            }
        }
    }

    override func viewWillDisappear(_ animated: Bool) {
        super.viewWillDisappear(animated)
        if session.isRunning { session.stopRunning() }
    }

    private func configureSession() {
        guard let device = Self.selectBackCamera(),
              let input = try? AVCaptureDeviceInput(device: device),
              session.canAddInput(input) else {
            // No camera, or permission was denied — an honest empty
            // scanner rather than a crash. The paste field on this same
            // screen still works.
            return
        }
        Self.applyMacroFocusConfiguration(to: device)
        session.addInput(input)

        let output = AVCaptureMetadataOutput()
        guard session.canAddOutput(output) else { return }
        session.addOutput(output)
        output.setMetadataObjectsDelegate(self, queue: .main)
        output.metadataObjectTypes = [.qr]
        metadataOutput = output

        let preview = AVCaptureVideoPreviewLayer(session: session)
        preview.videoGravity = .resizeAspectFill
        preview.frame = view.bounds
        view.layer.addSublayer(preview)
        previewLayer = preview

        let guideBox = CAShapeLayer()
        guideBox.fillColor = UIColor.clear.cgColor
        guideBox.strokeColor = UIColor.white.withAlphaComponent(0.9).cgColor
        guideBox.lineWidth = 2
        view.layer.addSublayer(guideBox)
        guideBoxLayer = guideBox

        layoutPreviewAndGuideBox()
    }

    /// (1) Prefer a virtual multi-lens back camera (`.builtInTripleCamera`
    /// / `.builtInDualWideCamera`) over the plain `.builtInWideAngleCamera`
    /// — see `QRScannerCameraKind`'s doc comment for why a virtual
    /// device is what makes macro switching possible at all.
    /// `DiscoverySession.devices` does not promise to honor
    /// `deviceTypes:`'s declaration order, so this maps every discovered
    /// device onto `QRScannerCameraKind` and asks the pure, tested
    /// `QRScannerCameraConfig.preferredKind(among:)` which one to use,
    /// rather than re-deriving the priority here.
    private static func selectBackCamera() -> AVCaptureDevice? {
        let discovery = AVCaptureDevice.DiscoverySession(
            deviceTypes: [.builtInTripleCamera, .builtInDualWideCamera, .builtInWideAngleCamera],
            mediaType: .video,
            position: .back)
        var deviceByKind: [QRScannerCameraKind: AVCaptureDevice] = [:]
        for device in discovery.devices {
            guard let kind = kind(for: device.deviceType) else { continue }
            deviceByKind[kind] = device
        }
        if let preferredKind = QRScannerCameraConfig.preferredKind(among: Array(deviceByKind.keys)) {
            return deviceByKind[preferredKind]
        }
        // The Simulator (no camera devices at all) or any back camera
        // this discovery session's three types don't recognize — the
        // same lookup every camera feature in this codebase used before
        // this fix, kept as the last-resort fallback.
        return AVCaptureDevice.default(for: .video)
    }

    private static func kind(for deviceType: AVCaptureDevice.DeviceType) -> QRScannerCameraKind? {
        switch deviceType {
        case .builtInTripleCamera: return .triple
        case .builtInDualWideCamera: return .dualWide
        case .builtInWideAngleCamera: return .wideAngle
        default: return nil
        }
    }

    /// Everything here is best-effort: `lockForConfiguration()` can
    /// fail (device already locked elsewhere), and every property is
    /// individually guarded by its own "is this supported" check, so a
    /// device missing any one of these capabilities still scans — just
    /// without that particular improvement. Nothing here is required
    /// for the scanner to work at all; it is all about making the
    /// puck's small, close QR decode reliably.
    private static func applyMacroFocusConfiguration(to device: AVCaptureDevice) {
        do {
            try device.lockForConfiguration()
            defer { device.unlockForConfiguration() }

            // (1) Macro switching: let a virtual (multi-lens) device
            // switch to its ultra-wide constituent automatically for a
            // close, out-of-focus subject. Apple's own documented
            // pattern for `setPrimaryConstituentDeviceSwitchingBehavior`
            // is to check `activePrimaryConstituentDeviceSwitchingBehavior
            // != .unsupported` first — true for a plain
            // `.builtInWideAngleCamera` (no constituents to switch
            // among) and possibly other devices/OS combinations — so
            // this call is never made where it would be rejected.
            // Available since iOS 15, under this app's iOS 17
            // deployment target, so no `#available` gate is needed.
            if device.activePrimaryConstituentDeviceSwitchingBehavior != .unsupported {
                device.setPrimaryConstituentDeviceSwitchingBehavior(.auto, restrictedSwitchingBehaviorConditions: [])
            }

            // (2) Bias focus toward near subjects, continuous autofocus
            // — the puck is always held close, never at landscape
            // distance, so there is no reason to ever hunt the far end
            // of the focus range.
            if device.isAutoFocusRangeRestrictionSupported {
                device.autoFocusRangeRestriction = .near
            }
            if device.isFocusModeSupported(.continuousAutoFocus) {
                device.focusMode = .continuousAutoFocus
            }

            // (3) Modest default zoom. See `QRScannerCameraConfig
            // .defaultZoomFactor`'s doc comment for the measured
            // trade-off; clamped to what this device/format actually
            // supports.
            device.videoZoomFactor = QRScannerCameraConfig.clampedZoomFactor(
                maximum: device.activeFormat.videoMaxZoomFactor)
        } catch {
            // No configuration lock — the scanner still runs at
            // whatever the device's own defaults are, same
            // graceful-fallback shape as `configureSession()`'s own "no
            // camera at all" guard above.
        }
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        layoutPreviewAndGuideBox()
    }

    private func layoutPreviewAndGuideBox() {
        // `CALayer.autoresizingMask` is unavailable on iOS (macOS-only
        // API) — laying these out by hand on every bounds change is the
        // iOS way to keep them tracking the view.
        previewLayer?.frame = view.bounds
        guard let guideBoxLayer else { return }
        let boxFrame = Self.guideBoxFrame(in: view.bounds)
        guideBoxLayer.frame = boxFrame
        guideBoxLayer.path = UIBezierPath(roundedRect: CGRect(origin: .zero, size: boxFrame.size),
                                           cornerRadius: 12).cgPath
        updateRectOfInterest()
    }

    /// A centered square, 62% of the shorter side — big enough that a
    /// hand holding a phone steady doesn't clip the puck's QR out of
    /// it, small enough to visibly narrow the decode region for (4)
    /// below.
    private static func guideBoxFrame(in bounds: CGRect) -> CGRect {
        let side = min(bounds.width, bounds.height) * 0.62
        return CGRect(x: bounds.midX - side / 2, y: bounds.midY - side / 2, width: side, height: side)
    }

    /// (4) Centers the decode region on the visible guide box.
    /// `AVCaptureMetadataOutput.rectOfInterest` uses a rotated,
    /// normalized coordinate space that does not match the preview
    /// layer's own view coordinates —
    /// `metadataOutputRectConverted(fromLayerRect:)` is the
    /// AVFoundation-provided conversion Apple documents for exactly
    /// this, used here instead of re-deriving that rotation math by
    /// hand.
    private func updateRectOfInterest() {
        guard let previewLayer, let metadataOutput, let guideBoxLayer,
              guideBoxLayer.frame.width > 0, guideBoxLayer.frame.height > 0 else { return }
        metadataOutput.rectOfInterest = previewLayer.metadataOutputRectConverted(fromLayerRect: guideBoxLayer.frame)
    }

    func metadataOutput(_ output: AVCaptureMetadataOutput,
                         didOutput metadataObjects: [AVMetadataObject],
                         from connection: AVCaptureConnection) {
        guard let object = metadataObjects.first as? AVMetadataMachineReadableCodeObject,
              object.type == .qr,
              let payload = object.stringValue else { return }
        let now = Date()
        if let lastScan, lastScan.payload == payload, now.timeIntervalSince(lastScan.at) < Self.rescanDelay {
            return
        }
        lastScan = (payload, now)
        onScanned(payload)
    }
}
#endif
