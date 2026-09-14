//
//  BackgroundConnectionDiagnosticsTests.swift — A03 §3.6/§3.10/§3.11 as
//  the Diagnostics screen actually renders them.
//
//  `BackgroundConnectionStatus` has its own honesty test in FireflyKit;
//  this one checks the half that lives in the app target: that the
//  screen reads the transport's real counters, and that it says UNKNOWN
//  — never "0", never a fabricated date — where there is nothing to
//  read. "We did not reconnect" and "there is no radio here to
//  reconnect" are different facts.
//
import FireflyMesh
import FireflyModel
import XCTest

/// An honest stand-in for `BLETransport`'s counters: returns exactly
/// what it was handed, invents nothing.
private final class StubLinkDiagnostics: BLELinkDiagnosticsProviding, @unchecked Sendable {
    private let value: BLELinkDiagnostics
    init(_ value: BLELinkDiagnostics) { self.value = value }
    func linkDiagnostics() async -> BLELinkDiagnostics { value }
}

private final class StubNotifications: NotificationSending, @unchecked Sendable {
    private let state: NotificationAuthorization
    init(_ state: NotificationAuthorization) { self.state = state }
    func post(_ plan: NotificationPlan) async {}
    @discardableResult func requestAuthorization() async -> Bool { false }
    func authorization() async -> NotificationAuthorization { state }
    func registerCategories() async {}
    func withdrawDelivered(threadIdentifier: String) async {}
}

@MainActor
final class BackgroundConnectionDiagnosticsTests: XCTestCase {

    private let t0 = Date(timeIntervalSince1970: 1_700_000_000)

    /// A stack with no BLE transport at all (the stub graph, the iOS
    /// Simulator) reports UNKNOWN for all three counters — the same rule
    /// every other unavailable row on this screen has always followed.
    func testCountersReadUnknownWhenThereIsNoTransportToAsk() {
        let model = DiagnosticsViewModel(client: StubMeshtasticClient(), now: { self.t0 })
        XCTAssertEqual(model.scanStartsLabel, DiagnosticsViewModel.unknown)
        XCTAssertEqual(model.reconnectsLabel, DiagnosticsViewModel.unknown)
        XCTAssertEqual(model.lastReconnectLabel, DiagnosticsViewModel.unknown)
        XCTAssertFalse(model.hasLinkDiagnostics)
    }

    /// With a transport, the counters are that transport's own
    /// observations — including a genuine zero, which is a real reading
    /// and is rendered as one.
    func testCountersRenderTheTransportsOwnObservations() async {
        let diagnostics = BLELinkDiagnostics(scanStarts: 4, reconnects: 2,
                                              lastReconnectAt: t0.addingTimeInterval(-360))
        let model = DiagnosticsViewModel(client: StubMeshtasticClient(), now: { self.t0 },
                                          linkDiagnostics: StubLinkDiagnostics(diagnostics),
                                          notifications: StubNotifications(.authorized),
                                          backgroundConnectEnabled: { true })
        model.observe()
        await eventually { model.scanStartsLabel == "4" }
        XCTAssertEqual(model.reconnectsLabel, "2")
        XCTAssertEqual(model.lastReconnectLabel, "6 min ago", "the app's one age vocabulary, not a second one")
        XCTAssertEqual(model.notificationsLabel, "Allowed")
        model.stopObserving()
    }

    /// A transport that has never reconnected in this process reports
    /// UNKNOWN for WHEN — not "never", which would claim more than we
    /// know, and not a date.
    func testAnUnobservedReconnectTimeIsUnknownRatherThanInvented() async {
        let model = DiagnosticsViewModel(client: StubMeshtasticClient(), now: { self.t0 },
                                          linkDiagnostics: StubLinkDiagnostics(BLELinkDiagnostics()),
                                          notifications: StubNotifications(.notDetermined),
                                          backgroundConnectEnabled: { true })
        model.observe()
        await eventually { model.reconnectsLabel == "0" }
        XCTAssertEqual(model.lastReconnectLabel, DiagnosticsViewModel.unknown)
        XCTAssertEqual(model.notificationsLabel, "Not asked yet")
        model.stopObserving()
    }

    // MARK: - A03 §3.1 (S1b) — the two restoration rows

