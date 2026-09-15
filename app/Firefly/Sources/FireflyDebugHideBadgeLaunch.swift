//
//  FireflyDebugHideBadgeLaunch.swift — `-FireflyDebugHideBadge`, a
//  debug-only launch flag that suppresses the DEMO strip
//  (`DemoBadge.swift`) for exactly one launch.
//
//  Built for `app/tools/store_media.sh` (App Store screenshots): the
//  demo stack is what puts real-looking crew/inbox/lineup content on
//  screen with no radio attached, but the DEMO badge it also raises
//  (`docs/specs/S20-demo-mode.md`'s own "never masquerade as live
//  data" rule) is honest and correct for every OTHER use of demo mode
//  — bench review, the milestone screenshots in `docs/screens/demo/`
//  — and stays the DEFAULT here too. This flag exists only so a store
//  screenshot run can opt out per shot, never so the badge can be
//  quietly dropped by accident.
//
//  Same shape as every sibling in this file's family
//  (`FireflyDebugStartDestinationLaunch`, `FireflyDebugCrewLaunch`): a
//  pure function over an injectable arguments array, living in the APP
//  target (`RootView` is the only consumer), `#if DEBUG`-gated so a
//  Release/TestFlight/App Store build can never hide the badge even if
//  the flag leaked onto a real launch — the badge is load-bearing
//  honesty, not cosmetic, so the seam that can turn it off does not
//  exist outside DEBUG at all.
//
import Foundation

enum FireflyDebugHideBadgeLaunch {
    /// `-FireflyDebugHideBadge` — a bare flag, no value. `true` only
    /// when present AND running a DEBUG build; unconditionally `false`
    /// otherwise (no flag, or a non-DEBUG build).
    static func isRequested(arguments: [String] = CommandLine.arguments) -> Bool {
        #if DEBUG
        arguments.contains("-FireflyDebugHideBadge")
        #else
        false
        #endif
    }
}
