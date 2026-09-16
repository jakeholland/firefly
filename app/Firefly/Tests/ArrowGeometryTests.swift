//
//  ArrowGeometryTests.swift — the compass-arrow's shape
//  (docs/design/compass-arrow.md, 2026-09-15 one-compass-arrow). Run via
//  `xcodebuild test -only-testing:FireflyAppTests` (see project.yml) —
//  same reason as `MoreScreenNavigationTests`' own header comment.
//
//  Pins the normalized geometry directly (tip/base/notch positions,
//  width ratio) rather than trusting a snapshot alone to notice a
//  regression — a snapshot only tells you SOMETHING changed, not that
//  the actual numbers still match docs/design/compass-arrow.md's shared
//  spec, which the puck's `test_radar_layout.c` implements the same
//  checks against on its own geometry.
//
import CoreGraphics
import XCTest

final class ArrowGeometryTests: XCTestCase {
    /// A convenient large rect so pixel-rounding in the assertions'
    /// tolerances doesn't matter: width = widthRatio * height, exactly
    /// as `RadarRingView.swift`'s `arrow(center:radius:)` sizes the real
    /// frame.
    private let length: CGFloat = 1000
    private lazy var rect = CGRect(x: 0, y: 0, width: ArrowGeometry.widthRatio * length, height: length)

    func testWidthRatioIsTheOwnersFatterValue() {
        // "Maybe align both to be a tad fatter" (owner, 2026-09-15) —
        // 0.23, shared with the puck's RADAR_LAYOUT_ARROW_WIDTH_RATIO,
        // up from the previous fixed-28pt shape's ~0.19.
        XCTAssertEqual(ArrowGeometry.widthRatio, 0.23, accuracy: 0.0001)
    }

    func testTipIsAtTopCentre() {
        let pts = ArrowGeometry.points(in: rect)
        XCTAssertEqual(pts.tip.x, rect.midX)
        XCTAssertEqual(pts.tip.y, rect.minY)
    }

    func testBaseCornersAreSymmetricAboutTheCentreLineAtTheBottomEdge() {
        let pts = ArrowGeometry.points(in: rect)
        XCTAssertEqual(pts.left.y, rect.maxY)
        XCTAssertEqual(pts.right.y, rect.maxY)
        XCTAssertEqual(pts.left.x, rect.minX)
        XCTAssertEqual(pts.right.x, rect.maxX)
        // Symmetric about the vertical centre-line, i.e. each corner is
        // exactly half the rect's width from midX — this is what makes
        // the base "+/-0.5*W perpendicular" in docs/design/compass-arrow.md.
        XCTAssertEqual(rect.midX - pts.left.x, pts.right.x - rect.midX, accuracy: 0.0001)
        XCTAssertEqual(rect.midX - pts.left.x, rect.width / 2, accuracy: 0.0001)
    }

    func testNotchSitsOnTheAxisAtThreeQuartersOfTheLengthFromTheTip() {
        let pts = ArrowGeometry.points(in: rect)
        // On-axis: no perpendicular offset from the tip/base centre-line.
        XCTAssertEqual(pts.notch.x, rect.midX)
        // docs/design/compass-arrow.md: notch at -0.25*L from centre,
        // i.e. 0.75*L back from the tip. In this rect's convention (tip
        // at minY, base at maxY, height == L) that's maxY * 0.75.
        XCTAssertEqual(pts.notch.y, rect.maxY * 0.75, accuracy: 0.0001)
        let tipToNotch = pts.notch.y - pts.tip.y
        let tipToBase = pts.left.y - pts.tip.y
        XCTAssertEqual(tipToNotch / tipToBase, 0.75, accuracy: 0.0001)
    }

    /// The whole point of a NOTCHED dart rather than a plain triangle:
    /// the notch must sit strictly between the tip and the base line,
    /// pulled forward (a smaller y than the base, in this rect's
    /// convention) — otherwise it wouldn't read as a concave notch at
    /// all.
    func testNotchIsStrictlyBetweenTipAndBase() {
        let pts = ArrowGeometry.points(in: rect)
        XCTAssertGreaterThan(pts.notch.y, pts.tip.y)
        XCTAssertLessThan(pts.notch.y, pts.left.y)
    }

    func testGeometryScalesWithTheRectRegardlessOfSize() {
        let small = CGRect(x: 0, y: 0, width: ArrowGeometry.widthRatio * 40, height: 40)
        let big = CGRect(x: 0, y: 0, width: ArrowGeometry.widthRatio * 4000, height: 4000)
        let smallPts = ArrowGeometry.points(in: small)
        let bigPts = ArrowGeometry.points(in: big)
        // Normalized (fraction of width/height) positions match at any
        // size — the shape's proportions don't depend on screen size.
        XCTAssertEqual(smallPts.notch.y / small.height, bigPts.notch.y / big.height, accuracy: 0.0001)
        XCTAssertEqual((smallPts.right.x - small.midX) / small.width,
                        (bigPts.right.x - big.midX) / big.width, accuracy: 0.0001)
    }
}
