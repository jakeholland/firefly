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
    static let ffInk = Color(fireflyHex: FireflyTheme.ink)
    static let ffAlert = Color(fireflyHex: FireflyTheme.alert)
    /// The DEMO badge's own color (`DemoBadge.swift`) — `staleAmber`,
    /// per the task's own instruction, deliberately NOT `ffAmber`: the
    /// badge should read as a caveat stamped on the screen, not as a
    /// normal piece of live chrome sharing the app's primary accent.
    static let ffStaleAmber = Color(fireflyHex: FireflyTheme.staleAmber)
}
