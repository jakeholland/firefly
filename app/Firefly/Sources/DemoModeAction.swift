//
//  DemoModeAction.swift — what "Try the demo"/"Leave the demo" mean, as
//  a pure function of whether this process is already running the demo
//  world. Split out of `FireflyApp`/`AppRuntimeBundle` the same reason
//  `RootLaunchPlan.swift` is its own file (that file's own header
//  comment): so `FireflyAppTests` can exercise the rule directly, with
//  no `AppGraph`/SwiftUI/async machinery in the way.
//
import Foundation
import FireflyModel

/// The one thing an in-app demo-mode tap can mean, resolved against the
/// CURRENT state — never a bare toggle a caller has to invert correctly
/// itself. `AppRuntimeBundle.isDemoMode`'s own doc comment is the single
/// source of truth this reads.
enum DemoModeAction: Equatable {
    /// Not already running the demo world — build one, in place, from
    /// `AppDependencies.demoBundle()`. Never reachable while `isDemoMode`
    /// is already `true` (`DemoModeAction.requested(isDemoMode:)` never
    /// returns this case then) — there is nothing this app needs to do
    /// to "try the demo" a second time.
    case enterDemo
    /// Already running the demo world — tear it down and rebuild the
    /// real stack from `AppDependencies.nonDemo()`. Never reachable
    /// while `isDemoMode` is already `false`.
    case leaveDemo

    /// The ONE place that turns "the button was tapped" into "which
    /// direction". A single, always-correct function rather than two
    /// call sites (Settings' toggle row, the connect-step button) each
    /// computing `!isDemoMode` themselves — the same "one place decides,
    /// every caller just calls it" shape `RootLaunchPlan.plan` follows.
    static func requested(isDemoMode: Bool) -> DemoModeAction {
        isDemoMode ? .leaveDemo : .enterDemo
    }

    /// The dependency graph the action switches to — never anything
    /// that touches Bluetooth or a real radio for `.enterDemo`
    /// (`AppDependencies.demoBundle()`'s own doc comment: a
    /// `DemoMeshtasticClient`, never `MeshtasticClient` over
    /// `BLETransport`), and never the demo world for `.leaveDemo`
    /// (`.nonDemo()`, not `.current()` — this file's own doc comment on
    /// why a leave action must not re-consult `DemoLaunch`).
    var dependencies: AppDependencies {
        switch self {
        case .enterDemo: return AppDependencies.demoBundle().dependencies
        case .leaveDemo: return .nonDemo()
        }
    }

    /// `RootView.runInitialDemoScreen()`'s own vocabulary
    /// (`DemoLaunch.requestedScreen()`'s screenshot-script names) —
    /// reused rather than re-invented: entering demo asks for exactly
    /// what `-FireflyDemoScreen find` asks for (the DEMO badge up, the
    /// crew-welcome cover lowered, Find/Radar on screen with the
    /// scripted crew already paired), which is also what proves this
    /// switch actually seeded a crew rather than landing on an empty
    /// "no radio yet" welcome. Leaving asks for nothing — the rebuilt
    /// real stack's own `hasKnownRadio`/`hasCrew` answer
    /// (`RootLaunchPlan.plan`) decides where it lands, the ordinary rule
    /// every other launch follows.
    var requestedScreen: String? {
        switch self {
        case .enterDemo: return "find"
        case .leaveDemo: return nil
        }
    }
}
