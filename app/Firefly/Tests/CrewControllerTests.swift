//
//  CrewControllerTests.swift — Slice B view-model tests
//  (`docs/specs/A02-crew-join.md`, AC6, AC7, AC10, §4.5). No network, no
//  BLE — `StubMeshtasticClient` plus the in-memory
//  `CrewProfileStoring`/`CrewSnapshotStoring`/`CrewHiddenStoring` stand-
//  ins, same convention `AppDependencies.stub()` uses elsewhere.
//
import FireflyMesh
import FireflyModel
import MeshtasticProto
import XCTest

@MainActor
final class CrewControllerTests: XCTestCase {
    private func makeController(
        profileStore: any CrewProfileStoring = InMemoryCrewProfileStore(),
        snapshotStore: any CrewSnapshotStoring = InMemoryCrewSnapshotStore(),
        hiddenStore: any CrewHiddenStoring = InMemoryCrewHiddenStore()
    ) -> (CrewController, StubMeshtasticClient) {
        let client = StubMeshtasticClient()
        client.connectedNodeNum = 48_621_524
        let controller = CrewController(
            client: client, profileStore: profileStore, snapshotStore: snapshotStore, hiddenStore: hiddenStore)
        return (controller, client)
    }

    // MARK: - A02_AC6 — Start mints, snapshots once, writes a replace plan

    func testA02_AC6_startMintsCodeAndWritesReplacePlanAtIndexZero() async {
        let (controller, client) = makeController()
        client.nodeConfig = NodeConfigSnapshot(region: .us)

        let began = await controller.beginStart(humanName: "Camp Firefly")
        XCTAssertTrue(began)
        guard case .start(let code, "Camp Firefly") = controller.pending else {
            return XCTFail("expected a staged .start")
        }

        let confirmed = await controller.confirmApply()
        XCTAssertTrue(confirmed)
        XCTAssertEqual(controller.profile?.code, code.canonical)
        XCTAssertEqual(controller.profile?.humanName, "Camp Firefly")
        XCTAssertTrue(controller.hasCrew)

        XCTAssertEqual(client.sentChannelWriteLog.count, 1)
        let request = client.sentChannelWriteLog[0]
        XCTAssertNil(request.loraConfig, "§1.7 — a crew join never writes lora_config")
        guard let primary = request.channels.first(where: { $0.index == 0 }) else {
            return XCTFail("no channel written at index 0")
        }
        XCTAssertEqual(primary.role, .primary)
        XCTAssertEqual(primary.settings.name, code.canonical)
        XCTAssertEqual(primary.settings.moduleSettings.positionPrecision, 32)
    }

    func testA02_AC6_snapshotIsTakenOnceNeverOverwritten() async {
        let snapshotStore = InMemoryCrewSnapshotStore()
        let (controller, client) = makeController(snapshotStore: snapshotStore)
        client.nodeConfig = NodeConfigSnapshot(region: .us)
        var stockPrimary = ChannelSettings()
        stockPrimary.name = "LongFast"
        stockPrimary.psk = Data([0x01])
        client.channelTable = [{
            var channel = Channel()
            channel.index = 0
            channel.role = .primary
            channel.settings = stockPrimary
            return channel
        }()]

        _ = await controller.beginStart(humanName: "First")
        _ = await controller.confirmApply()
        XCTAssertEqual(snapshotStore.load()?.name, "LongFast")

        // A second Start (e.g. "Start a new crew" from Advanced) must
        // NOT overwrite the snapshot with the crew channel it just wrote.
        _ = await controller.beginStart(humanName: "Second")
        _ = await controller.confirmApply()
        XCTAssertEqual(snapshotStore.load()?.name, "LongFast")
    }

    // MARK: - A02_AC7 — region gate

    func testA02_AC7_regionUnsetBlocksAndSetRegionUnsetIsNeverCalled() async {
        let (controller, client) = makeController()
        client.nodeConfig = NodeConfigSnapshot(region: .unset)
        XCTAssertTrue(controller.regionIsUnset)

        controller.regionSelection = .unset
        let result = await controller.confirmRegion()
        XCTAssertFalse(result)
        XCTAssertTrue(client.sentRegionWriteLog.isEmpty, "setRegion(.unset) must never be called")
    }

    func testA02_AC7_regionUnsetIsFalseWhenRegionSimplyNotYetKnown() {
        let (controller, client) = makeController()
        client.nodeConfig = nil
        XCTAssertFalse(controller.regionIsUnset, "not-yet-known must not read the same as explicitly UNSET")
    }

    func testA02_AC7_confirmingAPickedRegionWritesItAndUnblocks() async {
        let (controller, client) = makeController()
        client.nodeConfig = NodeConfigSnapshot(region: .unset)
        controller.regionSelection = .us

        let result = await controller.confirmRegion()
        XCTAssertTrue(result)
        XCTAssertEqual(client.sentRegionWriteLog, [.us])
        // `StubMeshtasticClient.setRegion` records the write but does
        // NOT update `nodeConfig` (unlike a real client's read-back) —
        // the gate must still clear, not stay stuck reporting UNSET
        // forever because of that.
        XCTAssertFalse(controller.regionIsUnset)
    }

