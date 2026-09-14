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
        guard case .start(let code, "Camp Firefly", nil) = controller.pending else {
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

    // MARK: - A02 slice E — Advanced -> "Start a new crew" while already on one

    /// §6.5's "Start a new crew" reuses THIS function — `beginStart`,
    /// unchanged in shape — so there is no second minting path to keep
    /// in sync. The only new behaviour is the confirmation copy: the
    /// old crew's name appears, and it says the old crew stops seeing
    /// you (task scope item 2).
    func testAdvanced_startingANewCrewWhileAlreadyOnOneNamesTheOldCrewAndWarnsItStopsSeeing() async {
        let (controller, client) = makeController()
        client.nodeConfig = NodeConfigSnapshot(region: .us)
        _ = await controller.beginStart(humanName: "Camp Firefly")
        _ = await controller.confirmApply()
        XCTAssertEqual(controller.profile?.humanName, "Camp Firefly")

        let began = await controller.beginStart(humanName: "Night Shift")
        XCTAssertTrue(began)
        guard case .start(_, "Night Shift", "Camp Firefly") = controller.pending else {
            return XCTFail("expected a staged .start carrying the OLD crew's name")
        }
        let extra = controller.confirmationLines.joined(separator: " ")
        XCTAssertTrue(extra.contains("Camp Firefly"), "the OLD crew must be named: \(extra)")
        XCTAssertTrue(extra.localizedCaseInsensitiveContains("stops seeing you"),
                      "must say the old crew stops seeing you: \(extra)")

        // AC8 still holds for this new sentence: no jargon anywhere on
        // the main sheet.
        let jargon = ["node", "channel", "index", "precision", "preset", "region", "PSK", "Meshtastic"]
        for word in jargon {
            XCTAssertFalse(extra.localizedCaseInsensitiveContains(word), "\"\(word)\" leaked onto the main sheet")
        }

        let confirmed = await controller.confirmApply()
        XCTAssertTrue(confirmed)
        XCTAssertEqual(controller.profile?.humanName, "Night Shift", "the NEW crew is now active")
    }

    /// PR #313 review, task scope item 2's OTHER half: a switch must not
    /// overwrite the pre-crew snapshot. `testA02_AC6_snapshotIsTakenOnce
    /// NeverOverwritten` above proves the `load() == nil` guard, but it
    /// leaves the stub's channel table on the ORIGINAL primary the whole
    /// time — so it would still pass against a build where the only
    /// protection was "the second read happened to see LongFast again".
    /// Here the radio really is moved onto the first crew's channel
    /// before the second Start, which is what a real switch looks like:
    /// both independent guards (§2.1 step 4's "once only" AND "never
    /// capture a channel whose name parses as a crew code") have to hold
    /// for the snapshot to survive as the ORIGINAL pre-crew channel.
    func testAdvanced_startingANewCrewLeavesTheOriginalPreCrewSnapshotIntact() async {
        let snapshotStore = InMemoryCrewSnapshotStore()
        let (controller, client) = makeController(snapshotStore: snapshotStore)
        client.nodeConfig = NodeConfigSnapshot(region: .us)
        var stockPrimary = ChannelSettings()
        stockPrimary.name = "LongFast"
        stockPrimary.psk = Data([0x01])
        stockPrimary.moduleSettings.positionPrecision = 13
        client.channelTable = [{
            var channel = Channel()
            channel.index = 0
            channel.role = .primary
            channel.settings = stockPrimary
            return channel
        }()]

        _ = await controller.beginStart(humanName: "Camp Firefly")
        _ = await controller.confirmApply()
        XCTAssertEqual(snapshotStore.load()?.name, "LongFast")
        let firstCode = controller.profile?.code
        XCTAssertNotNil(firstCode)

        // The radio is now genuinely on the first crew's channel — the
        // state a real "Start a new crew" begins from.
        var nowCrew = ChannelSettings()
        nowCrew.name = firstCode ?? ""
        nowCrew.psk = CrewKey.psk(for: try! CrewCode.parse(firstCode ?? ""))
        nowCrew.moduleSettings.positionPrecision = 32
        client.channelTable = [{
            var channel = Channel()
            channel.index = 0
            channel.role = .primary
            channel.settings = nowCrew
            return channel
        }()]

        _ = await controller.beginStart(humanName: "Night Shift")
        _ = await controller.confirmApply()

        XCTAssertEqual(snapshotStore.load()?.name, "LongFast",
                       "the snapshot is the ORIGINAL pre-crew channel, never the crew this phone was just on")
        XCTAssertEqual(snapshotStore.load()?.positionPrecision, 13)

        // …so Leave still restores the phone to where it actually
        // started, not onto the first crew at precision 32.
        let left = await controller.leaveCrew()
        XCTAssertTrue(left)
        XCTAssertEqual(client.sentChannelWriteLog.last?.channels.first?.settings.name, "LongFast")
    }

    // MARK: - PR #313 — the profile-change callback (`AppGraph.sync
    // CrewMembershipWithProfile`, i.e. what points the membership engine
    // at the crew this phone is actually on)

    /// Every path that CHANGES which crew this phone is on must fire the
    /// callback — a Start, a switch, and a Leave. Before PR #313 nothing
    /// listened, and `CrewMembershipEngine` never learned there was a
    /// crew at all.
    func testProfileChangedFiresOnStartOnSwitchAndOnLeave() async {
        let (controller, client) = makeController()
        client.nodeConfig = NodeConfigSnapshot(region: .us)
        var codes: [String?] = []
        controller.onProfileChanged = { [weak controller] in codes.append(controller?.profile?.code) }

        _ = await controller.beginStart(humanName: "Camp Firefly")
        _ = await controller.confirmApply()
        XCTAssertEqual(codes.count, 1)
        XCTAssertEqual(codes.last, controller.profile?.code)

        _ = await controller.beginStart(humanName: "Night Shift")
        _ = await controller.confirmApply()
        XCTAssertEqual(codes.count, 2)
        XCTAssertEqual(codes.last, controller.profile?.code)
        XCTAssertNotEqual(codes[0], codes[1], "a switch is a different crew, not the same one twice")

        _ = await controller.leaveCrew()
        XCTAssertEqual(codes.count, 3)
        XCTAssertNil(codes[2], "Leave clears the crew, so the engine is cleared too")
    }

    /// A first-ever Start (no prior crew) must not gain the new
    /// sentence just because the switching machinery now exists.
    func testAdvanced_ordinaryFirstStartStillHasNoSwitchingSentence() async {
        let (controller, client) = makeController()
        client.nodeConfig = NodeConfigSnapshot(region: .us)
        let began = await controller.beginStart(humanName: "Camp Firefly")
        XCTAssertTrue(began)
        guard case .start(_, "Camp Firefly", nil) = controller.pending else {
            return XCTFail("expected a staged .start with no switchingFrom")
        }
        XCTAssertTrue(controller.confirmationLines.isEmpty)
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

    // MARK: - §1.8 amendment (2026-09-14, bench finding) — "Copy
    // Meshtastic link" carries the connected radio's OWN, CURRENT LoRa
    // config, and refuses to export one at all while region is UNSET.

    func testMeshtasticURLCarriesConnectedRadiosLoraConfig() async throws {
        let (controller, client) = makeController()
        client.nodeConfig = NodeConfigSnapshot(
            region: .us, modemPreset: .longFast, usePreset: true, hopLimit: 3, txEnabled: true)

        let began = await controller.beginStart(humanName: "Camp Firefly")
        XCTAssertTrue(began)
        guard case .start(let code, _, _) = controller.pending else {
            return XCTFail("expected a staged .start")
        }

        let url = controller.meshtasticURL(for: code)
        XCTAssertNotNil(url, "region+lora config are known, export must succeed")
        let expected = try CrewChannel.meshtasticURL(for: code, loraConfig: client.nodeConfig!.loraConfig!)
        XCTAssertEqual(url, expected)

        // The exported ChannelSet actually carries lora_config — the
        // bug this amendment fixes: `--seturl` and the official apps'
        // URL import REPLACE the target radio's lora_config wholesale,
        // so an absent one writes it deaf (bench-confirmed on a Heltec
        // V3, region UNSET / use_preset false).
        let parsed = try ChannelURL.parse(url!)
        XCTAssertTrue(parsed.channelSet.hasLoraConfig)
        XCTAssertEqual(parsed.channelSet.loraConfig.region, .us)
        XCTAssertTrue(parsed.channelSet.loraConfig.usePreset)
    }

    func testMeshtasticURLIsNilWhenRegionUnset() async {
        let (controller, client) = makeController()
        client.nodeConfig = NodeConfigSnapshot(region: .unset)

        let began = await controller.beginStart(humanName: "Camp Firefly")
        XCTAssertTrue(began)
        guard case .start(let code, _, _) = controller.pending else {
            return XCTFail("expected a staged .start")
        }

        XCTAssertNil(controller.meshtasticURL(for: code),
                      "exporting with an UNSET region would write the importing radio deaf")
    }

    func testMeshtasticURLIsNilWhenNodeConfigNotYetKnown() async {
        let (controller, client) = makeController()
        client.nodeConfig = nil

        let began = await controller.beginStart(humanName: "Camp Firefly")
        XCTAssertTrue(began)
        guard case .start(let code, _, _) = controller.pending else {
            return XCTFail("expected a staged .start")
        }

        XCTAssertNil(controller.meshtasticURL(for: code), "no lora_config reported yet — never guess one")
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

    // MARK: - Radio gate — the owner's build-328 report
    //
    // "Tried to join but nothing happened, still on the Join a crew
    // screen." (Jake, 2026-09-14). These four pin the fix from both
    // sides: nothing is attempted without a radio, and every way the
    // attempt CAN fail says so in words.

    /// The regression itself. `beginJoin` with no connected client must
    /// land in `.needsRadio`, and — the load-bearing half — must not
    /// have touched the radio at all on the way there. Asserting only
    /// the state would pass for an implementation that tried the write
    /// first and set the state afterwards.
    func testJoinWithNoRadioIsRefusedBeforeAnythingIsAttempted() async {
        let client = StubMeshtasticClient()   // connectedNodeNum stays nil
        let controller = CrewController(client: client)
        let code = try! CrewCode.parse("FIRE-4K9M7X")

        let began = await controller.beginJoin(payload: .bareCode(code))

        XCTAssertFalse(began)
        XCTAssertEqual(controller.phase, .needsRadio)
        XCTAssertFalse(controller.hasConnectedRadio)
        XCTAssertNil(controller.pending, "nothing may be staged for a sheet that cannot be applied")
        XCTAssertNil(controller.profile, "no crew was joined")
        XCTAssertTrue(client.sentChannelWriteLog.isEmpty, "no write may be attempted without a radio")
        XCTAssertNil(controller.importer.applyPlan, "not even a plan may be prepared")
    }

    /// The exact shipped symptom: what the Join screen PUT ON SCREEN.
    /// Build 328 showed the bare Swift enum case `notConnected` in a
    /// grey footnote — technically feedback, and unusable as any.
    func testJoinWithNoRadioNeverShowsARawEnumCase() async {
        let client = StubMeshtasticClient()
        let controller = CrewController(client: client)
        _ = await controller.beginJoin(payload: .bareCode(try! CrewCode.parse("FIRE-4K9M7X")))

        let shown = controller.failureMessage
        XCTAssertEqual(shown, CrewController.needRadioMessage)
        XCTAssertNotEqual(shown, String(describing: AdminWriteError.notConnected))
        for jargon in ["notConnected", "AdminWriteError", "node", "channel"] {
            XCTAssertFalse(shown!.localizedCaseInsensitiveContains(jargon), "\"\(jargon)\" on screen")
        }

        // …and the message the plan path itself would produce, which is
        // where the raw case actually leaked from
        // (`ChannelImportViewModel.planMessage(for:)`'s
        // `String(describing:)` default).
        let planned = ChannelImportViewModel.planMessage(for: AdminWriteError.notConnected)
        XCTAssertNotEqual(planned, String(describing: AdminWriteError.notConnected))
        XCTAssertFalse(planned.contains("notConnected"))
    }

    /// The other side of the gate: with a client, the same JOIN stages a
    /// plan and the CONFIRM writes index 0 with the crew's own code.
    func testJoinWithAConnectedRadioStagesAPlanAndWrites() async {
        let (controller, client) = makeController()
        client.nodeConfig = NodeConfigSnapshot(region: .us)
        let code = try! CrewCode.parse("FIRE-4K9M7X")

        XCTAssertTrue(controller.hasConnectedRadio)
        let staged = await controller.beginJoin(payload: .bareCode(code))
        XCTAssertTrue(staged)
        XCTAssertEqual(controller.phase, .checkingPuck)
        XCTAssertNotNil(controller.pending)

        let confirmed = await controller.confirmApply()
        XCTAssertTrue(confirmed)
        XCTAssertEqual(controller.phase, .joined)
        XCTAssertEqual(controller.progressLabel, "Joined")
        XCTAssertNil(controller.failureMessage)
        XCTAssertEqual(controller.profile?.code, code.canonical)
        XCTAssertEqual(client.sentChannelWriteLog.count, 1)
        XCTAssertEqual(client.sentChannelWriteLog[0].channels.first(where: { $0.index == 0 })?
            .settings.name, code.canonical)
    }

    /// A puck that drops between the plan and the commit — the window
    /// the pre-fix code had no words for at all. The write IS attempted
    /// here (the radio was connected when CONFIRM was tapped); it fails,
    /// and the failure has to be a sentence, not a case name, and the
    /// app must not claim the crew was joined.
    func testDisconnectMidWriteFailsHonestlyAndJoinsNothing() async {
        let (controller, client) = makeController()
        client.nodeConfig = NodeConfigSnapshot(region: .us)
        let staged = await controller.beginJoin(payload: .bareCode(try! CrewCode.parse("FIRE-4K9M7X")))
        XCTAssertTrue(staged)

        client.failNextChannelWrite(with: .notConnected)
        let applied = await controller.confirmApply()

        XCTAssertFalse(applied)
        XCTAssertNil(controller.profile, "a failed write must never adopt a crew")
        guard case .failed(let message) = controller.phase else {
            return XCTFail("expected .failed, got \(controller.phase)")
        }
        XCTAssertEqual(message, "Your puck isn't connected. Connect it, then try again.")
        XCTAssertEqual(controller.failureMessage, message)
        XCTAssertNil(controller.progressLabel, "a failure is not progress")
        // The staged plan survives, so CONFIRM is a genuine retry rather
        // than something the user has to re-scan for.
        XCTAssertNotNil(controller.pending)
    }

    /// Every other honest `AdminWriteError` reaches the screen as its
    /// own sentence — a NAK/partial apply, a timeout, a read-back
    /// mismatch. None of them is ever a Swift enum case.
    ///
    /// Review of PR #319: each expected sentence is spelled out here
    /// rather than checked for length. "Longer than 20 characters and
    /// not literally `String(describing:)`" is a proxy — a message of
    /// `timeout (AdminWriteError.timeout)` satisfies it and violates
    /// the property.
    func testEveryApplyFailureIsASentenceNotACaseName() async {
        let cases: [(AdminWriteError, String)] = [
            (.timeout,
             "Your puck didn't answer in time — it may still be restarting. Try again in a moment."),
            (.readBackMismatch("channel 0 (FIRE-4K9M7X)"),
             "Your puck didn't confirm the change (channel 0 (FIRE-4K9M7X)). Nothing is certain " +
             "until it does — try again."),
            (.partialApplyFailed(step: "channel 0", underlying: "writeFailed"),
             "Couldn't send channel 0: writeFailed. Your puck may be only partly set up — " +
             "reconnect and try again."),
            (.encodingFailed, "Couldn't prepare that change to send."),
        ]
        for (scripted, expected) in cases {
            let (controller, client) = makeController()
            client.nodeConfig = NodeConfigSnapshot(region: .us)
            _ = await controller.beginJoin(payload: .bareCode(try! CrewCode.parse("FIRE-4K9M7X")))
            client.failNextChannelWrite(with: scripted)
            let confirmed = await controller.confirmApply()
            XCTAssertFalse(confirmed)

            guard case .failed(let message) = controller.phase else {
                return XCTFail("expected .failed for \(scripted), got \(controller.phase)")
            }
            XCTAssertEqual(message, expected, "\(scripted)")
            XCTAssertEqual(controller.failureMessage, expected)
            XCTAssertNil(controller.progressLabel, "a failure is not progress")
            XCTAssertNil(controller.profile)
            // The plan survives, so the screen's TRY AGAIN is a genuine
            // retry of the same attempt.
            XCTAssertNotNil(controller.pending)
        }
    }

    /// Leave is a write too (§3.4) — refusing it without a radio is what
    /// keeps a "leave" from becoming a local forget while the puck keeps
    /// transmitting on the crew channel.
    func testLeaveWithNoRadioIsRefusedAndWritesNothing() async {
        let (controller, client) = makeController()
        client.nodeConfig = NodeConfigSnapshot(region: .us)
        _ = await controller.beginStart(humanName: "Camp Firefly")
        _ = await controller.confirmApply()
        let writesBefore = client.sentChannelWriteLog.count

        client.connectedNodeNum = nil
        let left = await controller.leaveCrew()

        XCTAssertFalse(left)
        XCTAssertTrue(controller.hasCrew, "the app must not pretend it left")
        XCTAssertEqual(client.sentChannelWriteLog.count, writesBefore)
        XCTAssertNotNil(controller.leaveErrorMessage)
    }

    // MARK: - Progress states, and the read-back this app checks itself

    func testProgressLabelsAreTheThreeStepsThatActuallyRun() async {
        let (controller, client) = makeController()
        client.nodeConfig = NodeConfigSnapshot(region: .us)
        XCTAssertNil(controller.progressLabel, ".idle says nothing")

        _ = await controller.beginJoin(payload: .bareCode(try! CrewCode.parse("FIRE-4K9M7X")))
        XCTAssertEqual(controller.progressLabel, "Checking your puck…")

        _ = await controller.confirmApply()
        XCTAssertEqual(controller.progressLabel, "Joined")
    }

    /// `confirmApply()`'s own verification of the radio's read-back,
    /// pinned as the pure function it is: the written primary's NAME is
    /// the crew code (§1.3), so a report that does not carry it is not a
    /// crew this app may claim the user joined.
    func testReadBackVerificationRequiresThePrimaryToCarryTheCode() {
        let code = try! CrewCode.parse("FIRE-4K9M7X")
        let other = try! CrewCode.parse("FIRE-9Z8Y7X")

        XCTAssertFalse(CrewController.report(nil, carries: code))
        XCTAssertFalse(CrewController.report(ChannelWriteReport(channels: [], loraConfig: nil), carries: code))

        func channel(named name: String, role: Channel.Role) -> Channel {
            var settings = ChannelSettings()
            settings.name = name
            var channel = Channel()
            channel.index = 0
            channel.role = role
            channel.settings = settings
            return channel
        }
        XCTAssertTrue(CrewController.report(
            ChannelWriteReport(channels: [channel(named: code.canonical, role: .primary)], loraConfig: nil),
            carries: code))
        XCTAssertFalse(CrewController.report(
            ChannelWriteReport(channels: [channel(named: other.canonical, role: .primary)], loraConfig: nil),
            carries: code),
            "somebody else's crew read back is not this crew")
        XCTAssertFalse(CrewController.report(
            ChannelWriteReport(channels: [channel(named: code.canonical, role: .secondary)], loraConfig: nil),
            carries: code),
            "the crew has to be the PRIMARY, not some spare slot")
    }

    /// Scanning your own code stays a friendly no-op even with no puck
    /// connected — it is true, and actionable, regardless, and sending
    /// someone off to connect a radio for a write that would never
    /// happen would be the worse answer.
    func testRejoiningYourOwnCrewIsStillANoOpWithNoRadio() async {
        let profileStore = InMemoryCrewProfileStore()
        profileStore.save(CrewProfile(code: "FIRE-4K9M7X", humanName: "Camp Firefly", createdAtMs: 1))
        let client = StubMeshtasticClient()
        let controller = CrewController(client: client, profileStore: profileStore)

        let began = await controller.beginJoin(payload: .bareCode(try! CrewCode.parse("FIRE-4K9M7X")))

        XCTAssertFalse(began)
        XCTAssertEqual(controller.rejoinOwnCrewMessage, "You're already in Camp Firefly.")
        XCTAssertNotEqual(controller.phase, .needsRadio)
    }

    func testClearFailureResetsARefusalButNeverAJoin() async {
        let (controller, client) = makeController()
        client.nodeConfig = NodeConfigSnapshot(region: .us)
        client.connectedNodeNum = nil
        _ = await controller.beginJoin(payload: .bareCode(try! CrewCode.parse("FIRE-4K9M7X")))
        XCTAssertEqual(controller.phase, .needsRadio)

        controller.clearFailure()
        XCTAssertEqual(controller.phase, .idle)
        XCTAssertNil(controller.failureMessage)

        client.connectedNodeNum = 48_621_524
        _ = await controller.beginJoin(payload: .bareCode(try! CrewCode.parse("FIRE-4K9M7X")))
        _ = await controller.confirmApply()
        XCTAssertEqual(controller.phase, .joined)
        controller.clearFailure()
        XCTAssertEqual(controller.phase, .joined, "a finished join is not a failure to clear")
    }
}
