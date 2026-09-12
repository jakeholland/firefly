//
//  LineupGridView.swift — the by-stage grid: columns per stage, rows
//  by time, set blocks sized by duration, sticky time gutter and
//  sticky stage header (docs/specs/A01-companion-app.md, Lineup).
//
//  Geometry comes entirely from `LineupGridLayout` (FireflyModel) —
//  this view only turns minutes into points (`pointsPerMinute`) and
//  draws. "Sticky" is achieved by rendering the header row/gutter
//  column OUTSIDE the scrolling body and mirroring the body's own
//  scroll offset onto them via `.offset(...)` — the offset is measured
//  with a `GeometryReader`+`PreferenceKey` inside the scrollable
//  content, the standard SwiftUI technique for a pinned header/gutter
//  when the platform's built-in `pinnedViews` machinery only pins along
//  ONE scroll axis at a time and this grid needs both.
//
import FireflyModel
import SwiftUI

struct LineupGridView: View {
    @Bindable var model: LineupViewModel
    @State private var scrollOffset: CGSize = .zero

    private let gutterWidth: CGFloat = 40
    private let headerHeight: CGFloat = 36
    private let columnWidth: CGFloat = 148
    /// ~64pt/hour, matching the approved mock's geometry.
    private let pointsPerMinute: CGFloat = 64.0 / 60.0

    var body: some View {
        let layout = model.gridLayout
        if layout.columns.isEmpty {
            emptyState
        } else {
            VStack(spacing: 0) {
                // `GeometryReader` here isn't for the scroll-offset trick
                // (that one's below, in `scrollableBody`) — it is what
                // keeps the sticky header row/gutter from reporting
                // their OWN oversized intrinsic width (stage columns
                // are `columnWidth` each; N stages routinely exceed the
                // phone's screen width, which is the whole point of
                // "scrolls both ways") up to THIS view's parent. Without
                // an explicit `width:` here, `.clipped()` alone clips a
                // view to ITS OWN (already oversized) frame — a no-op —
                // and the resulting oversized VStack got centered by
                // the screen's actual width, cropping this screen's
                // ENTIRE header/day-pills/tab-toggle content by the
                // overflow on each side (found screenshotting demo mode
                // for this PR: "Lineup" and the festival name were
                // missing entirely, cropped off the left edge).
                GeometryReader { proxy in
                    // `alignment: .leading` here is load-bearing, not
                    // cosmetic: `stageHeaderRow` is deliberately left at
                    // its own oversized natural width (every stage
                    // column, unclipped past the screen — the sticky
                    // header needs its FULL width available to scroll
                    // into view via the `-scrollOffset.width` mirror
                    // below), so this VStack's own natural width is that
                    // header row's width, NOT `proxy.size.width`. The
                    // gutter+body row below, by contrast, genuinely IS
                    // `proxy.size.width` wide (its `ScrollView` is
                    // correctly constrained to the available space). A
                    // `VStack`'s default (`.center`) alignment centers
                    // EACH child within the stack's own (widest-child)
                    // width — so the narrower gutter+body row would sit
                    // centered under the wide header row, shifted right
                    // by half the difference (one stage column's worth,
                    // in practice) instead of sharing the header's
                    // left edge. `.leading` pins every row's leading
                    // edge together regardless of how much wider the
                    // header row's own (deliberately unclipped) content
                    // is — this is the actual by-stage-column alignment
                    // bug from this PR's first screenshot pass.
                    VStack(alignment: .leading, spacing: 0) {
                        HStack(spacing: 0) {
                            Color.clear.frame(width: gutterWidth, height: headerHeight)
                            stageHeaderRow(layout)
                                .frame(height: headerHeight, alignment: .leading)
                                .offset(x: -scrollOffset.width)
                                .clipped()
                        }
                        // `alignment: .top` here is the vertical twin of
                        // the `.leading` fix above: `timeGutter`'s own
                        // height is the axis length (often shorter than
                        // the space this row actually gets, since
                        // `scrollableBody`'s `ScrollView` fills whatever
                        // height is available). `HStack`'s default
                        // (`.center`) alignment would then center the
                        // SHORTER gutter within the TALLER row, sliding
                        // its hour labels down by half the leftover
                        // height instead of starting them at the same
                        // top edge as the scrollable grid lines/blocks.
                        HStack(alignment: .top, spacing: 0) {
                            timeGutter(layout)
                                .frame(width: gutterWidth)
                                .offset(y: -scrollOffset.height)
                                .clipped()
                            scrollableBody(layout)
                        }
                    }
                    .frame(width: proxy.size.width, height: proxy.size.height, alignment: .topLeading)
                }
                Text("Scrolls both ways · tap a set to pick it")
                    .font(.caption2)
                    .foregroundStyle(Color.ffMuted)
                    .padding(.horizontal)
                    .padding(.vertical, 4)
            }
        }
    }