    // MARK: - A02_AC10 — rejoin / leave

    func testA02_AC10_rejoiningOwnCodeWritesNothing() async {
        let (controller, client) = makeController()
        client.nodeConfig = NodeConfigSnapshot(region: .us)
        _ = await controller.beginStart(humanName: "Camp Firefly")
        _ = await controller.confirmApply()
        let writesBeforeRejoin = client.sentChannelWriteLog.count
        guard let code = controller.profile.flatMap({ try? CrewCode.parse($0.code) }) else {
            return XCTFail("no profile after start")
        }

        let began = await controller.beginJoin(payload: .bareCode(code))
        XCTAssertFalse(began)
        XCTAssertNotNil(controller.rejoinOwnCrewMessage)
        XCTAssertEqual(client.sentChannelWriteLog.count, writesBeforeRejoin)
    }

    func testA02_AC10_leaveRestoresSnapshotWhenPresent() async {
        let snapshotStore = InMemoryCrewSnapshotStore()
        let (controller, client) = makeController(snapshotStore: snapshotStore)
        client.nodeConfig = NodeConfigSnapshot(region: .us)
        var stockPrimary = ChannelSettings()
        stockPrimary.name = "LongFast"
        stockPrimary.psk = Data([0x01])
        stockPrimary.moduleSettings.positionPrecision = 12
        client.channelTable = [{
            var channel = Channel()
            channel.index = 0
            channel.role = .primary
            channel.settings = stockPrimary
            return channel
        }()]

        _ = await controller.beginStart(humanName: "Camp Firefly")
        _ = await controller.confirmApply()
        XCTAssertTrue(controller.hasCrew)

        let left = await controller.leaveCrew()
        XCTAssertTrue(left)
        XCTAssertFalse(controller.hasCrew)
        guard let lastWrite = client.sentChannelWriteLog.last,
              let restored = lastWrite.channels.first(where: { $0.index == 0 }) else {
            return XCTFail("no restoring write found")
        }
        XCTAssertEqual(restored.settings.name, "LongFast")
        XCTAssertEqual(restored.settings.moduleSettings.positionPrecision, 12)
    }

    func testA02_AC10_leaveWritesStockDefaultWhenNoSnapshotExists() async {
        let (controller, client) = makeController()
        client.nodeConfig = NodeConfigSnapshot(region: .us)
        // No channelTable set at all -> snapshotCurrentPrimaryIfNeeded()
        // finds nothing to snapshot, so Leave must fall back to the
        // stock default primary (§3.4): empty name, precision 0.
        _ = await controller.beginStart(humanName: "Camp Firefly")
        _ = await controller.confirmApply()

        let left = await controller.leaveCrew()
        XCTAssertTrue(left)
        guard let lastWrite = client.sentChannelWriteLog.last,
              let restored = lastWrite.channels.first(where: { $0.index == 0 }) else {
            return XCTFail("no restoring write found")
        }
        XCTAssertEqual(restored.settings.name, "")
        XCTAssertEqual(restored.settings.psk, Data([0x01]))
        XCTAssertEqual(restored.settings.moduleSettings.positionPrecision, 0)
    }

    // MARK: - §4.5 — hidden set persistence

    func testHiddenSet_persistsAcrossControllerInstancesForTheSameCrewCode() async {
        let hiddenStore = InMemoryCrewHiddenStore()
        let (controller, client) = makeController(hiddenStore: hiddenStore)
        client.nodeConfig = NodeConfigSnapshot(region: .us)
        _ = await controller.beginStart(humanName: "Camp Firefly")
        _ = await controller.confirmApply()

        let pairing = CrewPairingController(crew: CrewStore(), store: InMemoryCrewPairingStore())
        pairing.pair(nodeID: 111)
        controller.hide(nodeID: 111, pairing: pairing)
        XCTAssertEqual(controller.hiddenIDs(), [111])

        // A second controller reading the SAME hidden store (e.g. after
        // a relaunch) sees the same hide, keyed by crew code — §4.5:
        // "stored per crew code... so leaving and rejoining a crew
        // restores the hides you had."
        let profileStore2 = InMemoryCrewProfileStore()
        profileStore2.save(controller.profile!)
        let controller2 = CrewController(client: client, profileStore: profileStore2,
                                          snapshotStore: InMemoryCrewSnapshotStore(), hiddenStore: hiddenStore)
        XCTAssertEqual(controller2.hiddenIDs(), [111])

        controller.unhide(nodeID: 111)
        XCTAssertTrue(controller.hiddenIDs().isEmpty)
    }
}
