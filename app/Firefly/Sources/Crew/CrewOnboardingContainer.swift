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
    /// A02 §6.1 connect step (owner report, build 328) — the SAME
    /// `ConnectViewModel` the Connect screen drives, never a second one,
    /// so a connect made here is the connect the whole app sees.
    let connect: ConnectViewModel
    /// The real BLE scan when there is one (`AppDependencies.live()`),
    /// `StubPeripheralDiscovery` otherwise — composed exactly as
    /// `ConnectScreen` composes it from the same `scanner` seam.
    let scanner: (any NodeScanning)?
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
        /// Where the connect step should go once a puck is connected
        /// (or once the user taps "Do this later").
        enum Next: Hashable { case start, join }

        /// A02 §6.1's new middle step. Pushed instead of `.start`/
        /// `.join` when no puck is connected yet, and SKIPPED entirely
        /// when one already is — "the step is skipped automatically when
        /// already connected".
        case connect(next: Next)
        case start, join

        static func target(_ next: Next) -> Step { next == .start ? .start : .join }
    }
    @State private var path: [Step] = []
    /// Built here, once, rather than injected: the scan list is this
    /// flow's own state, exactly as it is `ConnectScreen`'s own state
    /// (that screen's `discovery` property has the same comment).
    @State private var discovery: any PeripheralDiscovering

    init(controller: CrewController,
         membership: any CrewMembershipProviding,
         connect: ConnectViewModel,
         scanner: (any NodeScanning)?,
         onFinished: @escaping () -> Void,
         onConnectPuck: @escaping () -> Void,
         initialJoinPayload: CrewScanPayload? = nil,
         forceStep: Step? = nil) {
        self.controller = controller
        self.membership = membership
        self.connect = connect
        self.scanner = scanner
        self.onFinished = onFinished
        self.onConnectPuck = onConnectPuck
        self.initialJoinPayload = initialJoinPayload
        self.forceStep = forceStep
        _discovery = State(initialValue: scanner.map { MeshPeripheralDiscovery(scanner: $0) }
                            ?? StubPeripheralDiscovery())
    }

    var body: some View {
        NavigationStack(path: $path) {
            CrewWelcomeView(
                onStart: { go(.start) },
                onJoin: { go(.join) },
                // `onConnectPuck` dismisses this cover itself (RootView's
                // own comment on why chaining `onFinished()` here was a
                // bug: it overwrote the destination that closure had
                // just chosen).
                onConnectPuck: onConnectPuck)
            .navigationDestination(for: Step.self) { step in
                switch step {
                case .connect(let next):
                    CrewConnectPuckView(
                        connect: connect,
                        discovery: discovery,
                        isRadioUsable: { controller.hasConnectedRadio },
                        onConnected: { leaveConnectStep(next: next) },
                        // "Do this later" advances to Start/Join rather
                        // than dropping the user back where they came
                        // from: the destination screen's own banner then
                        // explains, in place, exactly why its primary
                        // action is disabled. A dead end here would be a
                        // second way to make a tap do nothing.
                        onSkip: { leaveConnectStep(next: next) })
                case .start:
                    CrewStartView(controller: controller, membership: membership,
                                  onConnectPuck: { path.append(.connect(next: .start)) },
                                  onDone: onFinished)
                case .join:
                    CrewJoinView(
                        controller: controller,
                        initialPayload: initialJoinPayload,
                        onConnectPuck: { path.append(.connect(next: .join)) },
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

    /// §6.1: Welcome's two buttons go straight to Start/Join when a puck
    /// is already connected, and through the connect step when one is
    /// not. The check is `CrewController.hasConnectedRadio` — the same
    /// property the write path itself gates on, so this can never send
    /// someone past a step the write would then refuse.
    private func go(_ next: Step.Next) {
        path.append(controller.hasConnectedRadio ? Step.target(next) : .connect(next: next))
    }

    /// Leaves the connect step for its target, whether it was reached
    /// from Welcome (replace the step) or from a Start/Join banner (pop
    /// back to the screen underneath). One function, because the only
    /// difference between the two is whether the target is already on
    /// the path.
    private func leaveConnectStep(next: Step.Next) {
        controller.clearFailure()
        guard case .connect = path.last else { return }
        path.removeLast()
        let target = Step.target(next)
        if path.last != target { path.append(target) }
    }
}
