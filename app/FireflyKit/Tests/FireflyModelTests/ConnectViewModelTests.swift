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
}
