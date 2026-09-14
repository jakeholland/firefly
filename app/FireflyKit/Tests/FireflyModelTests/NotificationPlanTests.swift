//
//  NotificationPlanTests.swift — A03_AC10/AC11 and §3.11.2's wording.
//
//  Every row of §3.11.1's table, pinned. No `UNUserNotificationCenter`
//  is touched, which is the entire reason `NotificationPlan` exists as a
//  separate type (A03_AC10: "this is the seam that makes notification
//  behaviour testable at all").
//
import FireflyModel
import XCTest

final class NotificationPlanTests: XCTestCase {

    // MARK: - A03_AC10 — §3.11.1's table, row by row

    func testA03_AC10_FlareIsTimeSensitiveWithItsOwnThreadAndFindLink() {
        let plan = NotificationPlan.plan(for: .flare(from: 4_098, senderName: "Taylor", packetID: 77))
        XCTAssertEqual(plan.interruptionLevel, .timeSensitive)
        XCTAssertTrue(plan.playsSound)
        XCTAssertEqual(plan.threadIdentifier, "flare")
        XCTAssertEqual(plan.categoryIdentifier, "FLARE")
        XCTAssertEqual(plan.identifier, "flare-4098-77")
        XCTAssertEqual(plan.title, "Taylor needs you")
        XCTAssertEqual(plan.body, "They sent a flare. Tap to find them.")
        XCTAssertEqual(plan.deepLink, "firefly://find/4098")
    }

    /// §3.11.2's own fallback row. "Someone" is honest about a name we
    /// do not have — it is not a name this app invented for a person.
    func testA03_AC10_FlareWithNoNameSaysSomeoneRatherThanInventingOne() {
        for name in [String?.none, "", "   "] {
            let plan = NotificationPlan.plan(for: .flare(from: 4_098, senderName: name, packetID: 77))
            XCTAssertEqual(plan.title, "Someone needs you")
            XCTAssertEqual(plan.body, "A flare came in from your crew. Tap to find them.")
            XCTAssertEqual(plan.interruptionLevel, .timeSensitive, "an unnamed crew member's flare is still a flare")
        }
    }

    func testA03_AC10_RallyIsActiveAndCarriesTheComposedLine() {
        let plan = NotificationPlan.plan(for: .rally(from: 4_100, senderName: "Taylor", packetID: 12,
                                                      text: "MY SPOT \u{2014} 210 m NE of you",
                                                      isBroadcast: true))
        XCTAssertEqual(plan.interruptionLevel, .active)
        XCTAssertEqual(plan.threadIdentifier, "rally")
        XCTAssertEqual(plan.categoryIdentifier, "RALLY")
        XCTAssertEqual(plan.identifier, "rally-4100-12")
        XCTAssertEqual(plan.title, "Taylor set a meeting spot")
        XCTAssertEqual(plan.body, "MY SPOT \u{2014} 210 m NE of you")
    }

    /// REVIEW FIX (PR #310) — a RALLY's tap destination is the thread
    /// its FEED ROW went to, decided by the same `isBroadcastDestination`
    /// call `pushInboundFeedItem` makes. The link used to be
    /// `thread/dm/<from>` unconditionally, so the ordinary case — a crew
    /// broadcast — opened an empty 1:1 instead of the rally.
    func testRallyDeepLinkOpensTheThreadItsRowIsIn() {
        let broadcast = NotificationPlan.plan(for: .rally(from: 4_100, senderName: "Taylor", packetID: 12,
                                                           text: "MY SPOT", isBroadcast: true))
        XCTAssertEqual(broadcast.deepLink, "firefly://thread/crew")
        XCTAssertEqual(FireflyDeepLink.route(for: URL(string: broadcast.deepLink)!), .thread(.crew))

        let direct = NotificationPlan.plan(for: .rally(from: 4_100, senderName: "Taylor", packetID: 12,
                                                        text: "MY SPOT", isBroadcast: false))
        XCTAssertEqual(direct.deepLink, "firefly://thread/dm/4100")
        XCTAssertEqual(FireflyDeepLink.route(for: URL(string: direct.deepLink)!), .thread(.member(4_100)))
    }

    func testA03_AC10_DirectMessageIsActiveAndThreadsPerSender() {
        let plan = NotificationPlan.plan(for: .directMessage(from: 8_193, senderName: "Taylor",
                                                              packetID: 500, text: "on my way"))
        XCTAssertEqual(plan.interruptionLevel, .active)
        XCTAssertEqual(plan.threadIdentifier, "dm-8193")
        XCTAssertEqual(plan.categoryIdentifier, "MESSAGE")
        XCTAssertEqual(plan.identifier, "msg-8193-500")
        XCTAssertEqual(plan.title, "Taylor", "a DM's title is the person, nothing else")
        XCTAssertEqual(plan.body, "on my way")
        XCTAssertEqual(plan.deepLink, "firefly://thread/dm/8193")
    }

