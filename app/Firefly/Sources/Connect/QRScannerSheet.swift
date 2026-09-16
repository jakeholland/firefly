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
//  range, and (3) applies a modest default zoom **only on a plain
//  single-lens device** (a virtual device is left at its own minimum
//  zoom instead, so `.auto` constituent switching stays free to engage
//  — see `QRScannerCameraConfig.targetZoomFactor`). See
//  `QRScannerCameraConfig.swift` for the pure selection/zoom logic and
//  its own reasoning comments, and the PR body for what a device is
//  needed to verify.
//
//  Empty-`rectOfInterest` fix (owner report, iPhone 17 Pro Max,
//  TestFlight 347, 2026-09-16): the macro fix above ALSO added a (4)
//  that narrowed `AVCaptureMetadataOutput.rectOfInterest` to the
//  on-screen guide box, via `updateRectOfInterest()` called from
//  `viewDidLayoutSubviews()`. That runs from `viewDidLoad()`'s initial
//  layout pass — BEFORE `viewDidAppear()` kicks off
//  `session.startRunning()` (on a background queue, below).
//  `AVCaptureVideoPreviewLayer.metadataOutputRectConverted(fromLayerRect:)`
//  converts through the preview layer's `AVCaptureConnection`, which
//  does not exist until the session is running — so every call before
//  that point (including the one at first layout, well before
//  `viewDidAppear`, and every rotation before the user has scrolled to
//  this screen) produced an empty or degenerate rect. `rectOfInterest`
//  latched to that; nothing ever recomputed it once the session
//  actually started, because layout doesn't fire again on its own. A
//  QR dead center in a crisp, correctly focused frame was silently
//  outside the decode region and every `AVMetadataObject` was
//  discarded before `metadataOutput(_:didOutput:from:)` ever saw it —
//  no error, no signal, nothing to diagnose short of reading this
//  exact call chain. The `rectOfInterest` restriction is removed
//  entirely below: the guide box is now a purely visual hint, and the
//  output decodes the full frame (`rectOfInterest`'s documented
//  default), which is strictly more robust for a single code on a
//  puck screen than a narrowed region that can go stale or come out
//  empty. `#if DEBUG` diagnostics were added at the points this bug
//  had none, so a future version of this failure logs instead of
//  scanning in silence — see `log(_:)` below.
//
#if os(iOS)
import AVFoundation
import Foundation
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
    /// The visible square the user is told to hold the code inside —
    /// purely a visual hint since the `rectOfInterest` fix above; the
    /// decoder itself reads the full frame regardless of where this is
    /// drawn.
    private var guideBoxLayer: CAShapeLayer?
    #if DEBUG
    /// Kept only for `logSessionStartDiagnostics()` below — nothing in
    /// the non-debug path needs the device again once
    /// `configureSession()` has used it.
    private var debugCaptureDevice: AVCaptureDevice?
    #endif

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
            #if DEBUG
            logSessionStartDiagnostics()
            #endif
            DispatchQueue.global(qos: .userInitiated).async { [session] in
                session.startRunning()
                #if DEBUG
                Self.log("session.startRunning() completed, isRunning=\(session.isRunning)")
                #endif
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
        #if DEBUG
        debugCaptureDevice = device
        #endif

        let output = AVCaptureMetadataOutput()
        guard session.canAddOutput(output) else { return }
        session.addOutput(output)
        output.setMetadataObjectsDelegate(self, queue: .main)
        output.metadataObjectTypes = [.qr]
        // No `rectOfInterest` assignment — see this file's header
        // comment ("Empty-`rectOfInterest` fix"). Left at its
        // documented default (the full frame): the conversion this
        // used to compute is only valid once the session's capture
        // connection exists, i.e. after `startRunning()`, and this
        // layout pass runs before that. A full-frame decode with a
        // purely visual guide box is strictly more robust for a single
        // code on a puck screen than a narrowed region that can come
        // out empty and silently discard every detection.

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

            // (3) Zoom — NOT the same value for every device. See
            // `QRScannerCameraConfig.targetZoomFactor`'s doc comment:
            // `videoZoomFactor` is what gates which physical constituent
            // a virtual (multi-lens) device is using
            // (`virtualDeviceSwitchOverVideoZoomFactors`), so pinning it
            // to a fixed digital-zoom value on a virtual device risks
            // sitting at/above that device's own switch-over threshold
            // and permanently ruling out the ultra-wide constituent —
            // defeating the `.auto` switching just enabled in (1) above.
            // `device.isVirtualDevice` is the real gate: virtual devices
            // are left at their own minimum (letting `.auto` switching
            // do the work); only a plain single-lens device gets the
            // digital zoom punch-in, since it has no constituent to
            // switch to and zoom is the only lever available.
            device.videoZoomFactor = QRScannerCameraConfig.targetZoomFactor(
                isVirtualDevice: device.isVirtualDevice,
                minimum: device.minAvailableVideoZoomFactor,
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
    }

    /// A centered square, 62% of the shorter side — big enough that a
    /// hand holding a phone steady doesn't clip the puck's QR out of
    /// it. Purely a visual hint (see this file's header comment,
    /// "Empty-`rectOfInterest` fix") — it no longer narrows what the
    /// decoder reads.
    private static func guideBoxFrame(in bounds: CGRect) -> CGRect {
        let side = min(bounds.width, bounds.height) * 0.62
        return CGRect(x: bounds.midX - side / 2, y: bounds.midY - side / 2, width: side, height: side)
    }

    func metadataOutput(_ output: AVCaptureMetadataOutput,
                         didOutput metadataObjects: [AVMetadataObject],
                         from connection: AVCaptureConnection) {
        #if DEBUG
        Self.logMetadataOutputCall(metadataObjects)
        #endif
        guard let object = metadataObjects.first as? AVMetadataMachineReadableCodeObject,
              object.type == .qr,
              let payload = object.stringValue else { return }
        let now = Date()
        if let lastScan, lastScan.payload == payload, now.timeIntervalSince(lastScan.at) < Self.rescanDelay {
            #if DEBUG
            Self.log("throttled: same payload (\(payload.count) chars) within \(Self.rescanDelay)s")
            #endif
            return
        }
        lastScan = (payload, now)
        #if DEBUG
        Self.log("delivered: payload (\(payload.count) chars)")
        #endif
        onScanned(payload)
    }

    #if DEBUG
    // MARK: - `[QRScanner]` diagnostics (DEBUG only)
    //
    // Added chasing the "focus and framing both look right but it
    // never decodes" owner report (see this file's header comment) —
    // that bug had NO signal anywhere: `rectOfInterest` silently
    // discarded every `AVMetadataObject` before `metadataOutput
    // (_:didOutput:from:)` was ever called, so there was nothing to
    // read short of tracing this exact call chain by hand. Every line
    // here exists to make the next version of "decodes nothing" a few
    // seconds of `stderr` instead of a repeat of that. Never logs a
    // scanned payload's contents, only its length — this is a
    // diagnostics channel, not a place a QR's actual data (which can
    // carry a crew code or anything else a puck chooses to display)
    // should end up.
    //
    /// Same shape as `MeshtasticClient.log(_:)`: a raw
    /// `FileHandle.standardError.write`, never `print()`, so a line is
    /// never lost to stdout's full block-buffering.
    private static func log(_ message: String) {
        let line = "[QRScanner] \(message)\n"
        FileHandle.standardError.write(Data(line.utf8))
    }

    /// Logged right before `session.startRunning()` is dispatched —
    /// everything here reflects `configureSession()`'s choices, so
    /// this is the one place to see, on a real device, exactly what
    /// camera and settings the macro fix (this file's own earlier
    /// header section) actually landed on.
    private func logSessionStartDiagnostics() {
        guard let device = debugCaptureDevice else {
            Self.log("session start: no capture device (no camera, or permission denied)")
            return
        }
        let focusRange = device.isAutoFocusRangeRestrictionSupported
            ? "\(device.autoFocusRangeRestriction.rawValue)" : "unsupported"
        let primaryConstituent = device.activePrimaryConstituent?.localizedName ?? "none"
        // `AVCaptureMetadataOutput.rectOfInterest` defaults to the full
        // frame (`{{0, 0}, {1, 1}}`) and this file never assigns it —
        // logged anyway so a future regression that DOES reassign it is
        // visible here rather than rediscovered the hard way again.
        let effectiveRectOfInterest = session.outputs
            .compactMap { $0 as? AVCaptureMetadataOutput }
            .first.map { "\($0.rectOfInterest)" } ?? "n/a"
        Self.log("session start: device=\(device.localizedName) isVirtualDevice=\(device.isVirtualDevice) "
                 + "videoZoomFactor=\(device.videoZoomFactor) activePrimaryConstituent=\(primaryConstituent) "
                 + "focusRangeRestriction=\(focusRange) rectOfInterest=\(effectiveRectOfInterest)")
    }

    private static func logMetadataOutputCall(_ metadataObjects: [AVMetadataObject]) {
        let types = metadataObjects.map { $0.type.rawValue }
        let hasQRString = metadataObjects.contains {
            ($0 as? AVMetadataMachineReadableCodeObject)?.type == .qr
                && ($0 as? AVMetadataMachineReadableCodeObject)?.stringValue != nil
        }
        log("metadataOutput: count=\(metadataObjects.count) types=\(types) hasQRString=\(hasQRString)")
    }
    #endif
}
#endif
