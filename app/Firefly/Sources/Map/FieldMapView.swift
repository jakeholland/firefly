//
//  FieldMapView.swift — Map tab slice: the puck's own SCHEMATIC festpack
//  map, ported to the phone. Renders `FieldMapProjection`
//  (FireflyModel/Map/FieldMapProjection.swift) — which projects through
//  the SAME `ff_map`/`ff_geo` object code `firmware/app/screens/
//  scr_map.c` draws from — never a from-scratch reimplementation.
//
//  Honesty: this is explicitly labeled SCHEMATIC (bottom-left chip,
//  always visible) — a stylized, fixed-fit projection, not a literal
//  aerial map. Every crew pin still carries the SAME freshness-based
//  treatment `CrewMapPinTreatment` defines (solid/dashed/square/area) —
//  this view draws exactly what `MapViewModel` computed, nothing more.
//
import FireflyModel
import SwiftUI

struct FieldMapView: View {
    /// The whole view model, not a pre-computed `FieldMapProjection?` —
    /// PR #283 review, BLOCKING 1: this view's own `GeometryReader` is
    /// the only place that knows the REAL on-screen radius (`radiusPx`
    /// below), so the projection is computed HERE, from that measured
    /// value, rather than by a caller (`MapTabView`) guessing a fixed
    /// pixel count before layout is known. Passing a stale, hardcoded
    /// radius into `fieldMapProjection(radiusPx:marginPx:)` broke S09
    /// AC1's "every point stays inside the fitted circle" guarantee on
    /// any device/window whose Field-map square isn't exactly the
    /// literal that was hardcoded.
    let model: MapViewModel
    let onSelect: (UInt32) -> Void
    let selectedCrewID: UInt32?
    let colorblind: Bool

    var body: some View {
        GeometryReader { geo in
            let side = min(geo.size.width, geo.size.height)
            let radiusPx = Float(side / 2 - 12)
            let projection = model.fieldMapProjection(radiusPx: radiusPx, marginPx: 16)
            ZStack {
                Circle()
                    .fill(Color.ffSurface)
                    .overlay(Circle().stroke(Color.ffMuted.opacity(0.4), lineWidth: 1))

                if let projection {
                    Canvas { context, size in
                        let center = CGPoint(x: size.width / 2, y: size.height / 2)
                        for feature in projection.features {
                            draw(feature: feature, center: center, in: &context)
                        }
                    }
                    .frame(width: side, height: side)

                    ForEach(projection.crew, id: \.pin.id) { dot in
                        crewMarker(dot, selected: dot.pin.id == selectedCrewID)
                            .position(x: side / 2 + CGFloat(dot.x), y: side / 2 + CGFloat(dot.y))
                            .onTapGesture { onSelect(dot.pin.id) }
                            .accessibilityIdentifier("Map.Field.Crew.\(dot.pin.id)")
                    }

                    if let you = projection.you {
                        youMarker(you)
                            .position(x: side / 2 + CGFloat(you.x), y: side / 2 + CGFloat(you.y))
                            .accessibilityIdentifier("Map.Field.You")
                    }
                } else {
                    ProgressView().tint(.ffAmber)
                }

                VStack {
                    Spacer()
                    HStack {
                        schematicChip
                        Spacer()
                    }
                }
                .padding(12)
            }
            .frame(width: side, height: side)
            .position(x: geo.size.width / 2, y: geo.size.height / 2)
        }
        .background(Color.ffBackground)
        .accessibilityIdentifier("Screen.Map.Field")
    }

    private var schematicChip: some View {
        Text("SCHEMATIC")
            .font(.system(.caption2, design: .monospaced)).bold()
            .foregroundStyle(Color.ffBackground)
            .padding(.horizontal, 8).padding(.vertical, 4)
            .background(Color.ffMuted, in: Capsule())
    }

    private func draw(feature: FieldMapFeature, center: CGPoint, in context: inout GraphicsContext) {
        let color = Color.mapFeature(hex: feature.colorHex)
        let points = feature.points.map { CGPoint(x: center.x + CGFloat($0.x), y: center.y + CGFloat($0.y)) }

        switch feature.renderKind {
        case .omit, .labelOnly:
            break // no shape — see ff_map_feature_render_kind's own doc comment
        case .stageStub:
            guard let p = points.first else { return }
            let r: CGFloat = 10
            let rect = CGRect(x: p.x - r, y: p.y - r, width: r * 2, height: r * 2)
            context.fill(Path(ellipseIn: rect), with: .color(color.opacity(0.25)))
            context.stroke(Path(ellipseIn: rect), with: .color(color), lineWidth: 1.3)
        case .line:
            guard points.count == 2 else { return }
            var path = Path()
            path.move(to: points[0])
            path.addLine(to: points[1])
            context.stroke(path, with: .color(color), lineWidth: 1.3)
        case .polygon:
            guard points.count >= 3 else { return }
            var path = Path()
            path.move(to: points[0])
            for p in points.dropFirst() { path.addLine(to: p) }
            path.closeSubpath()
            context.fill(path, with: .color(color.opacity(0.13)))
            context.stroke(path, with: .color(color), lineWidth: 1.3)
        }

        // Labels for anything with at least one point — honest per S09's
        // own "the shape always draws; only text can be dropped" rule.
        // v1 here draws every label unconditionally (no collision
        // declutter pass yet — a documented, smaller-scope follow-up,
        // same spirit as this codebase's other flagged interpretation
        // calls).
        if let anchor = points.first, feature.renderKind != .omit {
            let text = Text(feature.label).font(.system(size: 9, design: .monospaced)).foregroundStyle(Color.ffInk)
            context.draw(context.resolve(text), at: CGPoint(x: anchor.x, y: anchor.y - 14))
        }
    }

    @ViewBuilder
    private func crewMarker(_ dot: FieldMapCrewDot, selected: Bool) -> some View {
        let color = Color.mapCrew(colorIndex: dot.pin.colorIndex, colorblind: colorblind)
        ZStack {
            switch dot.pin.treatment {
            case .live:
                Circle().fill(color).frame(width: 16, height: 16)
            case .staleRing, .lostRing:
                Circle().strokeBorder(style: StrokeStyle(lineWidth: 2, dash: [3, 2])).foregroundStyle(color)
                    .frame(width: 16, height: 16)
            case .asserted:
                RoundedRectangle(cornerRadius: 2).fill(color).frame(width: 14, height: 14)
            case .imprecise:
                Circle().strokeBorder(color.opacity(0.7), lineWidth: 2).frame(width: 34, height: 34)
            }
            if let initial = dot.pin.initial, dot.pin.treatment != .imprecise {
                Text(String(initial)).font(.system(size: 9, weight: .bold))
                    .foregroundStyle(dot.pin.treatment == .live ? Color.ffBackground : color)
            }
            if selected {
                Circle().stroke(Color.ffAmber, lineWidth: 2).frame(width: 26, height: 26)
            }
        }
        .frame(minWidth: 44, minHeight: 44) // FF_THEME_MIN_HIT_PX — 44pt tap target
        .contentShape(Circle())
    }

    private func youMarker(_ you: FieldMapYou) -> some View {
        ZStack {
            Circle().fill(Color.mapYou).frame(width: 14, height: 14)
            Circle().stroke(Color.white, lineWidth: 2).frame(width: 14, height: 14)
            if let heading = you.headingDegrees {
                Image(systemName: "location.north.fill")
                    .font(.system(size: 10))
                    .foregroundStyle(Color.mapYou)
                    .rotationEffect(.degrees(heading))
                    .offset(y: -14)
            }
        }
    }
}
