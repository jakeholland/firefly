//
//  RootLaunchPlanTests.swift — the cold-launch landing rule, and in
//  particular the one case it used to get wrong: a launch that came
//  from a tapped notification.
//
//  The defect (audit finding on the build-328 notification-tap PR).
//  `RootView.applyInitialSelection()` assigned `selection = .find`
//  unconditionally, from a `.task` that runs AFTER the
//  `.onChange(of: deepLinks.pending, initial: true)` which applies a
//  tapped route — so the route's tab was overwritten every cold launch.
//  Worse on a fresh install: the same function raised A02 §6.1's
//  full-screen crew welcome whenever `!hasCrew`, so tapping "Taylor
//  needs you" put the crew welcome on screen and the thread behind it.
//
//  These tests are the decision half. The wiring half — that `RootView`
//  actually asks — is `RootDeepLinkWiringGuardTests`, and the
//  behavioural half is `NotificationTapUITests`'
//  `testTappingMessageNotificationAfterTerminationOpensThread`, which
//  launches with NO `-FireflyDemoScreen` override precisely so this
//  rule is the only thing standing between a cold tap and the cover.
//
import XCTest

final class RootLaunchPlanTests: XCTestCase {

    // MARK: - The bug

    /// The regression, and the whole reason this type exists: a pending
    /// deep link outranks the first-launch default, so nothing assigns a
    /// tab over the one the tap chose.
    func testAPendingDeepLinkOutranksTheOrdinaryLanding() {
        XCTAssertEqual(RootLaunchPlan.plan(hasKnownRadio: true, hasCrew: true, hasPendingDeepLink: true),
                       .deferToDeepLink,
                       "a tapped notification's route must survive the initial selection")
    }

    /// The severe case: no crew code stored (a fresh install — which is
    /// every device that has just been handed a puck), where the old
    /// code did not merely pick the wrong tab but covered the screen.
    func testAPendingDeepLinkIsNotHiddenBehindTheCrewWelcome() {
        let plan = RootLaunchPlan.plan(hasKnownRadio: false, hasCrew: false, hasPendingDeepLink: true)
        XCTAssertEqual(plan, .deferToDeepLink,
                       "A02 §6.1's crew welcome must not be raised over a deep-linked thread or Find: "
                       + "on an install with no crew code, tapping a notification landed on the cover "
                       + "and the routed screen was never visible")
        XCTAssertNotEqual(plan, .findWithCrewWelcome)
    }

    /// Every combination of the two first-launch inputs, with a deep
    /// link pending — because the cover is raised by `!hasKnownRadio ||
    /// !hasCrew`, so "no crew" is not the only way in.
    func testADeepLinkWinsRegardlessOfTheFirstLaunchInputs() {
        for hasKnownRadio in [true, false] {
            for hasCrew in [true, false] {
                XCTAssertEqual(RootLaunchPlan.plan(hasKnownRadio: hasKnownRadio,
                                                   hasCrew: hasCrew,
                                                   hasPendingDeepLink: true),
                               .deferToDeepLink,
                               "hasKnownRadio=\(hasKnownRadio) hasCrew=\(hasCrew)")
            }
        }
    }

    /// `.deferToDeepLink` must also suppress the debug `-FireflyStartTab`
    /// /`-FireflyFindSegment` overrides — a debug launch argument does
    /// not outrank a real tap either. Pinned as its own property so the
    /// call site can ask one question rather than re-deriving it.
    func testDeferringIsTheOnlyPlanThatLeavesTheDestinationAlone() {
        XCTAssertTrue(RootLaunchPlan.deferToDeepLink.leavesDestinationToSomebodyElse)
        XCTAssertFalse(RootLaunchPlan.find.leavesDestinationToSomebodyElse)
        XCTAssertFalse(RootLaunchPlan.findWithCrewWelcome.leavesDestinationToSomebodyElse)
    }

    // MARK: - A02 §6.1, unchanged

    /// The ordinary launch is untouched by the fix — stated as a test
    /// rather than assumed, since "a deep link wins" is easy to
    /// over-apply into "nothing else ever lands anywhere".
    func testAKnownRadioAndACrewCodeLandOnFindWithNoCover() {
        XCTAssertEqual(RootLaunchPlan.plan(hasKnownRadio: true, hasCrew: true, hasPendingDeepLink: false),
                       .find)
    }

    /// A02 §6.1's gate, both halves of the `||`, still raises the cover
    /// when no deep link is in play — this is the behaviour the fix must
    /// NOT have removed, and the "cover reappears on the next plain
    /// launch" half of the decision (nothing here is persisted or
    /// remembered; the same inputs give the same answer every time).
    func testTheCrewWelcomeStillShowsOnAnOrdinaryFirstLaunch() {
        for (hasKnownRadio, hasCrew) in [(false, false), (false, true), (true, false)] {
            XCTAssertEqual(RootLaunchPlan.plan(hasKnownRadio: hasKnownRadio,
                                               hasCrew: hasCrew,
                                               hasPendingDeepLink: false),
                           .findWithCrewWelcome,
                           "A02 §6.1: hasKnownRadio=\(hasKnownRadio) hasCrew=\(hasCrew) must still "
                           + "raise the crew welcome when no deep link asked for somewhere else")
        }
    }
}
