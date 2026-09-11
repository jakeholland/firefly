//
//  FireflyTheme.swift — the puck's palette, on the phone.
//
//  Values are transcribed from firmware/app/theme/ff_theme.h, the single
//  source of truth, and a unit test pins them so a drift on either side
//  is a failing test rather than two products that look almost alike.
//  Geometry (412 px panel, glass offset, hit-target floor) deliberately
//  does NOT come across — that is a round 1.85" display's problem, not a
//  phone's.
//
import Foundation

public enum FireflyTheme {
    /// 0xRRGGBB, exactly as `ff_theme.h` states them.
    public static let bg: UInt32 = 0x0B0B10
    public static let surface: UInt32 = 0x14141C
    public static let amber: UInt32 = 0xFFC66B
    public static let staleAmber: UInt32 = 0xFFB454
    public static let liveGreen: UInt32 = 0x9BE07B
    public static let muted: UInt32 = 0x8B8A97
    public static let dim: UInt32 = 0x5554_5F
    public static let ink: UInt32 = 0xF2EFE6

    /// The app's error/danger token. `ff_theme.h` has no generic error
    /// colour of its own — `FF_THEME_MAP_MEDICAL` (0xFF6B6B) is a map-only
    /// one-off red, deliberately not exposed here. `staleAmber` is what the
    /// puck actually paints for its alert states (NO MESH, low battery), so
    /// that is what this derives from rather than inventing a new hex the
    /// puck has never shown (PR #262 review, SHOULD-FIX 3).
    public static let alert: UInt32 = staleAmber

    /// `ff_theme_crew_color`'s palette, in order — `color_idx` from
    /// `ff_crew_member_t` indexes straight into this.
    public static let crew: [UInt32] = [
        0xFF5CA8, // PINK
        0x4FD8C4, // TEAL
        0xB08CFF, // VIOLET
        0x9BE07B, // GREEN
        0xF96306, // ORANGE
        0xD3E05C, // GOLD
        0x7690E5, // BLUE
        0xF906F9, // MAGENTA
    ]

    public static func crewColor(index: Int) -> UInt32 {
        crew[((index % crew.count) + crew.count) % crew.count]
    }
}