    private var emptyState: some View {
        VStack(spacing: 8) {
            Image(systemName: "calendar")
                .font(.largeTitle)
                .foregroundStyle(Color.ffMuted)
            Text("No published set times for this night yet.")
                .font(.footnote)
                .foregroundStyle(Color.ffMuted)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    // MARK: - Sticky stage header

    private func stageHeaderRow(_ layout: LineupGridLayout) -> some View {
        HStack(spacing: 0) {
            ForEach(layout.columns) { column in
                Text((column.stage?.name ?? "Unknown stage").uppercased())
                    .font(.system(.caption2, design: .rounded).weight(.bold))
                    .lineLimit(1)
                    .padding(.horizontal, 10)
                    .frame(width: columnWidth, height: headerHeight, alignment: .leading)
                    .background(column.stage.map { Color(fireflyHex: $0.colorRGB) } ?? Color.ffSurface)
                    .foregroundStyle(Color.black)
            }
        }
    }

    // MARK: - Sticky time gutter

    private func timeGutter(_ layout: LineupGridLayout) -> some View {
        ZStack(alignment: .topTrailing) {
            ForEach(layout.hourLines) { line in
                Text(line.label)
                    .font(.system(.caption2, design: .monospaced))
                    .foregroundStyle(Color.ffMuted)
                    .padding(.trailing, 4)
                    // The -6 nudge centres a label on its own gridline,
                    // but this gutter is `.clipped()` to the axis
                    // rectangle, so a label at offset 0 (the axis
                    // starting exactly ON a whole hour — every night in
                    // the real Lost Lands pack does) was drawn at y=-6
                    // and clipped to a half-height sliver. Clamping at
                    // 0 costs the first label 6pt of centring and is
                    // the only label affected; every other line keeps
                    // the nudge.
                    .offset(y: max(0, CGFloat(line.offsetMinutes) * pointsPerMinute - 6))
            }
        }
        .frame(height: CGFloat(layout.axisLengthMinutes) * pointsPerMinute, alignment: .top)
    }

    // MARK: - Scrollable body

    private func scrollableBody(_ layout: LineupGridLayout) -> some View {
        // The outer `GeometryReader` exists ONLY to learn how much
        // room this `ScrollView` actually has (`outerProxy.size`) —
        // needed below because a short night (few stages and/or an
        // early close) routinely produces grid content NARROWER/
        // SHORTER than that. `ScrollView` CENTERS undersized content
        // within its own viewport by default, on both axes — found by
        // comparing this festpack's actual earliest set (21:00, i.e.
        // the very first row on the time axis) against the demo
        // screenshot's first visible block, which was floating well
        // below the grid's top edge, vertically centered in the empty
        // space beneath it instead of sharing the header/gutter's top
        // edge. `.frame(maxWidth: .infinity, maxHeight: .infinity)` on
        // the content does NOT fix this — proposed a nil/unbounded
        // size along a scrolling axis (which is what `ScrollView` asks
        // its content for), `.infinity` doesn't resolve to "the
        // viewport's size" the way it would outside a `ScrollView`, so
        // the centering still happens. Only `minWidth`/`minHeight`,
        // set to the ACTUAL measured viewport size, reliably wins:
        // content narrower/shorter than that gets stretched up to it
        // (topLeading-aligned, so the extra space lands after the
        // content, not straddling it) while content already bigger
        // (the common case — this is a horizontally- AND vertically-
        // scrolling grid precisely because most nights overflow the
        // screen) is completely unaffected, since its own size already
        // exceeds the minimum.
        GeometryReader { outerProxy in
            ScrollView([.horizontal, .vertical], showsIndicators: true) {
                ZStack(alignment: .topLeading) {
                    gridLines(layout)
                    HStack(spacing: 0) {
                        ForEach(layout.columns) { column in
                            columnBody(column, layout: layout)
                        }
                    }
                    if let nowOffset = model.gridNowOffsetMinutes {
                        nowLine(atOffsetMinutes: nowOffset, width: columnWidth * CGFloat(layout.columns.count))
                    }
                    GeometryReader { proxy in
                        Color.clear.preference(key: GridScrollOffsetKey.self,
                                                value: proxy.frame(in: .named("lineupGridScroll")).origin)
                    }
                    .frame(width: 0, height: 0)
                }
                .frame(width: columnWidth * CGFloat(layout.columns.count),
                       height: CGFloat(layout.axisLengthMinutes) * pointsPerMinute, alignment: .topLeading)
                .frame(minWidth: outerProxy.size.width, minHeight: outerProxy.size.height, alignment: .topLeading)
            }
            .coordinateSpace(name: "lineupGridScroll")
            .onPreferenceChange(GridScrollOffsetKey.self) { origin in
                scrollOffset = CGSize(width: -origin.x, height: -origin.y)
            }
        }
    }

    private func gridLines(_ layout: LineupGridLayout) -> some View {
        ForEach(layout.hourLines) { line in
            Rectangle()
                .fill(Color.ffDim.opacity(0.4))
                .frame(height: 1)
                .offset(y: CGFloat(line.offsetMinutes) * pointsPerMinute)
        }
    }

    private func columnBody(_ column: LineupGridLayout.Column, layout: LineupGridLayout) -> some View {
        ZStack(alignment: .topLeading) {
            ForEach(column.blocks) { block in
                SetBlockView(block: block, stage: column.stage, isPicked: model.isPicked(block.set))
                    .frame(width: columnWidth - 8, height: max(28, CGFloat(block.durationMinutes) * pointsPerMinute - 4))
                    .offset(x: 4, y: CGFloat(block.offsetMinutes) * pointsPerMinute + 2)
                    .onTapGesture { model.selectSet(block.set) }
            }
        }
        .frame(width: columnWidth, alignment: .topLeading)
    }

    private func nowLine(atOffsetMinutes offset: Int, width: CGFloat) -> some View {
        HStack(spacing: 0) {
            Circle().fill(Color.ffLiveGreen).frame(width: 8, height: 8).offset(x: -4)
            Rectangle().fill(Color.ffLiveGreen).frame(width: width, height: 2)
        }
        .offset(y: CGFloat(offset) * pointsPerMinute)
    }
}

private struct GridScrollOffsetKey: PreferenceKey {
    static var defaultValue: CGPoint { .zero }
    static func reduce(value: inout CGPoint, nextValue: () -> CGPoint) { value = nextValue() }
}

private struct SetBlockView: View {
    let block: LineupGridLayout.Block
    let stage: FestpackStage?
    let isPicked: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(alignment: .top) {
                Text(block.set.artist.uppercased())
                    .font(.system(.caption, design: .rounded).weight(.bold))
                    .lineLimit(2)
                Spacer(minLength: 0)
                if isPicked {
                    Image(systemName: "star.fill")
                        .font(.caption2)
                }
            }
            Text(timeLabel)
                .font(.system(.caption2, design: .monospaced))
        }
        .padding(8)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(stage.map { Color(fireflyHex: $0.colorRGB) } ?? Color.ffSurface)
        .foregroundStyle(Color.black)
        .clipShape(RoundedRectangle(cornerRadius: 10))
    }

    /// A duration is printed ONLY when the pack published this set's
    /// end time. The block's HEIGHT still comes from the inferred end
    /// (an axis has to place a rectangle somewhere), but a number on
    /// screen reads as a fact, and "runs until whatever is on next"
    /// is not one — see `LineupTimeInference.EndSource`.
    private var timeLabel: String {
        guard let time = LineupViewModel.timeText(block.set.startMinute) else { return "" }
        guard block.endSource == .published, block.durationMinutes >= 90 else { return time }
        let hours = block.durationMinutes / 60
        let minutes = block.durationMinutes % 60
        return minutes == 0 ? "\(time) · \(hours) hr" : "\(time) · \(hours)h\(minutes)m"
    }
}
