//
//  CrewWelcomeView.swift — first-launch welcome (`docs/specs/
//  A02-crew-join.md`, §6.1, artboard `CrewWelcome.dc.html`). Replaces
//  "launch lands on More with Connect pre-pushed" (A01) whenever there
//  is no known radio OR no crew yet — same condition, a different
//  destination. "Connect your puck" is the escape hatch to the radio
//  picker for someone re-installing onto an already-provisioned puck.
//
import SwiftUI

struct CrewWelcomeView: View {
    let onStart: () -> Void
    let onJoin: () -> Void
    let onConnectPuck: () -> Void

    var body: some View {
        VStack(spacing: 28) {
            Spacer()
            VStack(spacing: 12) {
                Text("Find your people")
                    .font(.system(.largeTitle, design: .rounded).weight(.heavy))
                    .foregroundStyle(Color.ffInk)
                    .multilineTextAlignment(.center)
                    // The screen marker lives HERE, not on the outer
                    // container, and this is the one screen where that
                    // distinction matters (PR #308 review, BLOCKING —
                    // the UI smoke test failed on it).
                    //
                    // Every pre-A02 screen puts its `Screen.X` marker on
                    // its outermost view (`ConnectScreen`'s own comment)
                    // and none of them has a child identifier, so nobody
                    // had hit this: a SwiftUI accessibility identifier
                    // applied to a container OVERWRITES the identifiers
                    // its descendants set for themselves. With
                    // `Screen.CrewWelcome` on the VStack, all three
                    // buttons below reported `Screen.CrewWelcome` too
                    // (confirmed by dumping the real tree) —
                    // `CrewWelcome.ConnectPuck` existed nowhere, and the
                    // `assertScreen` that "passed" was matching a
                    // StaticText, not the screen.
                    //
                    // On the title, the marker identifies the screen
                    // exactly as before and leaves the buttons their own
                    // names.
                    .accessibilityIdentifier("Screen.CrewWelcome")
                Text("No signal needed. Your puck talks to your crew's pucks directly.")
                    .font(.body)
                    .foregroundStyle(Color.ffMuted)
                    .multilineTextAlignment(.center)
            }
            .padding(.horizontal, 24)

            VStack(spacing: 12) {
                Button(action: onStart) {
                    Text("START A CREW")
                        .font(.system(.headline, design: .rounded).weight(.bold))
                        .frame(maxWidth: .infinity, minHeight: 48)
                }
                .buttonStyle(.borderedProminent)
                .tint(Color.ffAmber)
                .foregroundStyle(Color.ffBackground)
                .accessibilityIdentifier("CrewWelcome.Start")

                Button(action: onJoin) {
                    Text("JOIN A CREW")
                        .font(.system(.headline, design: .rounded).weight(.bold))
                        .frame(maxWidth: .infinity, minHeight: 48)
                }
                .buttonStyle(.bordered)
                .tint(Color.ffInk)
                .accessibilityIdentifier("CrewWelcome.Join")
            }
            .padding(.horizontal, 24)

            Text("One person starts the crew and shows a code. Everyone else scans it. " +
                 "That's the whole setup.")
                .font(.footnote)
                .foregroundStyle(Color.ffMuted)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 32)

            Spacer()

            Button("Already set up? Connect your puck", action: onConnectPuck)
                .font(.footnote)
                .foregroundStyle(Color.ffAmber)
                .padding(.bottom, 24)
                .accessibilityIdentifier("CrewWelcome.ConnectPuck")
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color.ffBackground)
    }
}
