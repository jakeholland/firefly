//
//  HeadingProviding.swift — the compass seam (docs/specs/
//  A01-companion-app.md, "Compass heading", and S5).
//
//  Landed here, un-owned by any single slice, because slice D (Radar,
//  which renders NOHDG when heading is unavailable) and slice F (which
//  owns the CoreLocation implementation on iOS and the permanent
//  no-magnetometer stand-in on macOS) would otherwise each invent this
//  protocol independently and disagree about its shape.
//
import Foundation

/// One heading reading. A negative `accuracyDegrees` means invalid —
/// `ff_radar_compute`'s own `arrow_valid` flag already expresses this,
/// and the app must feed it honestly rather than rendering a confidently
/// wrong arrow.
public struct HeadingReading: Sendable, Equatable {
    public let headingDegrees: Double
    public let accuracyDegrees: Double

    public init(headingDegrees: Double, accuracyDegrees: Double) {
        self.headingDegrees = headingDegrees
        self.accuracyDegrees = accuracyDegrees
    }

    /// `accuracyDegrees < 0` per CoreLocation's own convention for "no
    /// usable heading".
    public var isValid: Bool { accuracyDegrees >= 0 }
}

public protocol HeadingProviding: AnyObject, Sendable {
    /// A fresh, independent stream for the caller (S1's multicast rule).
    /// `nil` means NOHDG: no magnetometer (macOS, permanently) or no
    /// valid reading yet (iOS, transiently). Radar must render NOHDG,
    /// never a stuck or fabricated arrow.
    func headings() -> AsyncStream<HeadingReading?>
}

/// The permanent macOS answer (no magnetometer — see
/// docs/hardware/heltec-v3.md and the puck's own `RADAR_NOHDG` mode)
/// and the M1/unit-test default everywhere else. Yields `nil` forever.
/// This is correct, not a gap: running a tilt-compensated fusion on top
/// of CoreLocation's own would be two filters fighting, and the spec
/// says so explicitly.
public final class NoHeadingProvider: HeadingProviding, @unchecked Sendable {
    public init() {}
    public func headings() -> AsyncStream<HeadingReading?> {
        AsyncStream { continuation in
            continuation.yield(nil)
            continuation.finish()
        }
    }
}
