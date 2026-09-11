//
//  QRScannerSheet.swift — the camera sheet for scanning a Meshtastic
//  channel QR on iOS (docs/specs/A01-companion-app.md, Design language:
//  "QR scan on iOS via a camera sheet; paste on macOS"). macOS has no
//  camera sheet here at all — the Connect screen's paste field is the
//  whole story there, which is why everything in this file is
//  `#if os(iOS)`.
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
final class QRScannerViewController: UIViewController, AVCaptureMetadataOutputObjectsDelegate {
    private let onScanned: (String) -> Void
    private let session = AVCaptureSession()
    private var hasScanned = false
    private var previewLayer: AVCaptureVideoPreviewLayer?

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
        guard let device = AVCaptureDevice.default(for: .video),
              let input = try? AVCaptureDeviceInput(device: device),
              session.canAddInput(input) else {
            // No camera, or permission was denied — an honest empty
            // scanner rather than a crash. The paste field on this same
            // screen still works.
            return
        }
        session.addInput(input)

        let output = AVCaptureMetadataOutput()
        guard session.canAddOutput(output) else { return }
        session.addOutput(output)
        output.setMetadataObjectsDelegate(self, queue: .main)
        output.metadataObjectTypes = [.qr]

        let preview = AVCaptureVideoPreviewLayer(session: session)
        preview.videoGravity = .resizeAspectFill
        preview.frame = view.bounds
        view.layer.addSublayer(preview)
        previewLayer = preview
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        // `CALayer.autoresizingMask` is unavailable on iOS (macOS-only
        // API) — laying the preview layer out by hand on every bounds
        // change is the iOS way to keep it filling the view.
        previewLayer?.frame = view.bounds
    }

    func metadataOutput(_ output: AVCaptureMetadataOutput,
                         didOutput metadataObjects: [AVMetadataObject],
                         from connection: AVCaptureConnection) {
        guard !hasScanned,
              let object = metadataObjects.first as? AVMetadataMachineReadableCodeObject,
              object.type == .qr,
              let payload = object.stringValue else { return }
        hasScanned = true
        onScanned(payload)
    }
}
#endif
