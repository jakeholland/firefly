//
//  RootLaunchPlan.swift — what `RootView` lands on at launch, as a pure
//  function of the three things that decide it.
//
//  Split out of `RootView.applyInitialSelection()` (rather than left as
//  an `if` inside the view) for the same reason `MoreScreenNavigation
//  .swift` and `Find/FindSegment.swift` are their own SwiftUI-free
//  files: `FireflyAppTests` can then exercise the rule directly, and
//  the rule is the part that was wrong.
//
//  THE BUG THIS EXISTS TO FIX (audit finding on the build-328
//  notification-tap PR). `applyInitialSelection()` ran in a `.task` and
//  assigned `selection = .find` unconditionally — plus, when `!hasCrew`,
//  raised A02 §6.1's full-screen crew welcome. A notification tap that
//  LAUNCHED the app applies its route earlier, from `.onChange(of:
//  deepLinks.pending, initial: true)` during the first view update, so
//  the later `.task` overwrote it. The thread push itself survived (it
//  lives inside the Inbox tab's own `NavigationStack`), but the TAB did
//  not — and on a device with no crew code stored, the welcome cover hid
//  the result entirely. Tapping "Taylor needs you" on a fresh install
//  landed on the crew welcome.
//
//  THE RULE. A deep link is an explicit instruction about where this
//  launch goes, so it outranks the first-launch default — exactly the
//  precedence `runInitialDemoScreen()` already claims for
//  `-FireflyDemoScreen` (its own "a demo screen name is an explicit
//  instruction about what to show, so it OVERRIDES the first-launch
//  gate"). `.deferToDeepLink` therefore touches NEITHER tab selection
//  nor the cover: the route has already chosen the tab, and the cover is
//  skipped for THIS launch only. It is not dismissed, disabled or
//  remembered — the next plain launch of an app that still has no crew
//  raises it as before, because nothing here is persisted.
//
//  THIS TYPE IS ONLY HALF THE FIX, and saying so here is the point. It
//  answers "a route already landed, so do not assign over it". It
//  cannot answer the opposite ordering — a notification response that
//  arrives AFTER `applyInitialSelection()`'s `.task`, which is what a
//  cold launch measurably does on a fresh simulator — because by then
//  the cover is already up, and no decision taken before the fact can
//  lower it. That half lives in `RootView.applyPendingDeepLink()`,
//  which lowers the cover when it routes.
//  `RootDeepLinkWiringGuardTests` pins both, for exactly this reason.
//
import Foundation

/// Where a launch lands, before `-FireflyDemoScreen`/`-FireflyStartTab`
/// get their say. Three cases and no more: this is the whole of A02
/// §6.1's gate plus the deep-link precedence above it.
enum RootLaunchPlan: Equatable {
    /// A notification tap (or any `firefly://` route) already chose this
    /// launch's destination. Leave tab selection alone AND leave the
    /// crew welcome down — raising it here is what hid a deep-linked
    /// thread behind a cover on a crewless install.
    case deferToDeepLink
    /// The ordinary landing: a radio is known and a crew code is set, so
    /// there is somewhere useful to look.
    case find
    /// A02 §6.1 — "no known radio or no crew" shows the crew welcome
    /// over Find, which is §6.1's own replacement for "land on More with
    /// Connect pre-pushed".
    case findWithCrewWelcome

    /// `true` when this plan has no opinion about the tab, i.e. the
    /// caller must not assign one. Spelled out rather than left to the
    /// call site's `switch` so the debug `-FireflyStartTab`/
    /// `-FireflyFindSegment` overrides can ask the same question: a
    /// debug launch argument must not quietly outrank a real tap either.
    var leavesDestinationToSomebodyElse: Bool { self == .deferToDeepLink }

    /// - Parameters:
    ///   - hasKnownRadio: this process already knows which radio it is
    ///     going after (`RootView.hasKnownRadio`'s own doc comment).
    ///   - hasCrew: a crew code is set (A02 §6.1).
    ///   - hasPendingDeepLink: a tapped-notification route either is
    ///     still waiting in `DeepLinkRouter.pending` or has ALREADY been
    ///     applied by `RootView.applyPendingDeepLink()` during the first
    ///     view update. Both halves matter, and the second is the one
    ///     the bug needed: by the time the `.task` calling this runs,
    ///     `consume()` has normally already emptied `pending`, so
    ///     reading that property alone would report "no deep link" on
    ///     exactly the launch that had one.
    static func plan(hasKnownRadio: Bool, hasCrew: Bool, hasPendingDeepLink: Bool) -> RootLaunchPlan {
        if hasPendingDeepLink { return .deferToDeepLink }
        return (hasKnownRadio && hasCrew) ? .find : .findWithCrewWelcome
    }
}
