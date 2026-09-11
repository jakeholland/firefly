//
//  MapColors.swift — Map tab slice's own Color tokens.
//
//  `Theme+SwiftUI.swift` (top-level `Firefly/Sources/`) doesn't carry
//  the festpack feature-kind palette — rather than edit a shared file
//  this slice doesn't own, the extra tokens this screen needs live
//  here, same convention `Radar/RadarColors.swift` already set for
//  itself. Values are `FieldMapProjector.kindColorHex` (FireflyModel),
//  itself transcribed from `firmware/app/theme/ff_theme.h`'s
//  `FF_THEME_MAP_*` block — one source of truth, this file only knows
//  how to turn a hex into a `Color`.
//
import FireflyModel
import SwiftUI

extension Color {
    static func mapKind(_ kind: FestpackFeatureKind) -> Color {
        Color(fireflyHex: FieldMapProjector.kindColorHex(kind))
    }

    static func mapFeature(hex: UInt32) -> Color {
        Color(fireflyHex: hex)
    }

    static func mapCrew(colorIndex: UInt8) -> Color {
        Color(fireflyHex: FireflyTheme.crewColor(index: Int(colorIndex)))
    }

    /// The blue "you" dot — deliberately NOT one of the crew/theme
    /// tokens above: a system-blue "you are here" marker is the
    /// convention every map app's own location dot already uses, and
    /// reusing it here means "that's me" reads instantly rather than
    /// looking like an unassigned ninth crew color.
    static let mapYou = Color.blue
}
