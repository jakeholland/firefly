//
//  ArrowGeometry.swift — the compass-arrow's shape, as plain geometry.
//
//  Split out of `RadarRingView.swift`'s `ArrowShape` (same reasoning as
//  every other file in `FireflyAppTests`'s explicit source list — see
//  `app/project.yml`'s comments there): this file is CoreGraphics-only,
//  no `import SwiftUI`, specifically so `FireflyAppTests` can pin the
//  shape's actual path points (normalized geometry, notch position,
//  width ratio) without dragging in SwiftUI. `ArrowShape.path(in:)`
//  calls straight into `points(in:)` below, so there is exactly one
//  place this geometry is defined — the render code and the test both
//  read the same numbers.
//
//  docs/design/compass-arrow.md is the shared spec both this file and
//  the puck's `radar_layout.c` implement from: a notched dart, tip at
//  +0.5*L along the bearing, base corners at -0.5*L offset +/-0.5*W
//  perpendicular, notch on-axis at -0.25*L (0.75*L back from the tip).
//  This file expresses that in `ArrowShape`'s own rect convention (tip
//  at top-centre, base at the bottom edge) rather than centre-relative
//  offsets, since that's the convention `path(in:)` already used before
//  this change and the two are equivalent up to a Y-flip + translation.
//
import CoreGraphics

enum ArrowGeometry {
    /// W / L — "a tad fatter" (owner, 2026-09-15). Shared with the
    /// puck's `RADAR_LAYOUT_ARROW_WIDTH_RATIO`. The previous shape used
    /// a fixed 28pt width, which worked out to roughly 0.19 of a
    /// typical L; this is about 20% fatter than that.
    static let widthRatio: CGFloat = 0.23

    /// The dart's four outline points for a `width x length` rect, in
    /// the order the outline is drawn (tip -> right -> notch -> left,
    /// closing back to tip): tip at top-centre, base corners at the
    /// bottom two corners, notch at 0.75 of the height (0.75*L back
    /// from the tip, on the vertical centre-line, per
    /// docs/design/compass-arrow.md).
    static func points(in rect: CGRect) -> (tip: CGPoint, right: CGPoint, notch: CGPoint, left: CGPoint) {
        (
            tip: CGPoint(x: rect.midX, y: rect.minY),
            right: CGPoint(x: rect.maxX, y: rect.maxY),
            notch: CGPoint(x: rect.midX, y: rect.maxY * 0.75),
            left: CGPoint(x: rect.minX, y: rect.maxY)
        )
    }
}
