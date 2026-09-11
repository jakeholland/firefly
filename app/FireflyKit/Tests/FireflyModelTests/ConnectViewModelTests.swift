//
//  ConnectViewModelTests.swift — MVVM against a protocol, no radio.
//
import FireflyMesh
import FireflyModel
import MeshtasticProto
import XCTest

@MainActor
final class ConnectViewModelTests: XCTestCase {

    // MARK: - M2 (PR #272 review) test support — a REAL `MeshtasticClient`
    // over `LoopbackTransport`, needed only for the `alreadyConnecting`
    // race test below: `StubMeshtasticClient` has no reentrancy guard at
    // all, so it cannot produce the race being pinned. No radio, no
    // CoreBluetooth — same mocked-transport discipline
    // `ClientReconnectTests` (FireflyMeshTests) uses.

    private struct TestTimeout: Error {}

    private func waitForSentCount(_ n: Int, on transport: LoopbackTransport,
                                   file: StaticString = #filePath, line: UInt = #line) async throws {
        for _ in 0..<400 {
            if transport.sentMessages.count >= n { return }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTFail("timed out waiting for \(n) sent message(s); saw \(transport.sentMessages.count)", file: file, line: line)
        throw TestTimeout()
    }

    private func fromRadio(_ build: (inout FromRadio) -> Void) -> Data {
        var fr = FromRadio()
        build(&fr)
        return (try? fr.serializedData()) ?? Data()
    }

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

    // MARK: - BLOCKING 2 (PR #272 review): the alreadyConnecting race

    /// `AppGraph`'s launch auto-connect racing a user's own CONNECT tap
    /// on cold launch — the exact scenario the review named. Driven over
    /// a REAL `MeshtasticClient` (not `StubMeshtasticClient`, which has
    /// no reentrancy guard to race at all) so the LOSING call actually
    /// throws `MeshtasticClientError.alreadyConnecting` the same way
    /// production does, through `ConnectViewModel.connect()` itself.
    func testAlreadyConnectingRaceNeverShowsFailed() async throws {
        let transport = LoopbackTransport()
        let client = MeshtasticClient(transport: transport)
        let vm = ConnectViewModel(client: client)
        vm.observe()

        // The WINNING call — `AppGraph.autoConnectToLastKnownPeripheral()`'s
        // own shape: fired off as its own `Task`, never awaited inline.
        let winner = Task { try? await client.connect() }
        // The first call's reentrancy guard is set synchronously, before
        // its first suspension point — a short sleep is enough to make
        // the second call the deterministic loser (same convention
        // `ClientReconnectTests.testConcurrentConnectCallsDoNotDuplicateTheReceiveSubscription`
        // uses).
        try await Task.sleep(for: .milliseconds(5))

        // The user's own manual CONNECT tap — the LOSING call.
        await vm.connect()

        XCTAssertNotEqual(vm.statusLabel, "FAILED",
                           "a losing connect() call's .alreadyConnecting must never surface as FAILED")
        XCTAssertNil(vm.lastError)

        // Let the WINNING call actually finish, so ITS OWN stream events
        // — not this losing call — are what report the real outcome.
        try await waitForSentCount(2, on: transport) // heartbeat, want_config(onlyConfig)
        transport.inject(fromRadio { $0.myInfo.myNodeNum = 7 })
        transport.inject(fromRadio { $0.configCompleteID = MeshtasticConfigNonce.onlyConfig })
        try await waitForSentCount(3, on: transport) // want_config(onlyNodeDB)
        transport.inject(fromRadio { $0.configCompleteID = MeshtasticConfigNonce.onlyNodeDB })
        _ = await winner.value

        for _ in 0..<200 where vm.link != .ready {
            try? await Task.sleep(nanoseconds: 5_000_000)
        }
        XCTAssertEqual(vm.link, .ready, "the WINNING call's own stream events report the real state")
        XCTAssertNil(vm.lastError)
        vm.stopObserving()
    }

    // MARK: - SHOULD-FIX 3 (PR #272 review): CONNECT/DISCONNECT gating state matrix

    /// Every `LinkState` case, and what each button must read for it —
    /// pinned as one table rather than one assertion per state so the
    /// whole matrix is visible at a glance and a future state added to
    /// `LinkState` without an entry here fails a `switch` inside
    /// `isBusyOrConnected`/`isDisconnectable` at compile time, not
    /// silently here.
    func testConnectDisconnectGatingStateMatrix() {
        let vm = ConnectViewModel(client: StubMeshtasticClient())
        let cases: [(LinkState, connectDisabled: Bool, disconnectEnabled: Bool)] = [
            (.disconnected, false, false),
            (.connecting, true, true),
            (.handshaking, true, true),
            (.ready, true, true),
            (.reconnecting(attempt: 1), true, true),
            (.failed("x"), false, false),
        ]
        for (state, connectDisabled, disconnectEnabled) in cases {
            vm.apply(state)
            XCTAssertEqual(vm.isBusyOrConnected, connectDisabled, "CONNECT gating wrong for \(state)")
            XCTAssertEqual(vm.isDisconnectable, disconnectEnabled, "DISCONNECT gating wrong for \(state)")
        }
    }

    /// The specific regression SHOULD-FIX 3 names: `.reconnecting` can
    /// now last minutes (the bounded handshake-retry loop), and DISCONNECT
    /// must stay reachable throughout it, unlike the old `link == .ready`
    /// gate.
    func testReconnectingLeavesDisconnectReachable() {
        let vm = ConnectViewModel(client: StubMeshtasticClient())
        vm.apply(.reconnecting(attempt: 4))
        XCTAssertTrue(vm.isDisconnectable, "a stuck retry loop must always be abortable")
    }

    // MARK: - NIT (PR #272 review): a visible RETRY action after the bounded retry is exhausted

    func testConnectButtonLabelIsRetryOnlyWhenFailed() {
        let vm = ConnectViewModel(client: StubMeshtasticClient())
        for state: LinkState in [.disconnected, .connecting, .handshaking, .ready, .reconnecting(attempt: 2)] {
            vm.apply(state)
            XCTAssertEqual(vm.connectButtonLabel, "CONNECT", "wrong label for \(state)")
        }
        vm.apply(.failed("handshake retry exhausted"))
        XCTAssertEqual(vm.connectButtonLabel, "RETRY",
                        "the terminal state after the bounded retry loop gives up must offer a visible next action")
    }

    // MARK: - SHOULD-FIX 5 (PR #272 review): "Forget this node"

    func testCanForgetNodeIsFalseWithNoStoreAtAll() {
        let vm = ConnectViewModel(client: StubMeshtasticClient())
        XCTAssertFalse(vm.canForgetNode, "no settings seam at all — nothing to forget through")
    }

    func testCanForgetNodeReflectsWhetherAPeripheralIsRemembered() {
        let store = InMemorySettingsStore()
        let vm = ConnectViewModel(client: StubMeshtasticClient(), store: store)
        XCTAssertFalse(vm.canForgetNode, "nothing persisted yet")

        store.setString("11111111-1111-1111-1111-111111111111", .lastPeripheralID)
        XCTAssertTrue(vm.canForgetNode)
    }

    func testForgetNodeClearsThePersistedPeripheralAndDisconnects() async {
        let store = InMemorySettingsStore()
        store.setString("11111111-1111-1111-1111-111111111111", .lastPeripheralID)
        let client = StubMeshtasticClient()
        let vm = ConnectViewModel(client: client, store: store)
        vm.observe()
        await vm.connect()
        for _ in 0..<200 where vm.link != .ready {
            try? await Task.sleep(nanoseconds: 5_000_000)
        }

        await vm.forgetNode()

        XCTAssertNil(store.string(.lastPeripheralID), "FORGET must clear the remembered peripheral")
        for _ in 0..<200 where vm.link != .disconnected {
            try? await Task.sleep(nanoseconds: 5_000_000)
        }
        XCTAssertEqual(vm.link, .disconnected, "FORGET must also disconnect")
        vm.stopObserving()
    }

    /// SHOULD-FIX 5's other half: a PLAIN `disconnect()` must NOT touch
    /// the remembered peripheral — matches Meshtastic-Apple's own
    /// `AccessoryManager.disconnect()`. `forgetNode()`, above, is the
    /// only action that clears it.
    func testPlainDisconnectDoesNotClearTheRememberedPeripheral() async {
        let store = InMemorySettingsStore()
        store.setString("11111111-1111-1111-1111-111111111111", .lastPeripheralID)
        let vm = ConnectViewModel(client: StubMeshtasticClient(), store: store)

        await vm.disconnect()

        XCTAssertEqual(store.string(.lastPeripheralID), "11111111-1111-1111-1111-111111111111",
                        "plain DISCONNECT must keep the remembered node")
    }

    func testForgetNodeWithNoStoreStillDisconnects() async {
        let client = StubMeshtasticClient()
        let vm = ConnectViewModel(client: client)
        vm.observe()
        await vm.connect()
        for _ in 0..<200 where vm.link != .ready {
            try? await Task.sleep(nanoseconds: 5_000_000)
        }

        await vm.forgetNode() // no store at all — must not crash, must still disconnect

        for _ in 0..<200 where vm.link != .disconnected {
            try? await Task.sleep(nanoseconds: 5_000_000)
        }
        XCTAssertEqual(vm.link, .disconnected)
        vm.stopObserving()
    }

    // MARK: - Connect-screen redesign (owner feedback: "not sure to
    // which radio, the connect button needs to be on the line item")

    private func snapshot(num: UInt32, longName: String?) -> MeshNodeSnapshot {
        MeshNodeSnapshot(num: num, shortName: nil, longName: longName, position: nil,
                          lastHeard: nil, rssiDbm: nil, snrDb: nil, hopsAway: nil)
    }

    /// `headerStatusText`'s "state → header text" table for every
    /// `LinkState` that does not depend on a real node identity — the
    /// full CONNECTED example (BLE name + node long name + id + RSSI
    /// together) is its own test below, since it needs a connected
    /// node num to hang the id off.
    func testHeaderStatusTextTable() {
        let cases: [(setup: (ConnectViewModel) -> Void, expected: String)] = [
            ({ _ in }, "NOT CONNECTED"),
            ({ vm in
                vm.noteSelectedPeripheral(name: "Meshtastic_e7d4", rssiDbm: nil)
                vm.apply(.connecting)
            }, "CONNECTING · Meshtastic_e7d4"),
            ({ vm in
                vm.noteSelectedPeripheral(name: "Meshtastic_e7d4", rssiDbm: nil)
                vm.apply(.handshaking)
            }, "HANDSHAKING · Meshtastic_e7d4"),
            ({ vm in vm.apply(.reconnecting(attempt: 2)) }, "RECONNECTING (attempt 2)"),
            ({ vm in vm.apply(.failed("timeout")) }, "FAILED"),
        ]
        for (setup, expected) in cases {
            let vm = ConnectViewModel(client: StubMeshtasticClient())
            setup(vm)
            XCTAssertEqual(vm.headerStatusText, expected)
        }
    }

    /// The owner's own worked example, verbatim: "CONNECTED ·
    /// Meshtastic_e7d4 · Firefly 2 · !02e5e3d4 · −56 dBm".
    func testHeaderStatusTextOnceReadyNamesTheRadioNodeAndSignal() {
        let client = StubMeshtasticClient()
        client.connectedNodeNum = 0x02e5_e3d4
        let vm = ConnectViewModel(client: client)
        vm.noteSelectedPeripheral(name: "Meshtastic_e7d4", rssiDbm: -56)
        vm.apply(.ready)
        vm.apply(snapshot(num: 0x02e5_e3d4, longName: "Firefly 2"))
        XCTAssertEqual(vm.headerStatusText, "CONNECTED · Meshtastic_e7d4 · Firefly 2 · !02e5e3d4 · -56 dBm")
    }

    func testHeaderStatusTextClearsTheRadioOnAFullDisconnect() {
        let vm = ConnectViewModel(client: StubMeshtasticClient())
        vm.noteSelectedPeripheral(name: "Meshtastic_e7d4", rssiDbm: -56)
        vm.apply(.connecting)
        XCTAssertNotEqual(vm.headerStatusText, "NOT CONNECTED")

        vm.apply(.disconnected)
        XCTAssertEqual(vm.headerStatusText, "NOT CONNECTED",
                        "a clean disconnect has no 'which radio' left to keep naming")
    }

    // MARK: - `apply(_:MeshNodeSnapshot)` — node-identity half of `connectedRadio`

    func testApplySnapshotIgnoresAnotherNodesLongName() {
        let client = StubMeshtasticClient()
        client.connectedNodeNum = 100
        let vm = ConnectViewModel(client: client)
        vm.apply(snapshot(num: 200, longName: "Somebody Else"))
        XCTAssertNil(vm.connectedRadio?.longName, "a stranger's NodeInfo must never name MY radio")
    }

    func testApplySnapshotFillsInTheConnectedNodesLongName() {
        let client = StubMeshtasticClient()
        client.connectedNodeNum = 100
        let vm = ConnectViewModel(client: client)
        vm.apply(snapshot(num: 100, longName: "Firefly 2"))
        XCTAssertEqual(vm.connectedRadio?.longName, "Firefly 2")
    }

    func testApplySnapshotIgnoresABlankLongName() {
        let client = StubMeshtasticClient()
        client.connectedNodeNum = 100
        let vm = ConnectViewModel(client: client)
        vm.apply(snapshot(num: 100, longName: ""))
        XCTAssertNil(vm.connectedRadio?.longName, "an empty name is not a name — never shown as a blank bullet")
    }

    func testNoteSelectedPeripheralResetsLongNameForAFreshSelection() {
        let client = StubMeshtasticClient()
        client.connectedNodeNum = 5
        let vm = ConnectViewModel(client: client)
        vm.apply(snapshot(num: 5, longName: "Old Radio"))
        XCTAssertEqual(vm.connectedRadio?.longName, "Old Radio")

        vm.noteSelectedPeripheral(name: "Meshtastic_9999", rssiDbm: -70)
        XCTAssertNil(vm.connectedRadio?.longName,
                      "a fresh row tap is a DIFFERENT radio — must not keep the old one's name")
        XCTAssertEqual(vm.connectedRadio?.bleName, "Meshtastic_9999")
        XCTAssertEqual(vm.connectedRadio?.rssiDbm, -70)
    }

    // MARK: - `connectedNodeIDHex`

    func testConnectedNodeIDHexNilBeforeAnyConnectAttempt() {
        XCTAssertNil(ConnectViewModel(client: StubMeshtasticClient()).connectedNodeIDHex)
    }

    /// The demo-mode case: `DemoWorld.nodeDB` deliberately never
    /// includes "my" own node, so there is no `NodeInfo` snapshot to
    /// learn an id from — `connectedNodeNum` must still name it.
    func testConnectedNodeIDHexUsesConnectedNodeNumEvenWithoutANodeInfoSnapshot() {
        let client = StubMeshtasticClient()
        client.connectedNodeNum = 0x0000_1001
        let vm = ConnectViewModel(client: client)
        vm.apply(.ready)
        XCTAssertEqual(vm.connectedNodeIDHex, "!00001001")
    }

    func testConnectedNodeIDHexClearsOnDisconnect() {
        let client = StubMeshtasticClient()
        client.connectedNodeNum = 0x0000_1001
        let vm = ConnectViewModel(client: client)
        vm.apply(.ready)
        client.connectedNodeNum = nil
        vm.apply(.disconnected)
        XCTAssertNil(vm.connectedNodeIDHex)
    }

    // MARK: - `rememberedPeripheralID`

    func testRememberedPeripheralIDReflectsTheStore() {
        let store = InMemorySettingsStore()
        let vm = ConnectViewModel(client: StubMeshtasticClient(), store: store)
        XCTAssertNil(vm.rememberedPeripheralID, "nothing persisted yet")

        store.setString("11111111-1111-1111-1111-111111111111", .lastPeripheralID)
        XCTAssertEqual(vm.rememberedPeripheralID, "11111111-1111-1111-1111-111111111111")
    }

    // MARK: - `rowAction(isActivePeripheral:)` — per-row CONNECT/DISCONNECT gating

    /// Owner feedback: "the connect button needs to be on the line
    /// item or something". Pinned as one table, same convention as
    /// `testConnectDisconnectGatingStateMatrix` above: connect disabled
    /// on every OTHER row while busy, and disconnect reachable ONLY on
    /// the active row.
    func testRowActionGatingTable() {
        let vm = ConnectViewModel(client: StubMeshtasticClient())
        let cases: [(LinkState, active: ConnectViewModel.RadioRowAction, other: ConnectViewModel.RadioRowAction)] = [
            (.disconnected, .connect, .connect),
            (.connecting, .disconnect, .unavailable),
            (.handshaking, .disconnect, .unavailable),
            (.ready, .disconnect, .unavailable),
            (.reconnecting(attempt: 1), .disconnect, .unavailable),
            (.failed("x"), .connect, .connect),
        ]
        for (state, active, other) in cases {
            vm.apply(state)
            XCTAssertEqual(vm.rowAction(isActivePeripheral: true), active, "active row wrong for \(state)")
            XCTAssertEqual(vm.rowAction(isActivePeripheral: false), other, "other row wrong for \(state)")
        }
    }

    /// The specific regression this table guards against: DISCONNECT
    /// must be reachable on the active row throughout the WHOLE busy
    /// window, matching `isDisconnectable`'s own rule above, not just
    /// once `.ready`.
    func testActiveRowStaysDisconnectableThroughoutReconnecting() {
        let vm = ConnectViewModel(client: StubMeshtasticClient())
        vm.apply(.reconnecting(attempt: 4))
        XCTAssertEqual(vm.rowAction(isActivePeripheral: true), .disconnect)
        XCTAssertTrue(vm.isDisconnectable, "a stuck retry loop must always be abortable")
    }
}
