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
//  Debug-only by convention, not by `#if DEBUG`: matching `-FireflyDemo`
//  (real devices ignore it whenever nobody passes it), a stray
//  `-FireflyAutoConnect` argument on an ordinary launch is inert unless
//  someone deliberately supplies a peripheral name, and never fires on
//  its own.
//
import Foundation

enum FireflyAutoConnectLaunch {
    /// `-FireflyAutoConnect <name>` — the exact advertised peripheral
    /// name (e.g. `Meshtastic_06b0`) to scan for, select, and CONNECT
    /// to automatically once `ConnectScreen` appears. `nil` on every
    /// ordinary launch.
    static func requestedPeripheralName(arguments: [String] = CommandLine.arguments) -> String? {
        guard let index = arguments.firstIndex(of: "-FireflyAutoConnect"), index + 1 < arguments.count else {
            return nil
        }
        return arguments[index + 1]
    }
}
