//
//  Theme+SwiftUI.swift — FireflyTheme's 0xRRGGBB tokens as SwiftUI Colors.
//
//  The values themselves live in FireflyKit (and are pinned against
//  firmware/app/theme/ff_theme.h by a unit test). This file only knows
//  how to turn one into a `Color`.
//
import FireflyModel
import SwiftUI

extension Color {
    init(fireflyHex hex: UInt32) {
        self.init(
            .sRGB,
            red: Double((hex >> 16) & 0xFF) / 255.0,
            green: Double((hex >> 8) & 0xFF) / 255.0,
            blue: Double(hex & 0xFF) / 255.0,
            opacity: 1.0
        )
    }

    static let ffBackground = Color(fireflyHex: FireflyTheme.bg)
    static let ffSurface = Color(fireflyHex: FireflyTheme.surface)
    static let ffAmber = Color(fireflyHex: FireflyTheme.amber)
    static let ffLiveGreen = Color(fireflyHex: FireflyTheme.liveGreen)
    static let ffMuted = Color(fireflyHex: FireflyTheme.muted)
    /// A phone-only, lighter grey for caption/hint TEXT — NOT a
    /// puck-parity token. `FireflyTheme.muted`/`ff_theme.h`'s
    /// `FF_THEME_COLOR_MUTED` stays bit-for-bit what the puck itself
    /// paints (pinned by `ThemeTests`), so this deliberately does NOT
    /// derive from it and is never used for anything that has to match
    /// the puck's own screen (a crew swatch, a disabled-state tint).
    ///
    /// Owner decision, 2026-09-13 ("Sunlight contrast"): grey-on-black
    /// captions at caption/footnote size need more headroom outdoors
    /// than the puck's own 1.46" display does (UX review, persona B:
    /// "a lot of the actually-useful explanatory text is rendered in
    /// muted gray... exactly the combination that disappears first in
    /// bright sun"). Contrast against `.ffBackground` (`#0B0B10`):
    /// `FireflyTheme.muted` (`#8B8A97`) measures ~5.8:1 — already over
    /// WCAG AA's 4.5:1 floor for small text, but with little margin for
    /// a bright-sun glance; `#A3A2AD` measures ~7.8:1 (~7.3:1 against
    /// `.ffSurface`, `#14141C`), clearing AAA (7:1) too, same neutral
    /// hue, just lighter. Used wherever a caption/hint reads as
    /// explanatory text; a functional/disabled-state colour or a
    /// puck-matching swatch keeps `.ffMuted`.
    static let ffCaption = Color(fireflyHex: 0xA3A2AD)
    /// "app: Lineup by-stage grid, day pills, My picks" — the unselected
    /// day pill's border colour (`ff_theme.h`'s `dim`, 0x55545F);
    /// nothing before this feature needed a plain border token of its
    /// own.
    static let ffDim = Color(fireflyHex: FireflyTheme.dim)
    static let ffInk = Color(fireflyHex: FireflyTheme.ink)
    static let ffAlert = Color(fireflyHex: FireflyTheme.alert)
    /// The DEMO badge's own color (`DemoBadge.swift`) — `staleAmber`,
    /// per the task's own instruction, deliberately NOT `ffAmber`: the
    /// badge should read as a caveat stamped on the screen, not as a
    /// normal piece of live chrome sharing the app's primary accent.
    static let ffStaleAmber = Color(fireflyHex: FireflyTheme.staleAmber)
}
