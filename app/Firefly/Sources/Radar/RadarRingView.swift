//
//  RadarRingView.swift — the crew ring, the arrow, and (SIGNAL mode) the
//  inner signal-only ring.
//
//  Every angle drawn here is already computed by the C core
//  (`ff_radar_compute`, wrapped by `RadarComputing`) and handed over as
//  plain degrees on `RadarSnapshot`; this file only turns a known
//  angle + a known radius into screen coordinates (ordinary view-layer
//  trigonometry), never a bearing or a distance between two positions.
//
import FireflyModel
import SwiftUI

struct RadarRingView: View {
    let snapshot: RadarSnapshot
    let colorblind: Bool

    /// -80...80 (S29) — the fixed upper arc the signal-only ring is laid
    /// out across, evenly spaced by index, explicitly carrying no
    /// bearing meaning.
    private let signalArcStart = -80.0
    private let signalArcEnd = 80.0

    var body: some View {
        GeometryReader { geo in
            let side = min(geo.size.width, geo.size.height)
            let center = CGPoint(x: geo.size.width / 2, y: geo.size.height / 2)
            let ringRadius = side * 0.42
            let signalRadius = side * 0.30

            ZStack {
                Circle()
                    .stroke(Color.radarDim, lineWidth: 1)
                    .frame(width: ringRadius * 2, height: ringRadius * 2)
                    .position(center)

                ForEach(snapshot.dots) { dot in
                    crewDot(dot)
                        .position(point(onRadius: ringRadius, degrees: dot.ringDegrees, from: center))
                }

                if snapshot.mode == .signal {
                    ForEach(Array(snapshot.signalDots.enumerated()), id: \.element.id) { index, dot in
                        signalDot(dot)
                            .position(point(onRadius: signalRadius,
                                             degrees: signalArcAngle(index: index,
                                                                      count: snapshot.signalDots.count),
                                             from: center))
                    }
                }

                arrow(center: center, radius: ringRadius)
            }
        }
    }

    /// Even spacing over the fixed upper arc — S29's own correction note
    /// (this file's arc constants match `RADAR_LAYOUT_SIGNAL_RING_ARC_*`
    /// after that spec's own draft-arc correction): θ ∈ [-80°, 80°], 0° =
    /// straight up. A single dot centers at 0°.
    private func signalArcAngle(index: Int, count: Int) -> Double {
        guard count > 1 else { return 0 }
        let span = signalArcEnd - signalArcStart
        return signalArcStart + span * Double(index) / Double(count - 1)
    }

    private func point(onRadius radius: CGFloat, degrees: Double, from center: CGPoint) -> CGPoint {
        let radians = (degrees - 90) * .pi / 180 // 0deg = straight up
        return CGPoint(x: center.x + radius * cos(radians), y: center.y + radius * sin(radians))
    }

    @ViewBuilder
    private func crewDot(_ dot: RadarDot) -> some View {
        let color = Color.radarCrew(index: dot.colorIndex, colorblind: colorblind)
        ZStack {
            if dot.place {
                // A filled square — a different silhouette for a place,
                // never a differently-styled friend dot (S06 issue #33).
                Rectangle()
                    .fill(color)
                    .frame(width: 14, height: 14)
            } else if dot.stale {
                Circle()
                    .strokeBorder(color, style: StrokeStyle(lineWidth: 2, dash: [3, 3]))
                    .frame(width: 14, height: 14)
            } else {
                Circle().fill(color).frame(width: 14, height: 14)
            }
            if dot.imprecise {
                // A degraded-precision fix could be off by kilometers —
                // a fuzzy ring around the dot, never an ordinary crisp
                // one (S17 issue #74).
                Circle().stroke(color.opacity(0.35), lineWidth: 6).frame(width: 26, height: 26)
            }
            Text(String(dot.initial))
                .font(.system(size: 8, weight: .bold, design: .rounded))
                .foregroundStyle(Color.ffBackground)
        }
    }

    @ViewBuilder
    private func signalDot(_ dot: RadarSignalDot) -> some View {
        let color = tierColor(dot.tier)
        Group {
            if dot.viaRelay || dot.tier == .none {
                Circle().stroke(Color.radarDim, lineWidth: 1.5).frame(width: 8, height: 8)
            } else {
                Circle().fill(color).frame(width: 8, height: 8)
            }
        }
    }

    private func tierColor(_ tier: SignalTierPresentation) -> Color {
        switch tier {
        case .strong, .good: return .ffLiveGreen
        case .weak: return .radarStaleAmber
        case .faint, .none: return .ffMuted
        }
    }

    @ViewBuilder
    private func arrow(center: CGPoint, radius: CGFloat) -> some View {
        if snapshot.arrowValid {
            // "STALE says this is a few minutes old, LOST says do not
            // trust this" (S06, PR #16 UX review ruling) — the ghost
            // (LOST/SIGNAL-with-a-real-old-fix) treatment is
            // outline-only, never a dimmer copy of STALE's dashed-fill.
            let ghost = snapshot.mode == .lost || snapshot.mode == .signal
            let dashed = snapshot.mode == .stale
            Group {
                if ghost {
                    ArrowShape().stroke(Color.ffMuted.opacity(0.6), style: StrokeStyle(lineWidth: 2, dash: [4, 4]))
                } else if dashed {
                    ArrowShape().fill(Color.radarStaleAmber.opacity(0.28))
                        .overlay(ArrowShape().stroke(Color.radarStaleAmber, style: StrokeStyle(lineWidth: 2, dash: [6, 4])))
                } else {
                    ArrowShape().fill(Color.ffAmber)
                }
            }
            .frame(width: 28, height: radius * 0.9)
            .rotationEffect(.degrees(snapshot.arrowDegrees))
            .position(center)
        }
    }
}

/// A simple, honest arrow glyph — filled for a real bearing, or drawn by
/// the caller with a dashed/reduced-opacity stroke and no fill for the
/// STALE/ghost treatments above.
private struct ArrowShape: Shape {
    func path(in rect: CGRect) -> Path {
        var p = Path()
        p.move(to: CGPoint(x: rect.midX, y: rect.minY))
        p.addLine(to: CGPoint(x: rect.maxX, y: rect.maxY))
        p.addLine(to: CGPoint(x: rect.midX, y: rect.maxY * 0.75))
        p.addLine(to: CGPoint(x: rect.minX, y: rect.maxY))
        p.closeSubpath()
        return p
    }
}

/// CLOSE mode's three pulsing rings, replacing the arrow entirely.
struct ClosePulseRings: View {
    @State private var animate = false

    var body: some View {
        ZStack {
            ForEach(0..<3, id: \.self) { i in
                Circle()
                    .stroke(Color.ffLiveGreen.opacity(0.6), lineWidth: 2)
                    .scaleEffect(animate ? 1.0 : 0.4)
                    .opacity(animate ? 0.0 : 0.8)
                    .animation(
                        .easeOut(duration: 1.2).repeatForever(autoreverses: false).delay(Double(i) * 0.4),
                        value: animate)
            }
        }
        .onAppear { animate = true }
    }
}
