//
//  SignalPresentation.swift — how a signal reading is allowed to be shown.
//
//  The Radar face has a no-GPS mode (docs/specs/S29-radio-only.md): when
//  nobody has a position, the only thing the radio actually knows is how
//  loud a neighbour is. RSSI is not distance. It varies with a body
//  between the antennas, with a pocket, with which way someone is
//  facing. Rendering "STRONG" as "about 40 m" would be inventing a
//  measurement the hardware never made.
//
//  So the tier vocabulary is closed, it is words, and
//  `SignalTierTests` asserts that no label in it can be read as a
//  distance. That test is the enforcement; this comment is only the
//  reason.
//
import FireflyCore
import Foundation

public enum SignalTierPresentation: Sendable, CaseIterable, Equatable {
    case none, faint, weak, good, strong

    public init(ffTier: ff_signal_tier_t) {
        switch ffTier {
        case FF_SIGNAL_STRONG: self = .strong
        case FF_SIGNAL_GOOD: self = .good
        case FF_SIGNAL_WEAK: self = .weak
        case FF_SIGNAL_FAINT: self = .faint
        default: self = .none
        }
    }

    /// Classify a raw dBm reading using the PUCK's own thresholds —
    /// `ff_radar_signal_tier` in firmware/core, called through the C
    /// bridge, not reimplemented in Swift.
    public static func tier(rssiDbm: Int16) -> SignalTierPresentation {
        SignalTierPresentation(ffTier: ff_radar_signal_tier(rssiDbm))
    }

    /// The only string a UI may show for a tier.
    public var label: String {
        switch self {
        case .strong: return "STRONG"
        case .good: return "GOOD"
        case .weak: return "WEAK"
        case .faint: return "FAINT"
        case .none: return "NO SIGNAL"
        }
    }

    /// Relative bar fill, 0...1 — a strength glyph, never a scale bar
    /// with units on it.
    public var barFill: Double {
        switch self {
        case .strong: return 1.0
        case .good: return 0.72
        case .weak: return 0.45
        case .faint: return 0.2
        case .none: return 0.0
        }
    }
}
