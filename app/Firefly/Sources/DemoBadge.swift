//
//  DemoBadge.swift — the persistent "this is Firefly Fields, not a
//  real festival" label (`docs/specs/S20-demo-mode.md`'s own rule,
//  ported to the app: "the mode is clearly labeled DEMO so it never
//  masquerades as live field data"). Shown on EVERY screen while demo
//  mode is running, and only then — `RootView` is the one place that
//  decides whether it appears, off the same `demoRunner != nil` test
//  `FireflyApp.init` used to decide whether to build one at all, so
//  there is exactly one source of truth for "is this demo mode".
//
import SwiftUI

struct DemoBadge: View {
    var body: some View {
        Text("DEMO")
            .font(.system(.caption2, design: .monospaced).weight(.bold))
            .tracking(1.5)
            .foregroundStyle(Color.black)
            .padding(.horizontal, 10)
            .padding(.vertical, 4)
            .background(Color.ffStaleAmber, in: Capsule())
            .accessibilityLabel("Demo mode — Firefly Fields sample data, not a live connection")
            .allowsHitTesting(false)
    }
}
