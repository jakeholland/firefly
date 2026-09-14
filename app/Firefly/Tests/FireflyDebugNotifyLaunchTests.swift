//
//  FireflyDebugNotifyLaunchTests.swift — the `-FireflyDebugNotify`
//  reproduction seam's parser, tested the same way
//  `FireflyDebugStartDestinationLaunchTests` tests its sibling: as a
//  pure function over an injected arguments array, with no process
//  launch involved.
//
//  Worth pinning rather than trusting to the UI tests that use it,
//  because every failure mode here is SILENT. A kind name this seam
//  does not own must produce NO notification, so a typo in
//  `NotificationTapUITests`' launch arguments fails as "the banner never
//  appeared" — loudly — instead of quietly scheduling some other
//  category and testing the wrong route. And each kind must carry the
//  deep link its category really uses: a repro whose `userInfo` named
//  the wrong destination would "pass" while proving nothing about the
//  path that crashed in build 328.
//
import FireflyModel
import XCTest

final class FireflyDebugNotifyLaunchTests: XCTestCase {

    // MARK: - Parsing

    func testTheKindIsTheArgumentAfterTheFlag() {
        XCTAssertEqual(FireflyDebugNotifyLaunch.requestedKind(
            arguments: ["Firefly", "-FireflyDemo", "-FireflyDebugNotify", "flare"]), "flare")
    }

    func testAnOrdinaryLaunchRequestsNothing() {
        XCTAssertNil(FireflyDebugNotifyLaunch.requestedKind(arguments: ["Firefly"]))
        XCTAssertNil(FireflyDebugNotifyLaunch.requestedKind(arguments: ["Firefly", "-FireflyDemo"]))
    }

    /// A trailing flag with nothing after it is not a kind — and must
    /// not read off the end of the array.
    func testATrailingFlagWithNoValueRequestsNothing() {
        XCTAssertNil(FireflyDebugNotifyLaunch.requestedKind(
            arguments: ["Firefly", "-FireflyDebugNotify"]))
    }

    // MARK: - Plans

    /// Each kind maps to the plan its own category really produces —
    /// compared against `NotificationPlan.plan(for:)`'s output for the
    /// matching event, not against a hand-written expectation, because
    /// the point of this seam is that the repro notification IS the
    /// shipping one.
    func testEachKindBuildsTheShippingPlanForItsCategory() throws {
        let node = FireflyDebugNotifyLaunch.reproNodeID
        let expected: [String: NotificationPlan] = [
            "thread": .plan(for: .directMessage(from: node, senderName: "Taylor",
                                                packetID: 1, text: "Where are you?")),
            "crew": .plan(for: .crewMessage(from: node, senderName: "Taylor",
                                            packetID: 2, text: "At the main stage.")),
            "flare": .plan(for: .flare(from: node, senderName: "Taylor", packetID: 3)),
            "rally": .plan(for: .rally(from: node, senderName: "Taylor", packetID: 4,
                                       text: "MY SPOT — 210 m NE of you", isBroadcast: true)),
        ]
        for (kind, plan) in expected {
            XCTAssertEqual(FireflyDebugNotifyLaunch.plan(for: kind), plan,
                           "`-FireflyDebugNotify \(kind)` must schedule the plan the live inbound path "
                           + "builds for that event — same category, thread identifier, interruption "
                           + "level and userInfo deep link")
        }
    }

    /// The deep links the two UI-tested kinds route on, stated
    /// explicitly: `thread` must land in the Inbox tab's thread and
    /// `flare` on Find ▸ Radar, which is what
    /// `NotificationTapUITests` asserts on the other side of the tap.
    func testTheTestedKindsCarryTheDeepLinksTheirUITestsAssertOn() throws {
        let node = FireflyDebugNotifyLaunch.reproNodeID
        XCTAssertEqual(try XCTUnwrap(FireflyDebugNotifyLaunch.plan(for: "thread")).deepLink,
                       "firefly://thread/dm/\(node)")
        XCTAssertEqual(try XCTUnwrap(FireflyDebugNotifyLaunch.plan(for: "flare")).deepLink,
                       "firefly://find/\(node)")
    }

    /// Aliases, because the seam documents them: `dm`/`message` are
    /// `thread`, `find` is `flare`, and case does not matter.
    func testTheDocumentedAliasesResolveToTheSamePlans() {
        XCTAssertEqual(FireflyDebugNotifyLaunch.plan(for: "dm"), FireflyDebugNotifyLaunch.plan(for: "thread"))
        XCTAssertEqual(FireflyDebugNotifyLaunch.plan(for: "message"), FireflyDebugNotifyLaunch.plan(for: "thread"))
        XCTAssertEqual(FireflyDebugNotifyLaunch.plan(for: "find"), FireflyDebugNotifyLaunch.plan(for: "flare"))
        XCTAssertEqual(FireflyDebugNotifyLaunch.plan(for: "FLARE"), FireflyDebugNotifyLaunch.plan(for: "flare"))
    }

    /// The loud-failure property: an unknown name gets NO plan rather
    /// than a silently substituted default.
    func testAnUnknownKindGetsNoPlanRatherThanADefault() {
        for kind in ["", "threads", "banner", "thread "] {
            XCTAssertNil(FireflyDebugNotifyLaunch.plan(for: kind),
                         "`\(kind)` must schedule nothing: a typo in a test's launch arguments has to "
                         + "fail as a missing banner, not as a different category quietly tested")
        }
    }
}
