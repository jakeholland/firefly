//
//  FireflyDebugCrewLaunch.swift — `-FireflyDebugJoinCrew <code>` and
//  `-FireflyDebugStartCrew`, the bench seam for driving A02's Join/Start
//  non-interactively from the Mac bench app.
//
//  Shaped exactly like its two siblings, `FireflyDebugStartDestination
//  Launch` (`-FireflyStartTab`/`-FireflyFindSegment`) and
//  `FireflyAutoConnectLaunch` (`-FireflyAutoConnect`): a pure function
//  over an injectable arguments array, living in the APP target (nothing
//  in FireflyKit needs it), `#if DEBUG`-gated so a Release/TestFlight/
//  App Store build compiles it out entirely and always reads `nil`.
//
//  `-FireflyDebugJoinCrew` hands back a bare `String?` rather than a
//  parsed `CrewCode`, for the same reason `FireflyAutoConnectLaunch`
//  hands back a bare peripheral name: parsing is the CALLER's business
//  (`CrewScanPayload.classify` already accepts every spelling §1.2
//  allows, including a full `firefly://crew…` link), and keeping this
//  file free of `FireflyModel` keeps it testable from `FireflyAppTests`
//  with plain XCTest.
//
//  **These two drive a real write against a real puck.** They are only
//  ever acted on ONCE a radio is actually connected — `CrewController`'s
//  own radio gate refuses otherwise, exactly as a human tap would be
//  refused — and they go through the ordinary `beginJoin`/`beginStart`
//  + `confirmApply()` path, never a shortcut around the confirmation
//  the spec requires a human to see. What they skip is the TAPPING, not
//  the checks.
//
import Foundation

enum FireflyDebugCrewLaunch {
    /// `-FireflyDebugJoinCrew <code>` — the crew code (or `firefly://`
    /// link) to join automatically, once a puck is connected. `nil` on
    /// every ordinary launch, and unconditionally `nil` outside `DEBUG`.
    static func requestedJoinCode(arguments: [String] = CommandLine.arguments) -> String? {
        #if DEBUG
        value(for: "-FireflyDebugJoinCrew", in: arguments)
        #else
        nil
        #endif
    }

    /// `-FireflyDebugStartCrew [name]` — mint a crew automatically once
    /// a puck is connected. The name is OPTIONAL: bare
    /// `-FireflyDebugStartCrew` starts a crew called "My crew"
    /// (`CrewController.beginStart`'s own default), and a following
    /// argument that is not itself a flag is taken as the name.
    ///
    /// Returns the requested name, or `""` for the bare form — as
    /// distinct from `nil`, which means "not asked for at all". The two
    /// are different instructions and this never collapses them.
    static func requestedStartName(arguments: [String] = CommandLine.arguments) -> String? {
        #if DEBUG
        guard let index = arguments.firstIndex(of: "-FireflyDebugStartCrew") else { return nil }
        let next = index + 1
        guard next < arguments.count, !arguments[next].hasPrefix("-") else { return "" }
        return arguments[next]
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
