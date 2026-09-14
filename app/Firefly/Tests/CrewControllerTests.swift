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

    /// PR #308 review. "Once only" is not the whole of §2.1 step 4 —
    /// the snapshot must also never CAPTURE a Firefly crew channel.
    /// Reachable state: reinstall (or clear app data) on a phone whose
    /// radio this app already moved onto a crew. The snapshot store is
    /// empty, the radio's primary is `FIRE-XXXXXX`, and the pre-fix code
    /// latched that as "what came before" — so Leave would have
    /// "restored" the user onto a crew channel still transmitting
    /// precise positions, the exact outcome §3.4 calls the worst
    /// possible one.
    func testA02_AC6_snapshotNeverCapturesAFireflyCrewChannel() async {
        let snapshotStore = InMemoryCrewSnapshotStore()
        let (controller, client) = makeController(snapshotStore: snapshotStore)
        client.nodeConfig = NodeConfigSnapshot(region: .us)
        // The radio is ALREADY on a crew and this phone has no record.
        var existingCrew = ChannelSettings()
        existingCrew.name = "FIRE-4K9M7X"
        existingCrew.psk = CrewKey.psk(for: try! CrewCode.parse("FIRE-4K9M7X"))
        existingCrew.moduleSettings.positionPrecision = 32
        client.channelTable = [{
            var channel = Channel()
            channel.index = 0
            channel.role = .primary
            channel.settings = existingCrew
            return channel
        }()]

        _ = await controller.beginStart(humanName: "Camp Firefly")
        _ = await controller.confirmApply()
        XCTAssertNil(snapshotStore.load(),
                     "a crew channel must never be captured as the pre-crew primary")

        // …and Leave therefore falls back to the stock default, at
        // precision 0 — not back onto somebody's crew at precision 32.
        let left = await controller.leaveCrew()
        XCTAssertTrue(left)
        guard let restored = client.sentChannelWriteLog.last?.channels.first(where: { $0.index == 0 }) else {
            return XCTFail("no restoring write found")
        }
        XCTAssertEqual(restored.settings.name, "")
        XCTAssertEqual(restored.settings.moduleSettings.positionPrecision, 0)
        XCTAssertNil(try? CrewCode.parse(restored.settings.name))
    }

    // MARK: - A02_AC8 — the confirmation sheet says one plain thing, once

    func testA02_AC8_confirmationCopyCarriesNoJargonAndNeverRepeatsItself() async {
        let (controller, client) = makeController()
        client.nodeConfig = NodeConfigSnapshot(region: .us)
        _ = await controller.beginStart(humanName: "Camp Firefly")

        let jargon = ["node", "channel", "index", "precision", "preset", "region", "PSK", "Meshtastic"]
        let shown = ([controller.confirmationTitle, controller.confirmationPrimaryText]
            + controller.confirmationLines).joined(separator: " ")
        for word in jargon {
            XCTAssertFalse(shown.localizedCaseInsensitiveContains(word), "AC8: \"\(word)\" on the main sheet")
        }
        // The "blink off" sentence is the shared one every other admin
        // write on this sheet uses, and it appears exactly once.
        XCTAssertTrue(controller.confirmationPrimaryText.hasSuffix(AdminWriteCopy.radioBlinksOff))
        XCTAssertEqual(shown.components(separatedBy: AdminWriteCopy.radioBlinksOff).count - 1, 1)
        // A first-ever Start has no second sentence to add.
        XCTAssertTrue(controller.confirmationLines.isEmpty)
        // Technical details still exist, and DO carry the jargon.
        XCTAssertFalse(controller.confirmationTechnicalDetails.isEmpty)
    }

    func testA02_AC8_changingCrewsSaysWhichCrewYouAreLeaving() async {
        let (controller, client) = makeController()
        client.nodeConfig = NodeConfigSnapshot(region: .us)
        _ = await controller.beginStart(humanName: "Camp Firefly")
        _ = await controller.confirmApply()

        let other = try! CrewCode.parse("FIRE-ZZZZZZ")
        let began = await controller.beginJoin(payload: .crewLink(CrewLink(code: other, name: "Night Shift")))
        XCTAssertTrue(began)
        XCTAssertEqual(controller.confirmationTitle, "Join a new crew?")
        let extra = controller.confirmationLines.joined(separator: " ")
        XCTAssertTrue(extra.contains("Camp Firefly"), "the crew being LEFT must be named: \(extra)")
        XCTAssertTrue(extra.contains("Night Shift"), "the crew being JOINED must be named: \(extra)")
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
