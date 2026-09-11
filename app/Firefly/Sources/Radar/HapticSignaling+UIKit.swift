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
    private let flareGenerator = UINotificationFeedbackGenerator()

    func warmer() {
        DispatchQueue.main.async { self.warmerGenerator.impactOccurred() }
    }

    /// M2, S10: "haptic pattern (3 long) — overrides quiet hours." Same
    /// tellable-apart-by-count convention `colder()` already uses (this
    /// file's own note on `HapticSignaling`'s lack of a duration/pattern
    /// parameter) — three heavy pulses, spaced enough to read as
    /// distinct beats rather than one long buzz.
    func flareAlert() {
        DispatchQueue.main.async {
            self.flareGenerator.notificationOccurred(.warning)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
                self.flareGenerator.notificationOccurred(.warning)
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) {
                self.flareGenerator.notificationOccurred(.warning)
            }
        }
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
