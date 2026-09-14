//
//  CrewQRCode.swift — renders a `firefly://crew` deep link as a QR
//  image using CoreImage's built-in `CIQRCodeGenerator` (`docs/specs/
//  A02-crew-join.md`, §1.8: "generate with CoreImage `CIQRCodeGenerator`,
//  no external lib").
//
import CoreImage.CIFilterBuiltins
import SwiftUI

enum CrewQRCode {
    /// `nil` only if CoreImage itself fails to render — this app has no
    /// fallback QR renderer, so callers show the code in big mono text
    /// either way (the artboard's own belt-and-suspenders: the QR is
    /// never the ONLY way to get the code).
    static func image(for text: String, scale: CGFloat = 10) -> CGImage? {
        let context = CIContext()
        let filter = CIFilter.qrCodeGenerator()
        filter.message = Data(text.utf8)
        filter.correctionLevel = "M"
        guard let output = filter.outputImage else { return nil }
        let transformed = output.transformed(by: CGAffineTransform(scaleX: scale, y: scale))
        return context.createCGImage(transformed, from: transformed.extent)
    }
}

/// A SwiftUI wrapper — falls back to a plain placeholder square (never
/// a crash) if `CrewQRCode.image(for:)` returns `nil`.
struct CrewQRCodeView: View {
    let text: String

    var body: some View {
        if let cgImage = CrewQRCode.image(for: text) {
            Image(decorative: cgImage, scale: 1, orientation: .up)
                .interpolation(.none)
                .resizable()
                .scaledToFit()
        } else {
            RoundedRectangle(cornerRadius: 8)
                .fill(Color.ffSurface)
                .overlay(Text("QR unavailable").font(.caption).foregroundStyle(Color.ffMuted))
        }
    }
}
