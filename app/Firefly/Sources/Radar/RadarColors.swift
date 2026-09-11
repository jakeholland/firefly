//
//  RadarColors.swift — Slice D's own Color tokens.
//
//  `Theme+SwiftUI.swift` (top-level `Firefly/Sources/`) is not part of
//  this slice's file list, and it doesn't carry every token Radar needs
//  (stale amber, surface, dim, crew colors) — rather than edit a file
//  this slice doesn't own, the extra tokens this screen needs live here,
//  reading straight from the same `FireflyTheme`/`RadarCrewPalette`
//  values every other Firefly screen is pinned against.
//
import FireflyModel
import SwiftUI

extension Color {
    static let radarSurface = Color(fireflyHex: FireflyTheme.surface)
    static let radarStaleAmber = Color(fireflyHex: FireflyTheme.staleAmber)
    static let radarDim = Color(fireflyHex: FireflyTheme.dim)

    static func radarCrew(index: Int, colorblind: Bool) -> Color {
        Color(fireflyHex: RadarCrewPalette.hex(index: index, colorblind: colorblind))
    }
}