    func testA03_AC10_CrewMessageThreadsAsOneStackAndSaysWhichRoom() {
        let plan = NotificationPlan.plan(for: .crewMessage(from: 8_194, senderName: "Taylor",
                                                            packetID: 501, text: "we are at the rail"))
        XCTAssertEqual(plan.threadIdentifier, "crew")
        XCTAssertEqual(plan.title, "Taylor \u{00B7} crew")
        XCTAssertEqual(plan.deepLink, "firefly://thread/crew")
        // §9 Q3 asks whether this should be `.passive` ALWAYS. Until the
        // owner answers, the spec's own stated default ships — and this
        // line is where that decision is recorded, so flipping it is a
        // visible test change rather than a silent one.
        XCTAssertEqual(plan.interruptionLevel, .active, "A03 §3.11.1's stated default outside quiet hours")
    }

    /// Every DM and crew message groups per conversation, so a chatty
    /// channel is one stack rather than forty banners (audit 2.3.13).
    func testA03_AC10_ThreadIdentifiersSeparateEveryConversation() {
        let dmA = NotificationPlan.plan(for: .directMessage(from: 1, senderName: nil, packetID: 1, text: "a"))
        let dmB = NotificationPlan.plan(for: .directMessage(from: 2, senderName: nil, packetID: 1, text: "b"))
        let crew = NotificationPlan.plan(for: .crewMessage(from: 1, senderName: nil, packetID: 2, text: "c"))
        XCTAssertEqual(Set([dmA, dmB, crew].map(\.threadIdentifier)).count, 3)
    }

    /// The FLARE's interruption level is the ONLY `.timeSensitive` one.
    /// Meshtastic-Apple sets `.timeSensitive` on everything, including a
    /// new-node discovery — "the thing that trains users to revoke the
    /// permission" (§8). This test is that divergence, pinned.
    func testA03_AC10_OnlyAFlareBreaksThroughFocus() {
        let events: [NotificationEvent] = [
            .rally(from: 1, senderName: "T", packetID: 1, text: "x", isBroadcast: true),
            .directMessage(from: 1, senderName: "T", packetID: 2, text: "x"),
            .crewMessage(from: 1, senderName: "T", packetID: 3, text: "x"),
        ]
        for event in events {
            XCTAssertNotEqual(NotificationPlan.plan(for: event).interruptionLevel, .timeSensitive,
                               "only a FLARE may break through a Focus")
        }
        XCTAssertEqual(NotificationPlan.plan(for: .flare(from: 1, senderName: "T", packetID: 4)).interruptionLevel,
                        .timeSensitive)
    }

    // MARK: - A03_AC11 — derived identifiers, never random

    /// **A03_AC11.** The same packet planned twice produces the same
    /// identifier — which is what makes iOS REPLACE rather than stack
    /// (§1.10). `UUID().uuidString` (what this replaced) could not.
    func testA03_AC11_TheSamePacketAlwaysDerivesTheSameIdentifier() {
        let event = NotificationEvent.directMessage(from: 8_193, senderName: "Taylor",
                                                     packetID: 500, text: "on my way")
        XCTAssertEqual(NotificationPlan.plan(for: event).identifier,
                        NotificationPlan.plan(for: event).identifier)
        // ...and the whole plan is equal, not only its identifier: a
        // second delivery must not differ in level, sound or thread
        // either.
        XCTAssertEqual(NotificationPlan.plan(for: event), NotificationPlan.plan(for: event))
    }

    /// Different packets must NOT collide — the other half of the same
    /// property, and the one a naive "one identifier per sender" scheme
    /// would break by silently replacing an unread message.
    func testA03_AC11_DifferentPacketsNeverShareAnIdentifier() {
        var identifiers: Set<String> = []
        for from in UInt32(1)...UInt32(5) {
            for packetID in UInt32(1)...UInt32(5) {
                identifiers.insert(NotificationPlan.plan(
                    for: .directMessage(from: from, senderName: nil, packetID: packetID, text: "x")).identifier)
                identifiers.insert(NotificationPlan.plan(
                    for: .flare(from: from, senderName: nil, packetID: packetID)).identifier)
                identifiers.insert(NotificationPlan.plan(
                    for: .rally(from: from, senderName: nil, packetID: packetID, text: "x",
                                     isBroadcast: true)).identifier)
            }
        }
        XCTAssertEqual(identifiers.count, 75, "25 of each kind, none of them colliding")
    }

