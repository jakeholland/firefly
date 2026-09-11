//
//  FireflyAutoConnectLaunch.swift — `-FireflyAutoConnect <name>`, a
//  debug-only launch argument that drives the SAME UI path a person
//  would: scan, select the peripheral whose advertised name matches,
//  CONNECT. Built for the "app: fix live connect path never reaching
//  CONNECTED on macOS" investigation (docs/specs/A01-companion-app.md) —
//  computer-use automation is not always available to drive the signed
//  app bundle's UI by hand, so this gives the same repro a scripted,
//  headless way in: `open ... --args -FireflyAutoConnect Meshtastic_06b0`.
//
//  Deliberately named/scoped like `DemoLaunch` (`FireflyModel/Live/
//  Demo/DemoLaunch.swift`) — pure functions over an injectable
//  arguments array so parsing itself is testable with no real process
//  launch — but lives in the APP target, not `FireflyKit`: nothing
//  outside `ConnectScreen` needs to know this exists, unlike
//  `DemoLaunch`, which `AppDependencies.current()` itself consults.
//
//  Gated behind `#if DEBUG` (app: singleton view models own their
//  subscriptions in the composition root review) — unlike `-FireflyDemo`
//  (real devices ignore it whenever nobody passes it, on purpose, so a
//  bench Heltec can never turn into a fictional festival by accident),
//  this argument drives real CONNECT taps against a real peripheral: a
//  Release/TestFlight/App Store build silently auto-connecting to
//  whatever `-FireflyAutoConnect <name>` happens to be on the command
//  line is a materially different risk than the demo world's synthetic
//  data ever was, so this one is compiled out of a Release build
//  entirely rather than left "inert unless someone supplies a name".
//
import Foundation

enum FireflyAutoConnectLaunch {
    /// `-FireflyAutoConnect <name>` — the exact advertised peripheral
    /// name (e.g. `Meshtastic_06b0`) to scan for, select, and CONNECT
    /// to automatically once `ConnectScreen` appears. `nil` on every
    /// ordinary launch, and unconditionally `nil` in a non-`DEBUG`
    /// build — see this file's header comment.
    static func requestedPeripheralName(arguments: [String] = CommandLine.arguments) -> String? {
        #if DEBUG
        guard let index = arguments.firstIndex(of: "-FireflyAutoConnect"), index + 1 < arguments.count else {
            return nil
        }
        return arguments[index + 1]
        #else
        return nil
        #endif
    }
}
