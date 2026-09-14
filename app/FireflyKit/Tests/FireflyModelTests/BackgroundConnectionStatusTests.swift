//
//  BackgroundConnectionStatusTests.swift — A03 §3.10's honesty rule.
//
//  A03_AC14 states the mechanical half of §3.10 as a property rather
//  than a table lookup: **the substring "connected" never appears for
//  any input whose link state is not `.ready`.** S1a ships the subset of
//  the table this build can actually observe (§7.0's cut), so the
//  property is what is pinned exhaustively here — a table row can be
//  added later without weakening it.
//
import FireflyMesh
import FireflyModel
import XCTest

final class BackgroundConnectionStatusTests: XCTestCase {

    private let t0 = Date(timeIntervalSince1970: 1_700_000_000)

    private func inputs(enabled: Bool = true, link: LinkState = .ready, lastReconnectAt: Date? = nil,
                        notifications: NotificationAuthorization = .authorized)
        -> BackgroundConnectionStatus.Inputs {
        BackgroundConnectionStatus.Inputs(backgroundConnectEnabled: enabled, link: link,
                                           lastReconnectAt: lastReconnectAt, notifications: notifications, now: t0)
    }

    private var everyLinkState: [LinkState] {
        [.disconnected, .connecting, .handshaking, .ready, .reconnecting(attempt: 1),
         .reconnecting(attempt: 9), .failed("no route to the radio")]
    }

    /// **A03_AC14.** Exhaustive over every link state, both settings,
    /// every authorization state and with/without a measured reconnect:
    /// the word "connected" appears only when the link is genuinely
    /// `.ready`. This is the rule that stops the status line claiming
    /// background coverage it does not have (§3.13(3)).
    func testA03_AC14_TheWordConnectedNeverAppearsUnlessTheLinkIsReady() {
        for link in everyLinkState {
            for enabled in [true, false] {
                for auth in NotificationAuthorization.allCases {
                    for reconnect in [Date?.none, t0.addingTimeInterval(-360)] {
                        let status = BackgroundConnectionStatus.status(
                            inputs(enabled: enabled, link: link, lastReconnectAt: reconnect, notifications: auth))
                        let line = (status.headline.rawValue + " " + status.detail).lowercased()
                        if link == .ready { continue }
                        // AC14's own rule, mechanically: the SUBSTRING
                        // "connected". That catches "reconnected" too,
                        // which is the trap — a line saying "last
                        // reconnected 6 min ago" while the link is down
                        // reads, at a glance on a lock screen, as a
                        // claim about right now.
                        XCTAssertFalse(line.contains("connected"),
                                        "\(link) must not read as connected: \(line)")
                    }
                }
            }
        }
    }

    /// Setting off: says what off means, in the words the toggle uses.
    func testOffSaysWhatOffDoes() {
        let status = BackgroundConnectionStatus.status(inputs(enabled: false, link: .ready))
        XCTAssertEqual(status.headline, .off)
        XCTAssertEqual(status.detail, "Firefly disconnects when you leave the app.")
    }

    func testOnAndReadySaysStayingConnected() {
        let status = BackgroundConnectionStatus.status(inputs(link: .ready))
        XCTAssertEqual(status.headline, .on)
        XCTAssertTrue(status.detail.hasPrefix("Staying connected in your pocket."))
    }

    /// §3.10's own row: connected, but Firefly cannot alert anyone. A
    /// status line that said "staying connected" and nothing else would
    /// be true and useless — the user's FLAREs are going nowhere.
    func testOnAndReadyButNotificationsBlockedSaysSo() {
        for auth in [NotificationAuthorization.denied, .notDetermined] {
            let status = BackgroundConnectionStatus.status(inputs(link: .ready, notifications: auth))
            XCTAssertEqual(status.headline, .on)
            XCTAssertEqual(status.detail,
                            "Staying connected, but Firefly can't alert you. Turn on notifications.")
        }
    }

    /// A measured reconnect is rendered with the SHIPPED age helper
    /// (`PresenceAge`, PR #304) — one vocabulary for ages across Crew,
    /// Inbox, Radar and this row, not two.
    func testAMeasuredReconnectIsRenderedInTheAppsOneAgeVocabulary() {
        let status = BackgroundConnectionStatus.status(
            inputs(link: .reconnecting(attempt: 2), lastReconnectAt: t0.addingTimeInterval(-360)))
        XCTAssertTrue(status.detail.contains(PresenceAge.ago(360)), status.detail)
        XCTAssertTrue(status.detail.contains("Last came back on its own 6 min ago."), status.detail)
    }

    /// No observation, no sentence. An absent reconnect time renders as
    /// nothing at all rather than "never" or a zero — the same rule
    /// `DiagnosticsViewModel.uptimeLabel` follows with UNKNOWN.
    func testAnAbsentReconnectTimeIsNotFabricated() {
        let status = BackgroundConnectionStatus.status(inputs(link: .reconnecting(attempt: 1)))
        XCTAssertFalse(status.detail.contains("ago"))
        XCTAssertFalse(status.detail.contains("0"))
        XCTAssertEqual(status.detail, "Lost your puck. Still looking.")
    }

    /// `.failed` is the one state that is not "on, still trying" — the
    /// app has stopped, and says so rather than implying it is working.
    func testFailedSaysStopped() {
        let status = BackgroundConnectionStatus.status(inputs(link: .failed("gave up")))
        XCTAssertEqual(status.headline, BackgroundConnectionStatus.Headline.stopped)
    }

    /// A02 §6.4: no jargon on a surface a first-time reader sees.
    func testNoJargonInAnyLine() {
        let banned = ["dbm", "rssi", "gatt", "ble", "corebluetooth", "peripheral", "uuid", "nodenum"]
        for link in everyLinkState {
            for enabled in [true, false] {
                for auth in NotificationAuthorization.allCases {
                    let status = BackgroundConnectionStatus.status(
                        inputs(enabled: enabled, link: link, notifications: auth))
                    let line = status.detail.lowercased()
                    for word in banned {
                        XCTAssertFalse(line.contains(word), "\(word) in: \(line)")
                    }
                }
            }
        }
    }
}
