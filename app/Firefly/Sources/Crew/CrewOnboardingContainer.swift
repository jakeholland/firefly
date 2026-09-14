//
//  CrewOnboardingContainer.swift — the modal flow behind the first-launch
//  welcome (`docs/specs/A02-crew-join.md`, §6.1): Welcome → Start a
//  crew / Join a crew → Done. `RootView` presents this whenever there is
//  no known radio OR no crew yet (§6.1's own unchanged condition), and
//  dismisses it once a crew is set (or the user escapes to Connect).
//
import FireflyMesh
import FireflyModel
import SwiftUI

extension View {
    /// `.fullScreenCover` is iOS-only (`RootView`'s own comment on the
    /// FLARE takeover); a plain `.sheet` is this app's cross-platform
    /// substitute on macOS, same convention.
    @ViewBuilder
    func crewOnboardingCover<Content: View>(isPresented: Binding<Bool>, @ViewBuilder content: @escaping () -> Content) -> some View {
        #if os(iOS)
        self.fullScreenCover(isPresented: isPresented, content: content)
        #else
        self.sheet(isPresented: isPresented, content: content)
        #endif
    }
}

struct CrewOnboardingContainer: View {
    let controller: CrewController
    let membership: any CrewMembershipProviding
    let onFinished: () -> Void
    let onConnectPuck: () -> Void
    /// `onOpenURL`'s `firefly://crew…` payload (§1.8), when this
    /// container was opened by tapping a link rather than by the
    /// ordinary first-launch/"no crew" gate — jumps straight to Join
    /// with it pre-supplied, skipping the Welcome step.
    var initialJoinPayload: CrewScanPayload?
    /// `-FireflyDemoScreen crew-start|crew-join` — jumps straight past
    /// Welcome for a milestone screenshot. `nil` on every ordinary
    /// launch.
    var forceStep: Step?

    enum Step: Hashable {
        case start, join
    }
    @State private var path: [Step] = []

    var body: some View {
        NavigationStack(path: $path) {
            CrewWelcomeView(
                onStart: { path.append(.start) },
                onJoin: { path.append(.join) },
                // `onConnectPuck` dismisses this cover itself (RootView's
                // own comment on why chaining `onFinished()` here was a
                // bug: it overwrote the destination that closure had
                // just chosen).
                onConnectPuck: onConnectPuck)
            .navigationDestination(for: Step.self) { step in
                switch step {
                case .start:
                    CrewStartView(controller: controller, membership: membership, onDone: onFinished)
                case .join:
                    CrewJoinView(
                        controller: controller,
                        initialPayload: initialJoinPayload,
                        onMeshtasticLink: { _ in
                            // §3.1 shape 3 — out of THIS container's
                            // scope (it hands off to the existing
                            // channel-import flow); the escape hatch
                            // here is the same "go to Connect" exit
                            // every other path in this screen has, and
                            // for the same reason as above it does NOT
                            // chain `onFinished()`.
                            onConnectPuck()
                        },
                        onDone: onFinished)
                }
            }
        }
        .onAppear {
            if let forceStep, path.isEmpty {
                path = [forceStep]
            } else if initialJoinPayload != nil, path.isEmpty {
                path = [.join]
            }
        }
        // `RootView.runInitialDemoScreen()` sets `forceStep` AFTER
        // `demoRunner.waitUntilStarted()` (crew-start/crew-join must
        // wait for the demo client's own `connect()` before Start/Join
        // can mint against it — see that function's own comment on the
        // "notConnected" race this fixes), which is commonly AFTER this
        // cover is already presented (`applyInitialSelection()`'s
        // separate `.task` shows plain Welcome first, `!hasCrew` alone
        // being enough to open it). `.onAppear` alone would miss that
        // later assignment — this view is not re-created, only its
        // `forceStep` input changes — so this reacts to the change
        // directly.
        .onChange(of: forceStep) { _, newValue in
            if let newValue, path != [newValue] {
                path = [newValue]
            }
        }
    }
}