    /// A DM and a crew message with the SAME packet id are the same
    /// packet — a broadcast this node also received directly cannot
    /// notify twice.
    func testA03_AC11_OnePacketIsOneNotificationWhicheverWayItIsRouted() {
        let dm = NotificationPlan.plan(for: .directMessage(from: 7, senderName: nil, packetID: 9, text: "x"))
        let crew = NotificationPlan.plan(for: .crewMessage(from: 7, senderName: nil, packetID: 9, text: "x"))
        XCTAssertEqual(dm.identifier, crew.identifier)
    }

    // MARK: - §3.11.2 — plain language (A02 §6.4)

    /// A02 §6.4 bans the jargon from anything a first-time reader sees.
    /// A notification is the MOST exposed surface in the app — it lands
    /// on a lock screen — so the ban is mechanical here, the same shape
    /// `SignalTierTests` uses to forbid numbers in the signal view.
    func testNotificationWordingCarriesNoJargon() {
        let banned = ["portnum", "dBm", "rssi", "snr", "nodenum", "!0", "packetid", "uuid", "ble", "gatt"]
        let events: [NotificationEvent] = [
            .flare(from: 4_098, senderName: "Taylor", packetID: 1),
            .flare(from: 4_098, senderName: nil, packetID: 1),
            .rally(from: 4_100, senderName: "Taylor", packetID: 2, text: "MY SPOT", isBroadcast: true),
            .directMessage(from: 8_193, senderName: "Taylor", packetID: 3, text: "on my way"),
            .crewMessage(from: 8_194, senderName: "Taylor", packetID: 4, text: "at the rail"),
        ]
        for event in events {
            let plan = NotificationPlan.plan(for: event)
            let visible = (plan.title + " " + plan.body).lowercased()
            for word in banned {
                XCTAssertFalse(visible.contains(word), "\(word) has no business on a lock screen: \(visible)")
            }
            // The node id must never be readable ON SCREEN either — it
            // is in the identifier and the deep link, which nobody sees.
            XCTAssertFalse(visible.contains("4098"))
            XCTAssertFalse(visible.contains("8193"))
        }
    }

    // MARK: - §3.11.3 — the deep links round-trip

    func testDeepLinksRoundTripToTheRouteTheyName() {
        XCTAssertEqual(route("firefly://thread/crew"), .thread(.crew))
        XCTAssertEqual(route("firefly://thread/dm/8193"), .thread(.member(8_193)))
        XCTAssertEqual(route("firefly://find/4098"), .find(nodeID: 4_098))
        XCTAssertEqual(route("firefly://find"), .find(nodeID: nil))
    }

    /// Every plan's own deep link parses back to a route — the property
    /// that matters, rather than four hand-written strings that could
    /// drift from what the builder emits.
    func testEveryPlanDeepLinkParses() {
        let events: [NotificationEvent] = [
            .flare(from: 4_098, senderName: "T", packetID: 1),
            .rally(from: 4_100, senderName: "T", packetID: 2, text: "x", isBroadcast: true),
            .directMessage(from: 8_193, senderName: "T", packetID: 3, text: "x"),
            .crewMessage(from: 8_194, senderName: "T", packetID: 4, text: "x"),
        ]
        for event in events {
            let link = NotificationPlan.plan(for: event).deepLink
            XCTAssertNotNil(route(link), "\(link) is a link nothing can route")
        }
    }

    /// Anything this app does not own returns `nil` — including another
    /// `firefly://` host, so A02's own crew links (same scheme,
    /// different job) pass straight through instead of being eaten here.
    func testUnknownLinksAreNotClaimed() {
        XCTAssertNil(route("firefly://crew/join/ABC123"))
        XCTAssertNil(route("firefly://thread"))
        XCTAssertNil(route("firefly://thread/dm"))
        XCTAssertNil(route("firefly://thread/dm/not-a-number"))
        XCTAssertNil(route("https://example.com/thread/crew"))
    }

    /// The router holds a route exactly once: a redraw must not
    /// re-navigate under the user.
    @MainActor
    func testRouterConsumesItsPendingRouteExactlyOnce() {
        let router = DeepLinkRouter()
        XCTAssertTrue(router.handle(URL(string: "firefly://thread/crew")!))
        XCTAssertEqual(router.pending, .thread(.crew))
        XCTAssertEqual(router.consume(), .thread(.crew))
        XCTAssertNil(router.consume())
        XCTAssertFalse(router.handle(URL(string: "firefly://crew/join/ABC")!), "not ours; nothing is queued")
        XCTAssertNil(router.pending)
    }

    private func route(_ string: String) -> NotificationRoute? {
        FireflyDeepLink.route(for: URL(string: string)!)
    }
}
