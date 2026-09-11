//
//  HapticSignaling+UIKit.swift — the real S29 FIND haptics on iOS.
//
//  `HapticSignaling` itself lives in FireflyModel (no UIKit dependency
//  allowed there, A01: "no UI anywhere" in FireflyKit) — this is the one
//  concrete, platform-specific implementation, in the app target where
//  UIKit is already linked. macOS has no Taptic Engine at all, so it
//  gets `NoHapticSignaling` (FireflyModel) instead — see
//  `RadarView`'s composition.
//
#if os(iOS)
import FireflyModel
import UIKit

final class UIKitHapticSignaling: HapticSignaling, @unchecked Sendable {
    private let warmerGenerator = UIImpactFeedbackGenerator(style: .light)
    private let colderGenerator = UIImpactFeedbackGenerator(style: .rigid)

    func warmer() {
        DispatchQueue.main.async { self.warmerGenerator.impactOccurred() }
    }

    /// Tellable apart from `warmer()` by COUNT, not pattern — the real
    /// puck's own haptic seam has no duration/pattern parameter either
    /// (S29's own "Interpretation call" note), so two closely-spaced
    /// buzzes stand in for "a distinct, longer/lower pattern" until a
    /// richer haptic API is worth adding.
    func colder() {
        DispatchQueue.main.async {
            self.colderGenerator.impactOccurred()
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) {
                self.colderGenerator.impactOccurred()
            }
        }
    }
}
#endif
