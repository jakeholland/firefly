//
//  ConnectViewModelTests.swift — MVVM against a protocol, no radio.
//
import FireflyMesh
import FireflyModel
import XCTest

@MainActor
final class ConnectViewModelTests: XCTestCase {

    func testStartsDisconnected() {
        let vm = ConnectViewModel(client: StubMeshtasticClient())
        XCTAssertEqual(vm.link, .disconnected)
        XCTAssertEqual(vm.statusLabel, "NOT CONNECTED")
        XCTAssertNil(vm.lastError)
    }

    /// HANDSHAKING is its own state on purpose: the node database is not
    /// trustworthy until `config_complete_id` matches, so a screen that
    /// said CONNECTED there would be showing an empty crew as an answer.
    func testHandshakingIsNotReportedAsConnected() {
        let vm = ConnectViewModel(client: StubMeshtasticClient())
        vm.apply(.handshaking)
        XCTAssertEqual(vm.statusLabel, "HANDSHAKING")
        vm.apply(.ready)
        XCTAssertEqual(vm.statusLabel, "CONNECTED")
    }

    func testFailureIsSurfacedWithItsReason() {
        let vm = ConnectViewModel(client: StubMeshtasticClient())
        vm.apply(.failed("bluetooth is off"))
        XCTAssertEqual(vm.statusLabel, "FAILED")
        XCTAssertEqual(vm.lastError, "bluetooth is off")
    }

    func testConnectReachesReadyOverAStubClient() async {
        let vm = ConnectViewModel(client: StubMeshtasticClient())
        vm.observe()
        await vm.connect()
        // The stream delivery is async; poll briefly rather than sleep a
        // fixed interval.
        for _ in 0..<200 where vm.link != .ready {
            try? await Task.sleep(nanoseconds: 5_000_000)
        }
        XCTAssertEqual(vm.link, .ready)
        XCTAssertNil(vm.lastError)
    }

    // MARK: - M2: reconnecting, with an attempt count and "last connected X ago"

    func testReconnectingReportsItsAttemptCount() {
        let vm = ConnectViewModel(client: StubMeshtasticClient())
        vm.apply(.reconnecting(attempt: 3))
        XCTAssertEqual(vm.statusLabel, "RECONNECTING (attempt 3)",
                        "a silent HANDSHAKING during a multi-minute retry loop is not telling the truth")
    }

    func testLastConnectedLabelIsNilUntilTheFirstReadyAndNilWhileReady() {
        let vm = ConnectViewModel(client: StubMeshtasticClient())
        XCTAssertNil(vm.lastConnectedLabel, "never connected yet — nothing to say")
        vm.apply(.ready)
        XCTAssertNil(vm.lastConnectedLabel, "connected right now — 'last connected' would be a lie")
    }

    func testLastConnectedLabelReportsElapsedTimeOnceTheLinkDrops() {
        var now = Date(timeIntervalSince1970: 1_000)
        let vm = ConnectViewModel(client: StubMeshtasticClient(), now: { now })
        vm.apply(.ready)
        now = now.addingTimeInterval(95) // 1m 35s later
        vm.apply(.reconnecting(attempt: 1))
        XCTAssertEqual(vm.lastConnectedLabel, "last connected 1m ago")
    }

    func testRelativeAgoFormatting() {
        let base = Date(timeIntervalSince1970: 10_000)
        XCTAssertEqual(ConnectViewModel.relativeAgo(from: base, to: base.addingTimeInterval(5)), "5s ago")
        XCTAssertEqual(ConnectViewModel.relativeAgo(from: base, to: base.addingTimeInterval(125)), "2m ago")
        XCTAssertEqual(ConnectViewModel.relativeAgo(from: base, to: base.addingTimeInterval(3 * 3600 + 60)), "3h ago")
    }
}
