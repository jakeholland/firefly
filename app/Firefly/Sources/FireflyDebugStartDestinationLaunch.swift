//
//  FireflyDebugStartDestinationLaunch.swift — `-FireflyStartTab <name>`
//  and `-FireflyFindSegment <name>`, a debug-only launch-argument pair
//  that lands the app on a specific tab/segment at launch, against the
//  REAL (non-demo) composition graph. Built for "app: Map subscribes to
//  festpack updates" (2026-09-13): proving the Field forever-spinner fix
//  against a real, live `AlmanacFestpackProvider` (the bundled Lost
//  Lands 2026 pack) needs a way to reach Find's Field segment headlessly
//  on a fresh simulator install, where `RootView.applyInitialSelection()`
//  would otherwise land on More/Connect (`hasKnownRadio == false`) —
//  `-FireflyDemoScreen` does not help here, since it only ever applies
//  under `-FireflyDemo`'s OWN synthetic graph (`RootView
//  .runInitialDemoScreen()`'s own `guard let demoRunner`).
//
//  Deliberately named/scoped/gated exactly like `FireflyAutoConnectLaunch`
//  (same directory's sibling in spirit, `Connect/FireflyAutoConnectLaunch.swift`):
//  a pure function over an injectable arguments array, so parsing is
//  testable with no real process launch, living in the APP target (not
//  `FireflyKit`) since nothing outside `RootView` needs this, and
//  `#if DEBUG`-gated so a Release/TestFlight/App Store build can never
//  have its landing tab overridden by a stray command-line argument.
//
//  Returns bare `String?`, exactly like `FireflyAutoConnectLaunch
//  .requestedPeripheralName(arguments:)` does — never `Destination`/
//  `FindSegment` directly. Two reasons, not one: it keeps this file
//  decoupled from `RootView`'s own destination enums (the same reason
//  `FireflyAutoConnectLaunch` hands back a bare peripheral name rather
//  than, say, a `ConnectScreen` case), and it keeps this file testable
//  from `FireflyAppTests` with plain `XCTest` — that target compiles
//  the handful of app-target files it needs directly into itself rather
//  than linking against the `Firefly` app binary (see `FindSegment.swift`
//  /`SettingsViewModel.swift`'s own dual Sources-phase membership in
//  `Firefly.xcodeproj`), and `Destination` lives in `RootView.swift`,
//  which is not one of those shared files — pulling it in would mean
//  compiling the whole navigation skeleton (and everything it in turn
//  references) into the test target for one enum's sake.
//
import Foundation

enum FireflyDebugStartDestinationLaunch {
    /// `-FireflyStartTab <name>` — the raw tab name (`RootView
    /// .Destination.rawValue`, matched case-insensitively:
    /// "find"/"inbox"/"lineup"/"more") to override `RootView
    /// .applyInitialSelection()`'s own hasKnownRadio-driven choice with.
    /// `nil` on every ordinary launch, and unconditionally `nil` in a
    /// non-`DEBUG` build.
    static func requestedTabName(arguments: [String] = CommandLine.arguments) -> String? {
        #if DEBUG
        value(for: "-FireflyStartTab", in: arguments)
        #else
        nil
        #endif
    }

    /// `-FireflyFindSegment <name>` — the raw segment name (`FindSegment
    /// .rawValue`, matched case-insensitively: "radar"/"map"/"field"),
    /// only meaningful alongside `-FireflyStartTab find` but applied to
    /// `RootView`'s own `findSegment` state regardless. `nil` on every
    /// ordinary launch, and unconditionally `nil` in a non-`DEBUG` build.
    static func requestedFindSegmentName(arguments: [String] = CommandLine.arguments) -> String? {
        #if DEBUG
        value(for: "-FireflyFindSegment", in: arguments)
        #else
        nil
        #endif
    }

    #if DEBUG
    private static func value(for flag: String, in arguments: [String]) -> String? {
        guard let index = arguments.firstIndex(of: flag), index + 1 < arguments.count else { return nil }
        return arguments[index + 1]
    }
    #endif
}
