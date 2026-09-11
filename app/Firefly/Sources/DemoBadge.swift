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
//  A full-width STRIP, not an overlay (M1 review follow-up, #267): an
//  `overlay(alignment: .top)` badge floated in the same vertical slot
//  every screen's own inline nav-bar title occupies, so "THREAD"
//  rendered right underneath it (06-thread.png). `RootView` now stacks
//  this strip ABOVE the navigation content instead — it claims its own
//  row of real layout space under the status bar/notch, which pushes
//  every screen's nav bar down rather than sitting on top of it. That
//  makes "never covers content or hit targets" true by construction on
//  every screen, not just the ones someone happened to screenshot.
//
import SwiftUI

struct DemoBadge: View {
    var body: some View {
        Text("DEMO")
            .font(.system(.caption2, design: .monospaced).weight(.bold))
            .tracking(1.5)
            .foregroundStyle(Color.black)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 5)
            .background(Color.ffStaleAmber)
            .accessibilityLabel("Demo mode — Firefly Fields sample data, not a live connection")
            .allowsHitTesting(false)
    }
}