    /// The rows §6's P3 is actually read from, under this file's own
    /// rule: UNKNOWN, never "0" and never a fabricated date, where there
    /// is no BLE transport to ask. "This process was not restored" and
    /// "there is nothing here that could be restored" are different
    /// facts, and P3a is uninterpretable if the screen conflates them.
    ///
    /// PR #317 review (Tier 3): the rows shipped without this.
    func testRestorationRowsReadUnknownWhenThereIsNoTransportToAsk() {
        let model = DiagnosticsViewModel(client: StubMeshtasticClient(), now: { self.t0 })
        XCTAssertEqual(model.restoredSessionsLabel, DiagnosticsViewModel.unknown)
        XCTAssertEqual(model.lastRestoreLabel, DiagnosticsViewModel.unknown)
    }

    /// With a transport, both rows are that transport's own
    /// observations — and "Last restore" carries the state it restored
    /// INTO, in the register this screen uses rather than an enum case
    /// name.
    func testRestorationRowsRenderTheTransportsOwnObservations() async {
        let diagnostics = BLELinkDiagnostics(restores: 2,
                                              lastRestoreAt: t0.addingTimeInterval(-360),
                                              lastRestoreAction: .adoptConnected)
        let model = DiagnosticsViewModel(client: StubMeshtasticClient(), now: { self.t0 },
                                          linkDiagnostics: StubLinkDiagnostics(diagnostics),
                                          notifications: StubNotifications(.authorized),
                                          backgroundConnectEnabled: { true })
        model.observe()
        await eventually { model.restoredSessionsLabel == "2" }
        XCTAssertEqual(model.lastRestoreLabel, "6 min ago \u{00B7} still connected")
        model.stopObserving()
    }

    /// A transport that was never restored reports a genuine `0` for HOW
    /// MANY — it was asked and that is the answer — but UNKNOWN for
    /// WHEN, because there is no date to age and inventing one is the
    /// exact failure `lastReconnectAt` already guards against.
    func testAProcessThatWasNeverRestoredReportsZeroAndAnUnknownTime() async {
        let model = DiagnosticsViewModel(client: StubMeshtasticClient(), now: { self.t0 },
                                          linkDiagnostics: StubLinkDiagnostics(BLELinkDiagnostics()),
                                          notifications: StubNotifications(.authorized),
                                          backgroundConnectEnabled: { true })
        model.observe()
        await eventually { model.restoredSessionsLabel == "0" }
        XCTAssertEqual(model.lastRestoreLabel, DiagnosticsViewModel.unknown)
        model.stopObserving()
    }

    /// Every `BLERestoreAction` has its own words: "still connecting"
    /// and "had dropped" are genuinely different outcomes from "still
    /// connected", and P3c's result turns on which one it was.
    func testEveryRestoreActionHasItsOwnWords() {
        let words = [BLERestoreAction.adoptConnected, .keepPendingConnect, .reconnect]
            .map(DiagnosticsViewModel.restoreWords)
        XCTAssertEqual(words, ["still connected", "still connecting", "had dropped"])
        XCTAssertEqual(Set(words).count, 3)
    }

    /// The status line is the pure one from FireflyModel, rendered — not
    /// a second sentence assembled here that could drift from it.
    func testTheStatusLineIsTheSharedOneAndSaysOffWhenTheSettingIsOff() {
        let model = DiagnosticsViewModel(client: StubMeshtasticClient(), now: { self.t0 },
                                          backgroundConnectEnabled: { false })
        XCTAssertEqual(model.backgroundConnectionStatus.headline, .off)
        XCTAssertTrue(model.backgroundConnectionLabel.hasPrefix("Off"))
        XCTAssertTrue(model.backgroundConnectionLabel.contains("Firefly disconnects when you leave the app."))
    }

    /// The word "connected" never appears while the link is not
    /// `.ready` — A03_AC14, re-checked at the rendering layer, because a
    /// label that glued the headline and detail together wrongly could
    /// reintroduce exactly what the pure test forbids.
    func testTheRenderedLabelNeverClaimsConnectedWhileTheLinkIsDown() {
        let model = DiagnosticsViewModel(client: StubMeshtasticClient(), now: { self.t0 },
                                          backgroundConnectEnabled: { true })
        XCTAssertEqual(model.link, .disconnected, "no link has been observed yet")
        XCTAssertFalse(model.backgroundConnectionLabel.lowercased().contains("connected"),
                        model.backgroundConnectionLabel)
    }

    private func eventually(_ condition: @escaping () -> Bool, timeout: Int = 200) async {
        for _ in 0..<timeout {
            if condition() { return }
            try? await Task.sleep(nanoseconds: 10_000_000)
        }
        XCTFail("condition never became true")
    }
}
