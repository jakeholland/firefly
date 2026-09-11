//
//  ConnectSettingsViewModelTests.swift — state-transition tests for the
//  view models that live in the app target (docs/specs/
//  A01-companion-app.md, Slice C "Must add": "Add view-model unit
//  tests"). Run via `xcodebuild test -only-testing:FireflyAppTests`
//  (see project.yml) — these types are app-target Swift, not part of
//  the FireflyKit SwiftPM package `swift test` covers.
//
import FireflyMesh
import FireflyModel
import MeshtasticProto
import XCTest

/// Polls `condition` until it is true, then returns immediately — never
/// a fixed sleep, and never a fixed-iteration-count loop whose total
/// budget can run out before a state change actually lands. A CI run
/// under load (GitHub's macOS runner, run 34653207182) is the whole
/// reason this exists: `DiagnosticsViewModelTests`'
/// `testObserveTracksLinkStateIndependentlyOfAnotherSubscriber` used to
/// poll for `0..<200` iterations of a 5ms sleep — a 1s ceiling — and
/// still saw `.handshaking` after 3.4s under a loaded scheduler, so the
/// assert right after the loop read whatever the state happened to be
/// at timeout rather than what it eventually became. `timeout` here is
/// a generous, failure-only ceiling: it only matters when `condition`
/// never becomes true at all (a real bug), never as a stand-in for
/// "probably done by now" — a passing run still returns the moment the
/// real subscriber delivers the state, in whatever time that actually
/// takes.
@MainActor
private func eventually(_ description: String = "condition", timeout: TimeInterval = 15,
                         file: StaticString = #filePath, line: UInt = #line,
                         _ condition: () -> Bool) async {
    let deadline = Date().addingTimeInterval(timeout)
    while !condition() {
        if Date() >= deadline {
            XCTFail("timed out after \(timeout)s waiting for \(description)", file: file, line: line)
            return
        }
        try? await Task.sleep(nanoseconds: 5_000_000)
    }
}

// MARK: - NearbyNodesViewModel

@MainActor
final class NearbyNodesViewModelTests: XCTestCase {

    private func snapshot(num: UInt32, shortName: String? = nil, rssi: Int16? = -60,
                           lastHeard: Date? = nil) -> MeshNodeSnapshot {
        MeshNodeSnapshot(num: num, shortName: shortName, longName: nil, position: nil,
                          lastHeard: lastHeard, rssiDbm: rssi, snrDb: nil, hopsAway: nil)
    }

    private func makeController() -> CrewPairingController {
        CrewPairingController(crew: CrewStore(), store: InMemoryCrewPairingStore())
    }

    private func makeVM(_ pairing: CrewPairingController? = nil) -> NearbyNodesViewModel {
        NearbyNodesViewModel(client: StubMeshtasticClient(), pairing: pairing ?? makeController())
    }

    func testStartsEmpty() {
        XCTAssertTrue(makeVM().nodes.isEmpty)
    }

    /// Finding 1 (first real-radio session): a want_config nodeDB replay
    /// carries `last_heard` for every node but never a direct RSSI, so a
    /// node with no RSSI must still appear — excluding it is exactly why
    /// "NEARBY" stayed on its empty state after a completed handshake
    /// against a real, busy mesh. Its tier reads NONE — honestly absent,
    /// never invented from a reading that was never made.
    func testNodesWithoutRSSIStillAppearWithNoSignalTier() {
        let vm = makeVM()
        vm.apply(snapshot(num: 1, shortName: "REPLAY", rssi: nil, lastHeard: Date()))
        XCTAssertEqual(vm.nodes.count, 1)
        XCTAssertEqual(vm.nodes[0].tier, .none)
        XCTAssertEqual(vm.nodes[0].heardAgo, "HEARD JUST NOW")
    }

    /// A node the client has never reported a `lastHeard` for at all
    /// (not merely absent-RSSI — genuinely never heard) reads NEVER,
    /// never a fabricated "just now".
    func testNodeWithNoLastHeardAtAllReadsNever() {
        let vm = makeVM()
        vm.apply(snapshot(num: 1, rssi: nil, lastHeard: nil))
        XCTAssertEqual(vm.nodes[0].heardAgo, "NEVER HEARD")
    }

    func testAppliedSnapshotAppearsWithItsTier() {
        let vm = makeVM()
        vm.apply(snapshot(num: 1, shortName: "AB1", rssi: -55)) // strong, per SignalTierTests
        XCTAssertEqual(vm.nodes.count, 1)
        XCTAssertEqual(vm.nodes[0].displayName, "AB1")
        XCTAssertEqual(vm.nodes[0].tier, .strong)
        XCTAssertFalse(vm.nodes[0].isCrew)
        XCTAssertNil(vm.nodes[0].colorIndex, "a stranger has no crew colour to render")
    }

    /// A node named only by number renders Meshtastic's own `!%08x`
    /// convention, never a blank row.
    func testUnnamedNodeFallsBackToHexID() {
        let vm = makeVM()
        vm.apply(snapshot(num: 0xAB, shortName: nil, rssi: -55))
        XCTAssertEqual(vm.nodes[0].displayName, "!000000ab")
    }

    /// Finding 1: strangers rank by last-heard recency, not signal tier —
    /// most of a want_config replay carries no RSSI at all (tier NONE
    /// for all of them), so ranking by tier would leave the list in
    /// arbitrary nodeDB order. Deliberately uses THREE identical, strong
    /// RSSI readings — if this were still tier-ranked the order would be
    /// unspecified; recency ranking is the only thing that pins it.
    func testStrangersRankByLastHeardRecencyNotSignalTier() {
        let vm = makeVM()
        let now = Date()
        vm.apply(snapshot(num: 1, shortName: "OLDEST", rssi: -55, lastHeard: now.addingTimeInterval(-300)))
        vm.apply(snapshot(num: 2, shortName: "NEWEST", rssi: -55, lastHeard: now))
        vm.apply(snapshot(num: 3, shortName: "MIDDLE", rssi: -55, lastHeard: now.addingTimeInterval(-60)))
        XCTAssertEqual(vm.nodes.map(\.displayName), ["NEWEST", "MIDDLE", "OLDEST"])
    }

    /// A node that has never been heard at all sorts last, behind every
    /// node with a real timestamp — never treated as "just heard".
    func testNeverHeardStrangerSortsLast() {
        let vm = makeVM()
        vm.apply(snapshot(num: 1, shortName: "NEVER", rssi: -55, lastHeard: nil))
        vm.apply(snapshot(num: 2, shortName: "OLD", rssi: -55, lastHeard: Date().addingTimeInterval(-3600)))
        XCTAssertEqual(vm.nodes.map(\.displayName), ["OLD", "NEVER"])
    }

    /// The puck's own roster policy (`ff_heard.h`; core issue #268):
    /// paired stays pinned, unpaired is a bounded, LRU-evictable list —
    /// applied here to this screen's own app-side heard dictionary
    /// (issue #273's "keep the heard list app-side" intent) so a busy
    /// public mesh's ~200 nodeDB entries cannot grow it without limit.
    func testUnpairedStrangersAreBoundedWithLRUEviction() {
        let vm = makeVM()
        let now = Date()
        // One more than the bound — the oldest must be evicted, not the
        // newest.
        for i in 0..<65 {
            vm.apply(snapshot(num: UInt32(i), shortName: "N\(i)", rssi: -55,
                               lastHeard: now.addingTimeInterval(-Double(65 - i))))
        }
        XCTAssertEqual(vm.nodes.count, 64, "the bound caps unpaired tracking, never grows without limit")
        XCTAssertFalse(vm.nodes.contains { $0.id == 0 }, "the least-recently-heard entry is the one evicted")
        XCTAssertTrue(vm.nodes.contains { $0.id == 64 }, "the most-recently-heard entry survives")
    }

    /// A paired member is NEVER evicted by the unpaired bound, no matter
    /// how many strangers arrive after it — "paired pinned" (#268).
    func testPairedMembersAreNeverEvictedByTheUnpairedBound() {
        let vm = makeVM()
        vm.apply(snapshot(num: 999, shortName: "CREW", rssi: -55, lastHeard: Date().addingTimeInterval(-99_999)))
        vm.addToCrew(999)
        for i in 0..<80 {
            vm.apply(snapshot(num: UInt32(i), shortName: "N\(i)", rssi: -55, lastHeard: Date()))
        }
        XCTAssertTrue(vm.nodes.contains { $0.id == 999 && $0.isCrew }, "paired members are pinned, not LRU-evicted")
    }

    /// A later snapshot for the same node updates it in place rather
    /// than appending a duplicate row.
    func testReapplyingTheSameNodeUpdatesRatherThanDuplicates() {
        let vm = makeVM()
        vm.apply(snapshot(num: 1, shortName: "A", rssi: -96))
        vm.apply(snapshot(num: 1, shortName: "A", rssi: -55))
        XCTAssertEqual(vm.nodes.count, 1)
        XCTAssertEqual(vm.nodes[0].tier, .strong)
    }

    /// M2: Add/Remove go through the real `CrewPairingController` — a
    /// paired row gets a real colour and sorts ahead of strangers.
    func testAddToCrewPairsAndAssignsAColourAndSortsAhead() {
        let vm = makeVM()
        vm.apply(snapshot(num: 1, shortName: "A", rssi: -55))
        vm.apply(snapshot(num: 2, shortName: "B", rssi: -20)) // stronger signal, still a stranger

        vm.addToCrew(1)

        XCTAssertTrue(vm.nodes.first { $0.id == 1 }!.isCrew)
        XCTAssertNotNil(vm.nodes.first { $0.id == 1 }!.colorIndex)
        XCTAssertEqual(vm.nodes.map(\.id), [1, 2], "paired members sort ahead of strangers regardless of signal")
    }

    func testRemoveFromCrewUnpairsAndDropsItsColour() {
        let vm = makeVM()
        vm.apply(snapshot(num: 1, shortName: "A", rssi: -55))
        vm.addToCrew(1)
        vm.removeFromCrew(1)
        XCTAssertFalse(vm.nodes.first { $0.id == 1 }!.isCrew)
        XCTAssertNil(vm.nodes.first { $0.id == 1 }!.colorIndex)
    }

    /// M2's honest 8-limit message — the 9th distinct pairing is
    /// refused with a visible explanation, never a silently-ignored tap.
    func testAddingANinthMemberSetsTheHonestLimitMessage() {
        let pairing = makeController()
        let vm = makeVM(pairing)
        for nodeID in UInt32(1)...8 {
            vm.apply(snapshot(num: nodeID, shortName: "N\(nodeID)", rssi: -55))
            vm.addToCrew(nodeID)
        }
        XCTAssertNil(vm.limitMessage, "no message while the roster still has room")

        vm.apply(snapshot(num: 9, shortName: "NINE", rssi: -55))
        vm.addToCrew(9)

        XCTAssertNotNil(vm.limitMessage)
        XCTAssertFalse(vm.nodes.first { $0.id == 9 }!.isCrew, "the 9th add must not have silently succeeded")
    }

    /// M2's replacement for the OLD session-only behaviour: pairing now
    /// persists through the controller's own store, so a FRESH view
    /// model over the SAME controller sees what the first one paired.
    func testCrewMembershipSurvivesAFreshViewModelOverTheSamePairingController() {
        let pairing = makeController()
        let first = makeVM(pairing)
        first.apply(snapshot(num: 1, shortName: "A", rssi: -55))
        first.addToCrew(1)

        let second = makeVM(pairing)
        second.apply(snapshot(num: 1, shortName: "A", rssi: -55))
        XCTAssertTrue(second.nodes[0].isCrew, "the SAME controller's store, not a fresh in-memory one")
    }

    func testObserveDeliversNodeUpdatesFromTheClient() async {
        let client = StubMeshtasticClient()
        let vm = NearbyNodesViewModel(client: client, pairing: makeController())
        vm.observe()
        vm.observe() // idempotent, matching ConnectViewModel.observe()

        // StubMeshtasticClient never emits a node on its own (it is not
        // a fake radio) — this proves the SUBSCRIPTION plumbing, not
        // fabricated traffic, by publishing through the same EventHub
        // a real client would use. There is no public "inject a node"
        // hook on the protocol yet, so this test only asserts observe()
        // does not crash and stays idempotent; the ranking/apply logic
        // above is covered directly via `apply(_:)`.
        vm.stopObserving()
        vm.stopObserving() // idempotent
        XCTAssertTrue(vm.nodes.isEmpty)
    }
}

// MARK: - ChannelImportViewModel

@MainActor
final class ChannelImportViewModelTests: XCTestCase {

    func testStartsWithNoResultAndNoError() {
        let vm = ChannelImportViewModel()
        XCTAssertNil(vm.result)
        XCTAssertNil(vm.errorMessage)
    }

    func testImportingAMalformedLinkSetsAnErrorAndClearsAnyPriorResult() {
        let vm = ChannelImportViewModel()
        vm.importURL("not a channel link")
        XCTAssertNil(vm.result)
        XCTAssertEqual(vm.errorMessage, "Not a Meshtastic channel link.")
    }

    func testClearResetsBothResultAndError() {
        let vm = ChannelImportViewModel()
        vm.importURL("not a channel link")
        XCTAssertNotNil(vm.errorMessage)
        vm.clear()
        XCTAssertNil(vm.result)
        XCTAssertNil(vm.errorMessage)
    }

    func testPrecisionWarningsAreEmptyWithNoResult() {
        let vm = ChannelImportViewModel()
        XCTAssertTrue(vm.precisionWarnings.isEmpty)
    }

    func testASuccessfulImportClearsAnyPriorError() throws {
        let vm = ChannelImportViewModel()
        vm.importURL("garbage")
        XCTAssertNotNil(vm.errorMessage)

        var settings = ChannelSettings()
        settings.name = "Firefly"
        settings.moduleSettings.positionPrecision = 16
        let set = ChannelSet(settings: [settings])
        let url = ChannelURL.encode(set)

        vm.importURL(url)
        XCTAssertNil(vm.errorMessage)
        XCTAssertEqual(vm.result?.channelSet.settings.first?.name, "Firefly")
        XCTAssertTrue(vm.precisionWarnings.isEmpty, "an explicit precision must not warn")
    }

    /// `ChannelURL.encode` deliberately always fixes this gap (see its
    /// own doc comment), so building the fixture through it would
    /// prove nothing — this constructs the base64url payload directly,
    /// the same way an external, less careful sharer's link would
    /// arrive, to prove `parse` reports the gap rather than hiding it.
    func testPrecisionWarningNamesTheChannel() throws {
        let vm = ChannelImportViewModel()
        var settings = ChannelSettings()
        settings.name = "NoLimit"
        let set = ChannelSet(settings: [settings]) // no moduleSettings at all
        let data = try set.serializedData()
        let payload = data.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .trimmingCharacters(in: CharacterSet(charactersIn: "="))

        vm.importURL("https://meshtastic.org/e/#\(payload)")
        XCTAssertEqual(vm.precisionWarnings.count, 1)
        XCTAssertTrue(vm.precisionWarnings[0].contains("NoLimit"))
    }

    // MARK: - M3 (PR #274 review, BLOCKING 1 & 2): preparePlan() + applySummary

    /// BLOCKING 1: an "add" import must land in a free SECONDARY slot,
    /// never index 0/primary — proven here against a client whose
    /// scripted `currentChannelTable()` reports index 0 (primary) and 1
    /// (secondary) already occupied, so the free slot must be 2.
    func testPreparePlanAddModePlacesInTheLowestFreeSecondarySlot() async throws {
        let client = RecordingAdminWriteClient()
        var occupied0 = Channel(); occupied0.index = 0; occupied0.role = .primary
        var occupied1 = Channel(); occupied1.index = 1; occupied1.role = .secondary
        client.channelTable = [occupied0, occupied1]
        let vm = ChannelImportViewModel(client: client)

        var settings = ChannelSettings()
        settings.name = "Ops"
        vm.importURL(ChannelURL.encode(ChannelSet(settings: [settings]), addMode: true))

        let ok = await vm.preparePlan()
        XCTAssertTrue(ok)
        let plan = try XCTUnwrap(vm.applyPlan)
        XCTAssertTrue(plan.addMode)
        XCTAssertEqual(plan.request.channels.count, 1)
        XCTAssertEqual(plan.request.channels[0].index, 2)
        XCTAssertEqual(plan.request.channels[0].role, .secondary)
        XCTAssertNil(plan.request.loraConfig, "an add import must never carry a LoRa config")
        XCTAssertEqual(plan.disabledIndexes, [], "add mode never disables anything")
        XCTAssertFalse(plan.untouchedIndexes.contains(2), "the slot just written is not untouched")
        XCTAssertTrue(plan.untouchedIndexes.contains(0), "the primary must be reported untouched, not written")
    }

    /// BLOCKING 1: no free slot at all — must error rather than ever
    /// touching index 0 or an existing PSK.
    func testPreparePlanAddModeWithNoFreeSlotsSetsAnHonestPlanError() async throws {
        let client = RecordingAdminWriteClient()
        client.channelTable = (0..<8).map { i -> Channel in
            var c = Channel(); c.index = Int32(i); c.role = i == 0 ? .primary : .secondary
            return c
        }
        let vm = ChannelImportViewModel(client: client)
        vm.importURL(ChannelURL.encode(ChannelSet(settings: [ChannelSettings()]), addMode: true))

        let ok = await vm.preparePlan()
        XCTAssertFalse(ok)
        XCTAssertNil(vm.applyPlan)
        XCTAssertNotNil(vm.planErrorMessage)
        XCTAssertTrue(client.channelWrites.isEmpty, "a plan that fails to build must never reach a write")
    }

    /// BLOCKING 2: a "replace" import must disable every slot it does
    /// not fill, up to the node's max channel count — and disclose that
    /// in the plan, not just do it silently.
    func testPreparePlanReplaceModeDisablesEveryUnfilledSlot() async throws {
        let client = RecordingAdminWriteClient()
        let vm = ChannelImportViewModel(client: client)

        var primary = ChannelSettings()
        primary.name = "Firefly"
        var lora = Config.LoRaConfig()
        lora.region = .us
        vm.importURL(ChannelURL.encode(ChannelSet(settings: [primary], loraConfig: lora), addMode: false))

        let ok = await vm.preparePlan()
        XCTAssertTrue(ok)
        let plan = try XCTUnwrap(vm.applyPlan)
        XCTAssertFalse(plan.addMode)
        // 1 written (index 0) + 7 disabled (index 1...7) = 8 channel entries on the wire.
        XCTAssertEqual(plan.request.channels.count, 8)
        XCTAssertEqual(plan.disabledIndexes, Array(Int32(1)..<8))
        XCTAssertEqual(plan.untouchedIndexes, [], "replace accounts for every slot — nothing is untouched")
        XCTAssertNotNil(plan.request.loraConfig)
        let disabledEntries = plan.request.channels.filter { $0.role == .disabled }
        XCTAssertEqual(disabledEntries.count, 7)
        XCTAssertTrue(disabledEntries.allSatisfy { !$0.hasSettings || $0.settings == ChannelSettings() })
    }

    /// SHOULD-FIX 4: absent `moduleSettings` must write the SAFE default
    /// (0), never 32, and the summary must say so honestly.
    func testPreparePlanDefaultsMissingPrecisionToTheSafeValueNotFull() async throws {
        let client = RecordingAdminWriteClient()
        let vm = ChannelImportViewModel(client: client)

        // `ChannelURL.encode` deliberately always fixes the missing-
        // moduleSettings gap (its own doc comment), so building the
        // fixture through it would prove nothing here — this constructs
        // the base64url payload directly, the same way `ChannelURLTests
        // .testPrecisionWarningNamesTheChannel` does, so the import
        // genuinely carries no moduleSettings at all.
        var settings = ChannelSettings()
        settings.name = "NoLimit" // no moduleSettings at all
        var lora = Config.LoRaConfig()
        lora.region = .us
        let set = ChannelSet(settings: [settings], loraConfig: lora)
        let data = try set.serializedData()
        let payload = data.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .trimmingCharacters(in: CharacterSet(charactersIn: "="))
        vm.importURL("https://meshtastic.org/e/#\(payload)")

        _ = await vm.preparePlan()
        let plan = try XCTUnwrap(vm.applyPlan)
        XCTAssertEqual(plan.request.channels[0].settings.moduleSettings.positionPrecision, 0)
        XCTAssertFalse(plan.writtenChannels[0].precisionWasExplicit)

        let summary = try XCTUnwrap(vm.applySummary)
        XCTAssertTrue(summary.channelLines[0].contains("SAFE default"), "expected an honest disclosure, got: \(summary.channelLines[0])")
        XCTAssertFalse(summary.channelLines[0].contains("32"), "must never mention writing full precision by default")
    }

    /// An explicit precision in the imported link must be used verbatim.
    func testPreparePlanUsesTheImportedPrecisionVerbatimWhenPresent() async throws {
        let client = RecordingAdminWriteClient()
        let vm = ChannelImportViewModel(client: client)

        var settings = ChannelSettings()
        settings.name = "Firefly"
        settings.moduleSettings.positionPrecision = 16
        var lora = Config.LoRaConfig()
        lora.region = .us
        vm.importURL(ChannelURL.encode(ChannelSet(settings: [settings], loraConfig: lora)))

        _ = await vm.preparePlan()
        let plan = try XCTUnwrap(vm.applyPlan)
        XCTAssertEqual(plan.request.channels[0].settings.moduleSettings.positionPrecision, 16)
        XCTAssertTrue(plan.writtenChannels[0].precisionWasExplicit)
    }

    /// `preparePlan()` must surface a failed occupancy read honestly
    /// rather than guessing an empty table.
    func testPreparePlanSurfacesAFailedOccupancyReadHonestly() async {
        let client = RecordingAdminWriteClient()
        client.channelTableError = AdminWriteError.timeout
        let vm = ChannelImportViewModel(client: client)
        vm.importURL(ChannelURL.encode(ChannelSet(settings: [ChannelSettings()]), addMode: true))

        let ok = await vm.preparePlan()
        XCTAssertFalse(ok)
        XCTAssertNil(vm.applyPlan)
        XCTAssertNotNil(vm.planErrorMessage)
    }
}

// MARK: - SettingsViewModel

@MainActor
final class SettingsViewModelTests: XCTestCase {
    private var suiteName: String!
    private var defaults: UserDefaults!

    override func setUp() {
        super.setUp()
        suiteName = "SettingsViewModelTests.\(UUID().uuidString)"
        defaults = UserDefaults(suiteName: suiteName)
    }

    override func tearDown() {
        defaults.removePersistentDomain(forName: suiteName)
        defaults = nil
        suiteName = nil
        super.tearDown()
    }

    func testInitReadsExistingStoreValues() {
        let store = SettingsStore(defaults: defaults)
        store.setBool(true, .locationSharingEnabled)
        store.colorblindPaletteEnabled = true

        let vm = SettingsViewModel(store: store, channelImport: ChannelImportViewModel())
        XCTAssertTrue(vm.shareGPSWithNode)
        XCTAssertTrue(vm.colorblindPalette)
    }

    /// M2: a fresh store (nothing ever written) must read `.system`,
    /// never a guessed metric/imperial default — the exact bug PR #265's
    /// review flagged against the old bool key.
    func testInitDefaultsUnitsPreferenceToSystem() {
        let vm = SettingsViewModel(store: SettingsStore(defaults: defaults), channelImport: ChannelImportViewModel())
        XCTAssertEqual(vm.unitsPreference, .system)
    }

    func testSettersPersistThroughToAFreshViewModelOverTheSameStore() {
        let store = SettingsStore(defaults: defaults)
        let vm = SettingsViewModel(store: store, channelImport: ChannelImportViewModel())

        vm.setNodeLongName("Firefly One")
        vm.setNodeShortName("FF1")
        vm.setShareGPSWithNode(true)
        vm.setLocationIntervalSeconds(45)
        vm.setStayConnectedInBackground(true)
        vm.setColorblindPalette(true)
        vm.setUnitsPreference(.imperial)

        let reopened = SettingsViewModel(store: SettingsStore(defaults: defaults), channelImport: ChannelImportViewModel())
        XCTAssertEqual(reopened.nodeLongName, "Firefly One")
        XCTAssertEqual(reopened.nodeShortName, "FF1")
        XCTAssertTrue(reopened.shareGPSWithNode)
        XCTAssertEqual(reopened.locationIntervalSeconds, 45)
        XCTAssertTrue(reopened.stayConnectedInBackground)
        XCTAssertTrue(reopened.colorblindPalette)
        XCTAssertEqual(reopened.unitsPreference, .imperial)
    }

    /// The spec's floor: "default 30 s, floor 5 s" for the GPS-push
    /// cadence.
    func testLocationIntervalIsFlooredAtFiveSeconds() {
        let vm = SettingsViewModel(store: SettingsStore(defaults: defaults), channelImport: ChannelImportViewModel())
        vm.setLocationIntervalSeconds(1)
        XCTAssertEqual(vm.locationIntervalSeconds, 5)
    }

    /// UNKNOWN only until want_config has actually reported a region —
    /// never a permanent placeholder (finding 2, first real-radio
    /// session).
    func testRegionIsUnknownUntilTheClientReportsOne() {
        let vm = SettingsViewModel(store: SettingsStore(defaults: defaults), channelImport: ChannelImportViewModel())
        XCTAssertEqual(vm.region, "UNKNOWN")
    }

    /// Finding 2: the passive read seam — a region the CLIENT reports
    /// (want_config, or an admin write's own read-back) shows up here
    /// with no separate action, and no fabricated placeholder text.
    func testRegionReadsFromTheClientsNodeConfig() {
        let client = StubMeshtasticClient()
        client.connectedNodeNum = 1
        client.nodeConfig = NodeConfigSnapshot(region: .us)
        let vm = SettingsViewModel(store: SettingsStore(defaults: defaults), channelImport: ChannelImportViewModel(),
                                    client: client)
        XCTAssertEqual(vm.region, "US")
        XCTAssertEqual(vm.nodeConfigSourceLabel, "from node")
    }

    func testChannelNameIsUnknownUntilSomethingIsImportedOrReportedByTheClient() {
        let vm = SettingsViewModel(store: SettingsStore(defaults: defaults), channelImport: ChannelImportViewModel())
        XCTAssertEqual(vm.currentChannelName, "UNKNOWN")
        XCTAssertNil(vm.nodeConfigSourceLabel, "no source claimed until the client has actually reported something")
    }

    /// The whole point of sharing one `ChannelImportViewModel` instance
    /// between Connect and Settings: an import made through it is
    /// visible here too, without SettingsViewModel inventing its own
    /// copy — still the fallback for a node this client has never
    /// actually been connected to.
    func testChannelNameReflectsASharedChannelImportViewModel() {
        let channelImport = ChannelImportViewModel()
        let vm = SettingsViewModel(store: SettingsStore(defaults: defaults), channelImport: channelImport)

        var settings = ChannelSettings()
        settings.name = "Crew"
        settings.moduleSettings.positionPrecision = 32
        let url = ChannelURL.encode(ChannelSet(settings: [settings]))
        channelImport.importURL(url)

        XCTAssertEqual(vm.currentChannelName, "Crew")
    }

    /// Finding 2: the client's own PRIMARY channel name wins over a
    /// same-session import — it is what the node actually has, not what
    /// the phone merely staged.
    func testChannelNameReadsFromTheClientsNodeConfigOverAnImport() {
        let channelImport = ChannelImportViewModel()
        var settings = ChannelSettings()
        settings.name = "Staged"
        settings.moduleSettings.positionPrecision = 32
        channelImport.importURL(ChannelURL.encode(ChannelSet(settings: [settings])))

        let client = StubMeshtasticClient()
        client.connectedNodeNum = 1
        client.nodeConfig = NodeConfigSnapshot(primaryChannelName: "LongFast")
        let vm = SettingsViewModel(store: SettingsStore(defaults: defaults), channelImport: channelImport, client: client)

        XCTAssertEqual(vm.currentChannelName, "LongFast")
    }

    /// Finding 2: an empty (but REPORTED) primary channel name reads as
    /// the same honest "(default channel)" label `currentChannelName`
    /// already used for an imported default channel — never UNKNOWN,
    /// which would say the client reported nothing at all.
    func testEmptyPrimaryChannelNameFromTheClientReadsAsDefaultChannel() {
        let client = StubMeshtasticClient()
        client.connectedNodeNum = 1
        client.nodeConfig = NodeConfigSnapshot(primaryChannelName: "")
        let vm = SettingsViewModel(store: SettingsStore(defaults: defaults), channelImport: ChannelImportViewModel(),
                                    client: client)
        XCTAssertEqual(vm.currentChannelName, "(default channel)")
    }

    /// Finding 2: "the name fields pre-fill from the node's owner" —
    /// only while the user has never typed a local draft of their own.
    func testNameFieldsPrefillFromTheNodesOwnerWhenNoLocalDraftExists() {
        let client = StubMeshtasticClient()
        client.connectedNodeNum = 1
        client.nodeConfig = NodeConfigSnapshot(ownerLongName: "Firefly One", ownerShortName: "FF1")
        let vm = SettingsViewModel(store: SettingsStore(defaults: defaults), channelImport: ChannelImportViewModel(),
                                    client: client)
        XCTAssertEqual(vm.nodeLongName, "Firefly One")
        XCTAssertEqual(vm.nodeShortName, "FF1")
    }

    /// A LOCAL draft the user already typed is never silently overwritten
    /// by a later node-config refresh.
    func testNameFieldsDoNotPrefillOverAnExistingLocalDraft() async {
        let client = StubMeshtasticClient()
        client.connectedNodeNum = 1
        let store = SettingsStore(defaults: defaults)
        let vm = SettingsViewModel(store: store, channelImport: ChannelImportViewModel(), client: client)
        vm.setNodeLongName("My Own Draft")

        vm.observe()
        client.nodeConfig = NodeConfigSnapshot(ownerLongName: "Node's Name")
        await eventually("vm.nodeConfig to arrive") { vm.nodeConfig != nil }
        XCTAssertEqual(vm.nodeLongName, "My Own Draft", "a typed local draft is never overwritten by node data")
        vm.stopObserving()
    }

    // MARK: - NIT (PR #282 review): "from node" label on the pre-filled
    // name fields, mirroring `nodeConfigSourceLabel`'s Region/Channel
    // label just below in `channelSection`.

    func testNodeNameSourceLabelsReadFromNodeOnceThePrefillLands() {
        let client = StubMeshtasticClient()
        client.connectedNodeNum = 1
        client.nodeConfig = NodeConfigSnapshot(ownerLongName: "Firefly One", ownerShortName: "FF1")
        let vm = SettingsViewModel(store: SettingsStore(defaults: defaults), channelImport: ChannelImportViewModel(),
                                    client: client)
        XCTAssertEqual(vm.nodeLongNameSourceLabel, "from node")
        XCTAssertEqual(vm.nodeShortNameSourceLabel, "from node")
    }

    /// No source claimed until the client has actually reported an
    /// owner name — same honesty rule `nodeConfigSourceLabel`'s own test
    /// (`testChannelNameIsUnknownUntilSomethingIsImportedOrReportedByTheClient`)
    /// follows.
    func testNodeNameSourceLabelsAreNilUntilTheClientReportsAnOwnerName() {
        let vm = SettingsViewModel(store: SettingsStore(defaults: defaults), channelImport: ChannelImportViewModel())
        XCTAssertNil(vm.nodeLongNameSourceLabel)
        XCTAssertNil(vm.nodeShortNameSourceLabel)
    }

    /// The moment the user types a local draft in EITHER field, that
    /// field's own label stops claiming "from node" — even though the
    /// other field may still be a genuine, unedited pre-fill.
    func testNodeNameSourceLabelClearsOnlyForTheFieldWithALocalDraft() async {
        let client = StubMeshtasticClient()
        client.connectedNodeNum = 1
        let store = SettingsStore(defaults: defaults)
        let vm = SettingsViewModel(store: store, channelImport: ChannelImportViewModel(), client: client)

        vm.observe()
        client.nodeConfig = NodeConfigSnapshot(ownerLongName: "Node's Name", ownerShortName: "NODE")
        await eventually("vm.nodeConfig to arrive") { vm.nodeConfig != nil }
        XCTAssertEqual(vm.nodeLongNameSourceLabel, "from node")
        XCTAssertEqual(vm.nodeShortNameSourceLabel, "from node")

        vm.setNodeLongName("My Own Draft")
        XCTAssertNil(vm.nodeLongNameSourceLabel, "a typed draft must stop claiming the value came from the node")
        XCTAssertEqual(vm.nodeShortNameSourceLabel, "from node", "the untouched field is still an honest pre-fill")
        vm.stopObserving()
    }

    // MARK: - Finding 3 (first real-radio session, macOS): location
    // authorization requested when "Share phone GPS" turns on.

    /// Turning the toggle ON while permission is genuinely undecided
    /// asks for it — nothing in this app ever did before this finding.
    func testEnablingShareGPSRequestsLocationAuthorizationWhenNotDetermined() async {
        let location = ScriptedAuthSettingsLocationProvider(authorization: .notDetermined)
        let vm = SettingsViewModel(store: SettingsStore(defaults: defaults), channelImport: ChannelImportViewModel(),
                                    location: location)
        vm.setShareGPSWithNode(true)

        await eventually("a whenInUse authorization request") { location.whenInUseRequestCount > 0 }
        XCTAssertEqual(location.whenInUseRequestCount, 1)
    }

    /// Turning the toggle OFF never asks — only ON is the honest moment
    /// to prompt.
    ///
    /// Audited for the same "fixed sleep before an assert" flake this
    /// file's positive-condition waits above were rewritten around
    /// (`eventually(...)`): this one and
    /// `testEnablingShareGPSDoesNotReRequestOnceAlreadyDecided` below are
    /// absence checks — there is no state transition to wait for, only
    /// "nothing happened by the time we looked." A longer sleep only
    /// strengthens that assertion (never races it toward a false
    /// failure the way waiting on a positive condition does), so a
    /// fixed delay is the honest tool here, not a bug to fix.
    func testDisablingShareGPSNeverRequestsAuthorization() async {
        let location = ScriptedAuthSettingsLocationProvider(authorization: .notDetermined)
        let vm = SettingsViewModel(store: SettingsStore(defaults: defaults), channelImport: ChannelImportViewModel(),
                                    location: location)
        vm.setShareGPSWithNode(false)
        try? await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(location.whenInUseRequestCount, 0)
    }

    /// A user who already decided (either way) is never re-prompted just
    /// for flipping the toggle again.
    func testEnablingShareGPSDoesNotReRequestOnceAlreadyDecided() async {
        for authorization: LocationAuthorization in [.deniedOrRestricted, .whenInUse, .always, .locationServicesDisabled] {
            let location = ScriptedAuthSettingsLocationProvider(authorization: authorization)
            let vm = SettingsViewModel(store: SettingsStore(defaults: defaults), channelImport: ChannelImportViewModel(),
                                        location: location)
            vm.setShareGPSWithNode(true)
            try? await Task.sleep(nanoseconds: 50_000_000)
            XCTAssertEqual(location.whenInUseRequestCount, 0, "must not re-prompt for \(authorization)")
        }
    }

    /// PR #282 review, BLOCKING fix: same "counting mock provider"
    /// regression guard as `RadarViewModelTests`'
    /// `testMyPositionLineReadsAuthorizationExactlyOncePerCallThroughTheInjectedProviderOnly` —
    /// `setShareGPSWithNode` must read authorization exactly once per
    /// call, through the injected `LocationProviding` only, never a
    /// second, hidden services check of its own.
    func testSetShareGPSWithNodeReadsAuthorizationExactlyOncePerCallThroughTheInjectedProviderOnly() {
        let location = ScriptedAuthSettingsLocationProvider(authorization: .whenInUse)
        let vm = SettingsViewModel(store: SettingsStore(defaults: defaults), channelImport: ChannelImportViewModel(),
                                    location: location)

        // `value: false` short-circuits `if value, location.authorization
        // == .notDetermined` before the right-hand side is ever
        // evaluated — only `true` calls reach the authorization read at
        // all, so this drives three `true` calls to isolate exactly that.
        let before = location.authorizationReadCount
        vm.setShareGPSWithNode(true)
        vm.setShareGPSWithNode(true)
        vm.setShareGPSWithNode(true)
        XCTAssertEqual(location.authorizationReadCount - before, 3,
                       "setShareGPSWithNode must read the injected provider's cached authorization exactly once per call, never a second hidden check")
    }

    // MARK: - Festival data URL override (BLOCKING review finding 1: https-only)

    func testFestpackSourceURLOverrideAcceptsAValidHTTPSURL() {
        let store = SettingsStore(defaults: defaults)
        let vm = SettingsViewModel(store: store, channelImport: ChannelImportViewModel())

        vm.setFestpackSourceURLOverride("https://example.com/custom.festpack.json")

        XCTAssertEqual(vm.festpackSourceURLOverride, "https://example.com/custom.festpack.json")
        XCTAssertNil(vm.festpackSourceURLError)
    }

    func testFestpackSourceURLOverrideRejectsHTTPAndKeepsThePreviousValue() {
        let store = SettingsStore(defaults: defaults)
        let vm = SettingsViewModel(store: store, channelImport: ChannelImportViewModel())
        vm.setFestpackSourceURLOverride("https://example.com/good.festpack.json")

        vm.setFestpackSourceURLOverride("http://example.com/plaintext.festpack.json")

        XCTAssertEqual(vm.festpackSourceURLOverride, "https://example.com/good.festpack.json",
                       "a rejected override must never replace the previous value")
        XCTAssertNotNil(vm.festpackSourceURLError)
    }

    func testFestpackSourceURLOverrideRejectsFileScheme() {
        let store = SettingsStore(defaults: defaults)
        let vm = SettingsViewModel(store: store, channelImport: ChannelImportViewModel())

        vm.setFestpackSourceURLOverride("file:///etc/passwd")

        XCTAssertNil(vm.festpackSourceURLOverride)
        XCTAssertNotNil(vm.festpackSourceURLError)
    }

    func testFestpackSourceURLOverrideRejectsGarbage() {
        let store = SettingsStore(defaults: defaults)
        let vm = SettingsViewModel(store: store, channelImport: ChannelImportViewModel())

        vm.setFestpackSourceURLOverride("not a url at all")

        XCTAssertNil(vm.festpackSourceURLOverride)
        XCTAssertNotNil(vm.festpackSourceURLError)
    }

    func testClearingFestpackSourceURLOverrideClearsAnyPriorError() {
        let store = SettingsStore(defaults: defaults)
        let vm = SettingsViewModel(store: store, channelImport: ChannelImportViewModel())
        vm.setFestpackSourceURLOverride("http://example.com/plaintext.festpack.json")
        XCTAssertNotNil(vm.festpackSourceURLError)

        vm.setFestpackSourceURLOverride("")

        XCTAssertNil(vm.festpackSourceURLOverride)
        XCTAssertNil(vm.festpackSourceURLError)
    }
}

/// Finding 3's own `SettingsViewModelTests` seam — same shape as
/// `RadarViewModelTests`' private `ScriptedAuthLocationProvider`, kept
/// separate (this file cannot see that one — different test target/file
/// visibility) rather than sharing one across targets.
///
/// PR #282 review, BLOCKING fix: also counts reads of `authorization`
/// itself — see `RadarViewModelTests`' own copy of this same addition
/// for the full "counting mock provider" reasoning.
private final class ScriptedAuthSettingsLocationProvider: LocationProviding, @unchecked Sendable {
    private let lock = NSLock()
    private let _authorization: LocationAuthorization
    private var _whenInUseRequestCount = 0
    private var _authorizationReadCount = 0

    init(authorization: LocationAuthorization) { self._authorization = authorization }

    var authorization: LocationAuthorization {
        recordAuthorizationRead()
        return _authorization
    }
    var whenInUseRequestCount: Int { lock.lock(); defer { lock.unlock() }; return _whenInUseRequestCount }
    var authorizationReadCount: Int { lock.lock(); defer { lock.unlock() }; return _authorizationReadCount }

    // Same `NSLock` noasync convention as `RadarViewModelTests`' own
    // `ScriptedAuthLocationProvider`.
    private func recordRequest() {
        lock.lock(); _whenInUseRequestCount += 1; lock.unlock()
    }
    private func recordAuthorizationRead() {
        lock.lock(); _authorizationReadCount += 1; lock.unlock()
    }
    func requestWhenInUseAuthorization() async { recordRequest() }
    func requestAlwaysAuthorization() async {}
    func fixes() -> AsyncStream<LocationFix?> { AsyncStream { _ in } }
}

// MARK: - DiagnosticsViewModel

@MainActor
final class DiagnosticsViewModelTests: XCTestCase {

    func testStartsDisconnected() {
        let vm = DiagnosticsViewModel(client: StubMeshtasticClient())
        XCTAssertEqual(vm.linkStateLabel, "NOT CONNECTED")
    }

    /// A third independent `linkState()` subscriber, alongside
    /// ConnectViewModel's and CoreStore's own (the spec calls this out
    /// by name) — this proves Diagnostics reaches CONNECTED from the
    /// SAME client a Connect-screen `ConnectViewModel` is also
    /// observing, without either stealing the other's events.
    func testObserveTracksLinkStateIndependentlyOfAnotherSubscriber() async {
        let client = StubMeshtasticClient()
        let diagnostics = DiagnosticsViewModel(client: client)
        let connect = ConnectViewModel(client: client)

        diagnostics.observe()
        connect.observe()
        await connect.connect()

        await eventually("diagnostics.link to reach .ready") { diagnostics.link == .ready }
        XCTAssertEqual(diagnostics.linkStateLabel, "CONNECTED")
        XCTAssertEqual(connect.link, .ready)
        diagnostics.stopObserving()
    }

    func testEveryUnmodeledFieldReadsUnknownNotAPlaceholderNumber() {
        let vm = DiagnosticsViewModel(client: StubMeshtasticClient())
        for value in [vm.heardInLast10MinCount, vm.packetsIn, vm.packetsOut, vm.ackRate,
                      vm.nodeBatteryPercent, vm.nodeVoltage, vm.firmwareVersion] {
            XCTAssertEqual(value, "UNKNOWN")
        }
    }

    // MARK: - M2: reconnecting label + link uptime

    /// `StubMeshtasticClient` only ever yields its own fixed
    /// connect()/disconnect() sequence, which never includes
    /// `.reconnecting` — `ScriptedLinkClient` below lets this test push
    /// it directly.
    func testReconnectingReportsItsAttemptCount() async {
        let client = ScriptedLinkClient()
        let vm = DiagnosticsViewModel(client: client)
        vm.observe()

        client.push(.reconnecting(attempt: 2))
        await eventually("linkStateLabel to report the reconnect attempt") {
            vm.linkStateLabel == "RECONNECTING (attempt 2)"
        }
        XCTAssertEqual(vm.linkStateLabel, "RECONNECTING (attempt 2)",
                        "a silent HANDSHAKING during a multi-minute retry loop is not telling the truth")
        vm.stopObserving()
    }

    /// Uptime is `UNKNOWN` — never a fabricated `0s` — until the link has
    /// actually reached `.ready` at least once.
    func testUptimeIsUnknownBeforeEverReachingReady() {
        let vm = DiagnosticsViewModel(client: StubMeshtasticClient())
        XCTAssertEqual(vm.uptimeLabel, "UNKNOWN")
    }

    /// The power-cycle manual test (app/README.md) is specifically "does
    /// uptime reset after the node comes back" — this pins that a
    /// SECOND `.ready` streak starts its own clock rather than
    /// accumulating across the drop.
    func testUptimeResetsOnEachNewReadyStreak() async throws {
        var now = Date(timeIntervalSince1970: 0)
        let client = StubMeshtasticClient()
        let vm = DiagnosticsViewModel(client: client, now: { now })
        vm.observe()

        try await client.connect()
        await eventually("vm.link to reach .ready") { vm.link == .ready }
        now = now.addingTimeInterval(90) // 1m 30s of uptime
        XCTAssertEqual(vm.uptimeLabel, "1m 30s")

        await client.disconnect()
        await eventually("vm.link to leave .ready") { vm.link != .ready }
        XCTAssertEqual(vm.uptimeLabel, "UNKNOWN", "not connected right now — no uptime to report")

        now = now.addingTimeInterval(10)
        try await client.connect()
        await eventually("vm.link to reach .ready again") { vm.link == .ready }
        XCTAssertEqual(vm.uptimeLabel, "0s", "a fresh .ready streak starts its own clock, not the old one's")
        vm.stopObserving()
    }
}

/// A client a test drives by hand, publishing exactly the `LinkState`
/// values pushed to it — `StubMeshtasticClient` only ever yields its own
/// fixed connect()/disconnect() sequence, which never includes
/// `.reconnecting`. Invents nothing on its own, same rule every other
/// test double in this codebase follows.
private final class ScriptedLinkClient: MeshtasticClientProtocol, @unchecked Sendable {
    private let linkHub = EventHub<LinkState>()
    private let nodeHub = EventHub<MeshNodeSnapshot>()
    private let deliveryHub = EventHub<DeliveryEvent>()
    private let textHub = EventHub<IncomingText>()
    private let privateHub = EventHub<IncomingPrivate>()

    func linkState() -> AsyncStream<LinkState> { linkHub.subscribe() }
    func nodeUpdates() -> AsyncStream<MeshNodeSnapshot> { nodeHub.subscribe() }
    func deliveryUpdates() -> AsyncStream<DeliveryEvent> { deliveryHub.subscribe() }
    func incomingTexts() -> AsyncStream<IncomingText> { textHub.subscribe() }
    func incomingPrivate() -> AsyncStream<IncomingPrivate> { privateHub.subscribe() }
    var connectedNodeNum: UInt32?

    func connect() async throws {}
    func disconnect() async {}
    @discardableResult
    func sendText(_ text: String, to destination: UInt32, wantAck: Bool) async throws -> UInt32 { 0 }
    @discardableResult
    func sendPosition(_ fix: ExternalPositionFix, to destination: UInt32) async throws -> UInt32 { 0 }
    @discardableResult
    func sendPrivate(_ payload: Data, to destination: UInt32, wantAck: Bool) async throws -> UInt32 { 0 }
    @discardableResult
    func applyChannelSet(_ request: ChannelWriteRequest) async throws -> ChannelWriteReport {
        ChannelWriteReport(channels: request.channels, loraConfig: request.loraConfig)
    }
    @discardableResult
    func setOwner(longName: String, shortName: String) async throws -> OwnerWriteReport {
        OwnerWriteReport(longName: longName, shortName: shortName)
    }
    @discardableResult
    func setRegion(_ region: Config.LoRaConfig.RegionCode) async throws -> RegionWriteReport {
        RegionWriteReport(region: region)
    }
    func currentChannelTable() async throws -> [Channel] { [] }

    func push(_ state: LinkState) { linkHub.yield(state) }
}

/// M3 — records every admin write call it receives (and nothing else),
/// so a test can assert exactly how many happened and when: the
/// mechanical proof behind "never writes without confirmation"
/// (docs/specs/A01-companion-app.md M3). `shouldThrow`, when set, makes
/// every write fail with the given error instead of recording+
/// succeeding — for the error-path assertions.
private final class RecordingAdminWriteClient: MeshtasticClientProtocol, @unchecked Sendable {
    private let linkHub = EventHub<LinkState>()
    private let nodeHub = EventHub<MeshNodeSnapshot>()
    private let deliveryHub = EventHub<DeliveryEvent>()
    private let textHub = EventHub<IncomingText>()
    private let privateHub = EventHub<IncomingPrivate>()
    private let lock = NSLock()

    private var _channelWrites: [ChannelWriteRequest] = []
    private var _ownerWrites: [(String, String)] = []
    private var _regionWrites: [Config.LoRaConfig.RegionCode] = []
    var shouldThrow: AdminWriteError?
    /// M3 (PR #274 review, BLOCKING 1 & 2) — what `currentChannelTable()`
    /// reports, scripted per test; `channelTableError`, when set, makes
    /// that call fail instead (independent of `shouldThrow`, which only
    /// governs the write calls) — for a test proving `preparePlan()`
    /// surfaces a failed occupancy read honestly.
    var channelTable: [Channel] = []
    var channelTableError: Error?

    var channelWrites: [ChannelWriteRequest] { lock.lock(); defer { lock.unlock() }; return _channelWrites }
    var ownerWrites: [(String, String)] { lock.lock(); defer { lock.unlock() }; return _ownerWrites }
    var regionWrites: [Config.LoRaConfig.RegionCode] { lock.lock(); defer { lock.unlock() }; return _regionWrites }

    func linkState() -> AsyncStream<LinkState> { linkHub.subscribe() }
    func nodeUpdates() -> AsyncStream<MeshNodeSnapshot> { nodeHub.subscribe() }
    func deliveryUpdates() -> AsyncStream<DeliveryEvent> { deliveryHub.subscribe() }
    func incomingTexts() -> AsyncStream<IncomingText> { textHub.subscribe() }
    func incomingPrivate() -> AsyncStream<IncomingPrivate> { privateHub.subscribe() }
    var connectedNodeNum: UInt32? = 1

    func connect() async throws { linkHub.yield(.ready) }
    func disconnect() async { linkHub.yield(.disconnected) }
    @discardableResult
    func sendText(_ text: String, to destination: UInt32, wantAck: Bool) async throws -> UInt32 { 0 }
    @discardableResult
    func sendPosition(_ fix: ExternalPositionFix, to destination: UInt32) async throws -> UInt32 { 0 }
    @discardableResult
    func sendPrivate(_ payload: Data, to destination: UInt32, wantAck: Bool) async throws -> UInt32 { 0 }
    func yieldLink(_ state: LinkState) { linkHub.yield(state) }

    @discardableResult
    func applyChannelSet(_ request: ChannelWriteRequest) async throws -> ChannelWriteReport {
        if let shouldThrow { throw shouldThrow }
        recordChannelWrite(request)
        return ChannelWriteReport(channels: request.channels, loraConfig: request.loraConfig)
    }
    @discardableResult
    func setOwner(longName: String, shortName: String) async throws -> OwnerWriteReport {
        if let shouldThrow { throw shouldThrow }
        recordOwnerWrite(longName: longName, shortName: shortName)
        return OwnerWriteReport(longName: longName, shortName: shortName)
    }
    @discardableResult
    func setRegion(_ region: Config.LoRaConfig.RegionCode) async throws -> RegionWriteReport {
        if let shouldThrow { throw shouldThrow }
        recordRegionWrite(region)
        return RegionWriteReport(region: region)
    }
    func currentChannelTable() async throws -> [Channel] {
        if let channelTableError { throw channelTableError }
        return channelTable
    }

    // M3 / Swift 6: every locked mutation above happens in one of these
    // synchronous helpers, never lexically inside an `async` function
    // body — `NSLock.lock()`/`unlock()` are `noasync`, the same rule
    // `DemoMeshtasticClient`'s/`AppGraphTests.CountingClient`'s own
    // record helpers document (PR #275 review fixing this: this file's
    // three write methods above were still locking directly inside an
    // `async` body).
    private func recordChannelWrite(_ request: ChannelWriteRequest) {
        lock.lock(); _channelWrites.append(request); lock.unlock()
    }
    private func recordOwnerWrite(longName: String, shortName: String) {
        lock.lock(); _ownerWrites.append((longName, shortName)); lock.unlock()
    }
    private func recordRegionWrite(_ region: Config.LoRaConfig.RegionCode) {
        lock.lock(); _regionWrites.append(region); lock.unlock()
    }
}

// MARK: - M3: never-writes-without-confirmation state machine

@MainActor
final class AdminWriteConfirmationStateMachineTests: XCTestCase {

    // MARK: ChannelImportViewModel

    func testImportingAChannelURLNeverWritesToTheNode() throws {
        let client = RecordingAdminWriteClient()
        let vm = ChannelImportViewModel(client: client)

        var settings = ChannelSettings()
        settings.name = "Firefly"
        settings.moduleSettings.positionPrecision = 32
        let url = ChannelURL.encode(ChannelSet(settings: [settings]))
        vm.importURL(url)

        XCTAssertNotNil(vm.result)
        XCTAssertTrue(client.channelWrites.isEmpty, "parsing/importing a link must never itself write to the node")
    }

    func testConfirmApplyIsTheOnlyThingThatWrites() async throws {
        let client = RecordingAdminWriteClient()
        let vm = ChannelImportViewModel(client: client)

        var settings = ChannelSettings()
        settings.name = "Firefly"
        settings.moduleSettings.positionPrecision = 32
        // A "replace" import (addMode false) carries LoRa config so
        // makeChannelWritePlan can build a plan without a client round
        // trip mattering to the result.
        var lora = Config.LoRaConfig()
        lora.region = .us
        let url = ChannelURL.encode(ChannelSet(settings: [settings], loraConfig: lora))
        vm.importURL(url)
        XCTAssertTrue(client.channelWrites.isEmpty)

        let planned = await vm.preparePlan()
        XCTAssertTrue(planned, "preparePlan must succeed for a plain replace import")
        let ok = await vm.confirmApply()

        XCTAssertTrue(ok)
        XCTAssertEqual(client.channelWrites.count, 1, "confirmApply must write EXACTLY once")
        XCTAssertNotNil(vm.lastAppliedReport)
        XCTAssertNil(vm.applyErrorMessage)
    }

    /// PR #274 review, BLOCKING 1 & 2 — `confirmApply()` must refuse to
    /// write at all until `preparePlan()` has produced a plan; no result
    /// alone is ever enough (the old, pre-review behaviour).
    func testConfirmApplyWithNoPreparedPlanWritesNothingEvenWithAnImportedResult() async {
        let client = RecordingAdminWriteClient()
        let vm = ChannelImportViewModel(client: client)
        vm.importURL(ChannelURL.encode(ChannelSet(settings: [ChannelSettings()])))
        XCTAssertNotNil(vm.result)

        let ok = await vm.confirmApply()

        XCTAssertFalse(ok, "confirmApply must refuse without a plan from preparePlan()")
        XCTAssertTrue(client.channelWrites.isEmpty)
    }

    func testConfirmApplyWithNoImportedResultWritesNothing() async {
        let client = RecordingAdminWriteClient()
        let vm = ChannelImportViewModel(client: client)
        let ok = await vm.confirmApply()
        XCTAssertFalse(ok)
        XCTAssertTrue(client.channelWrites.isEmpty)
    }

    func testConfirmApplyFailureSurfacesAnErrorAndClearsOnRetry() async throws {
        let client = RecordingAdminWriteClient()
        client.shouldThrow = .readBackMismatch("channel 0 did not read back as written")
        let vm = ChannelImportViewModel(client: client)

        var settings = ChannelSettings()
        settings.name = "Firefly"
        settings.moduleSettings.positionPrecision = 32
        var lora = Config.LoRaConfig()
        lora.region = .us
        vm.importURL(ChannelURL.encode(ChannelSet(settings: [settings], loraConfig: lora)))

        _ = await vm.preparePlan()
        let ok = await vm.confirmApply()
        XCTAssertFalse(ok)
        XCTAssertNotNil(vm.applyErrorMessage)
        XCTAssertNil(vm.lastAppliedReport)
    }

    // MARK: SettingsViewModel

    func testEditingTheNodeNameDraftNeverWritesToTheNode() {
        let client = RecordingAdminWriteClient()
        let vm = SettingsViewModel(store: InMemorySettingsStore(), channelImport: ChannelImportViewModel(client: client), client: client)
        vm.setNodeLongName("Firefly One")
        vm.setNodeShortName("FF1")
        XCTAssertTrue(client.ownerWrites.isEmpty, "editing the local draft must never itself write to the node")
    }

    func testApplyNodeNameIsTheOnlyThingThatWritesTheOwner() async {
        let client = RecordingAdminWriteClient()
        let vm = SettingsViewModel(store: InMemorySettingsStore(), channelImport: ChannelImportViewModel(client: client), client: client)
        vm.setNodeLongName("Firefly One")
        vm.setNodeShortName("FF1")

        let ok = await vm.applyNodeName()

        XCTAssertTrue(ok)
        XCTAssertEqual(client.ownerWrites.count, 1)
        XCTAssertEqual(client.ownerWrites.first?.0, "Firefly One")
        XCTAssertEqual(client.ownerWrites.first?.1, "FF1")
    }

    func testChangingTheRegionPickerNeverWritesToTheNode() {
        let client = RecordingAdminWriteClient()
        let vm = SettingsViewModel(store: InMemorySettingsStore(), channelImport: ChannelImportViewModel(client: client), client: client)
        vm.regionSelection = .us
        XCTAssertTrue(client.regionWrites.isEmpty, "picking a region must never itself write to the node")
    }

    func testApplyRegionIsTheOnlyThingThatWritesTheRegion() async {
        let client = RecordingAdminWriteClient()
        let vm = SettingsViewModel(store: InMemorySettingsStore(), channelImport: ChannelImportViewModel(client: client), client: client)
        vm.regionSelection = .jp

        let ok = await vm.applyRegion()

        XCTAssertTrue(ok)
        XCTAssertEqual(client.regionWrites, [.jp])
    }

    func testRegionDefaultsToUnsetNeverGuessingUS() {
        let client = RecordingAdminWriteClient()
        let vm = SettingsViewModel(store: InMemorySettingsStore(), channelImport: ChannelImportViewModel(client: client), client: client)
        XCTAssertEqual(vm.regionSelection, .unset, "never presume a region the node was never asked about")
    }

    func testIsConnectedReflectsTheClientAtInit() {
        let client = RecordingAdminWriteClient()
        client.connectedNodeNum = nil
        let vm = SettingsViewModel(store: InMemorySettingsStore(), channelImport: ChannelImportViewModel(client: client), client: client)
        XCTAssertFalse(vm.isConnected)
    }

    func testObserveTracksLinkStateToReadyAndDisconnected() async {
        let client = RecordingAdminWriteClient()
        client.connectedNodeNum = nil
        let vm = SettingsViewModel(store: InMemorySettingsStore(), channelImport: ChannelImportViewModel(client: client), client: client)
        XCTAssertFalse(vm.isConnected)

        vm.observe()
        client.yieldLink(.ready)
        await eventually("vm.isConnected to become true") { vm.isConnected }
        XCTAssertTrue(vm.isConnected)

        client.yieldLink(.disconnected)
        await eventually("vm.isConnected to become false") { !vm.isConnected }
        XCTAssertFalse(vm.isConnected)
        vm.stopObserving()
    }

    /// PR #276 follow-up (app: singleton view models own their
    /// subscriptions in the composition root) — same test shape as
    /// `FireflyModelTests/AppGraphTests.swift`'s
    /// `testConnectViewModelObservesAutomaticallyWithNoCallerEverCallingObserve`,
    /// now for `SettingsViewModel`. Fails pre-fix: before
    /// `SettingsViewModel.makeObserving(...)` existed,
    /// `SettingsViewModel(store:channelImport:client:)` alone started no
    /// subscription at all — only `SettingsScreen.onAppear` did — so
    /// `isConnected` never left its `init`-time snapshot for a view
    /// model no screen had shown yet (or one shown once and then
    /// remounted away, `ConnectScreen`'s own NavigationSplitView bug).
    func testSettingsViewModelObservesAutomaticallyWithNoCallerEverCallingObserve() async {
        let client = RecordingAdminWriteClient()
        client.connectedNodeNum = nil
        let vm = SettingsViewModel.makeObserving(store: InMemorySettingsStore(),
                                                  channelImport: ChannelImportViewModel(client: client),
                                                  client: client)
        // Deliberately NOT calling `vm.observe()` here — the whole point
        // of this test; `.makeObserving(...)` above already did.
        XCTAssertFalse(vm.isConnected)

        client.yieldLink(.ready)
        await eventually("vm.isConnected to become true") { vm.isConnected }
        XCTAssertTrue(vm.isConnected,
                       "the Settings screen must reflect .ready even if no screen ever called observe() itself — " +
                       "SettingsViewModel.makeObserving(...) owns starting this subscription, not " +
                       "SettingsScreen.onAppear")
    }
}

// MARK: - Node picker (PeripheralDiscovery)

/// A `NodeScanning` a test drives by hand. No CoreBluetooth: this is the
/// whole reason the picker depends on the seam rather than on
/// `BLETransport` (`NodeScanning`'s own doc comment).
private actor FakeScanner: NodeScanning {
    private let hub = EventHub<BLEDiscoveredPeripheral>()
    private(set) var stopCount = 0
    private(set) var preferred: UUID?
    private(set) var scanCount = 0

    func scan() async -> AsyncStream<BLEDiscoveredPeripheral> {
        scanCount += 1
        return hub.subscribe()
    }

    func stopScanning() async { stopCount += 1 }
    func setPreferredPeripheral(_ id: UUID?) async { preferred = id }
    nonisolated func yield(_ peripheral: BLEDiscoveredPeripheral) { hub.yield(peripheral) }
}

@MainActor
final class PeripheralDiscoveryTests: XCTestCase {

    private func waitUntil(_ condition: @escaping () -> Bool, timeout: TimeInterval = 15,
                            file: StaticString = #filePath, line: UInt = #line) async {
        await eventually(timeout: timeout, file: file, line: line, condition)
    }

    /// The async twin, for conditions that have to `await` into an actor
    /// — same generous, failure-only timeout as `eventually`/`waitUntil`
    /// above, just able to `await` the condition itself.
    private func waitUntilAsync(_ condition: @escaping () async -> Bool, timeout: TimeInterval = 15,
                                 file: StaticString = #filePath, line: UInt = #line) async {
        let deadline = Date().addingTimeInterval(timeout)
        while true {
            if await condition() { return }
            if Date() >= deadline {
                XCTFail("timed out after \(timeout)s waiting for condition", file: file, line: line)
                return
            }
            try? await Task.sleep(nanoseconds: 5_000_000)
        }
    }

    /// The honest empty answer wherever there is no radio at all.
    func testStubDiscoversNothing() async {
        let discovery = StubPeripheralDiscovery()
        discovery.startScanning()
        var seen: [[DiscoveredPeripheral]] = []
        for await list in discovery.peripherals() {
            seen.append(list)
            break
        }
        XCTAssertEqual(seen, [[]], "an empty picker, never an invented peripheral")
    }

    func testRealDiscoveryPublishesWhatTheScannerSawDeduplicatedAndRanked() async {
        let scanner = FakeScanner()
        let discovery = MeshPeripheralDiscovery(scanner: scanner)
        var latest: [DiscoveredPeripheral] = []
        let stream = discovery.peripherals()
        let drain = Task { for await list in stream { latest = list } }

        discovery.startScanning()
        // Wait for the scan subscription to actually exist before
        // yielding: `scan()` is `async` (it is an actor's method), so
        // `startScanning()` returning is not the same fact as "the
        // stream is live", and `EventHub` is multicast, not replayed
        // (S1). In production this ordering is the transport's own to
        // guarantee — `BLETransport.scan()` subscribes BEFORE it calls
        // `scanForPeripherals`, so no advertisement can precede the
        // subscription; only a hand-driven fake can get ahead of it.
        await waitUntilAsync { await scanner.scanCount == 1 }
        let weak = UUID(uuidString: "00000000-0000-0000-0000-0000000000AA")!
        let strong = UUID(uuidString: "00000000-0000-0000-0000-0000000000BB")!
        scanner.yield(BLEDiscoveredPeripheral(id: weak, name: "Meshtastic_06b0", rssi: -80))
        scanner.yield(BLEDiscoveredPeripheral(id: strong, name: "Meshtastic_e7d4", rssi: -40))
        // The same board advertising again: an UPDATE, never a second row.
        scanner.yield(BLEDiscoveredPeripheral(id: weak, name: "Meshtastic_06b0", rssi: -70))

        await waitUntil { latest.count == 2 && latest.contains { $0.rssiDbm == -70 } }
        XCTAssertEqual(latest.map(\.name), ["Meshtastic_e7d4", "Meshtastic_06b0"],
                        "strongest first — the board on the table, not the one three tents over")
        XCTAssertEqual(latest.last?.rssiDbm, -70, "the newest sighting's reading, not the first one's")

        drain.cancel()
    }

    func testSelectingARowSetsThePreferredPeripheralAndConnectsNothing() async {
        let scanner = FakeScanner()
        let discovery = MeshPeripheralDiscovery(scanner: scanner)
        let id = UUID(uuidString: "00000000-0000-0000-0000-0000000000CC")!

        discovery.select(id.uuidString)

        await waitUntilAsync { await scanner.preferred != nil }
        let preferred = await scanner.preferred
        XCTAssertEqual(preferred, id)
        let scans = await scanner.scanCount
        XCTAssertEqual(scans, 0, "selecting a row must not start a scan, and must not connect")
    }

    func testStartScanningIsIdempotentAndStopReachesTheTransport() async {
        let scanner = FakeScanner()
        let discovery = MeshPeripheralDiscovery(scanner: scanner)

        discovery.startScanning()
        discovery.startScanning()
        discovery.startScanning()
        await waitUntilAsync { await scanner.scanCount >= 1 }
        let scans = await scanner.scanCount
        XCTAssertEqual(scans, 1, "RESCAN must not open a second subscription to the same radio")

        discovery.stopScanning()
        await waitUntilAsync { await scanner.stopCount == 1 }
        let stops = await scanner.stopCount
        XCTAssertEqual(stops, 1)
    }
}

// MARK: - RadioListBuilder (Connect screen redesign — owner feedback:
// "not sure to which radio, the connect button needs to be on the line
// item or something")

@MainActor
final class RadioListBuilderTests: XCTestCase {

    private func peripheral(_ id: String, name: String?, rssi: Int) -> DiscoveredPeripheral {
        DiscoveredPeripheral(id: id, name: name, rssiDbm: rssi)
    }

    private func snapshot(num: UInt32, longName: String?) -> MeshNodeSnapshot {
        MeshNodeSnapshot(num: num, shortName: nil, longName: longName, position: nil,
                          lastHeard: nil, rssiDbm: nil, snrDb: nil, hopsAway: nil)
    }

    /// Nothing remembered, nothing scanned, link disconnected — the
    /// honest empty answer, same rule `StubPeripheralDiscovery` itself
    /// follows: no row is better than an invented one.
    func testEmptyWithNothingRememberedAndNothingDiscovered() {
        let connect = ConnectViewModel(client: StubMeshtasticClient())
        let rows = RadioListBuilder.rows(discovered: [], connect: connect)
        XCTAssertTrue(rows.isEmpty)
    }

    func testEveryDiscoveredPeripheralGetsItsOwnConnectRowWhenDisconnected() {
        let connect = ConnectViewModel(client: StubMeshtasticClient())
        let discovered = [peripheral("AA", name: "Meshtastic_06b0", rssi: -80),
                           peripheral("BB", name: "Meshtastic_e7d4", rssi: -40)]
        let rows = RadioListBuilder.rows(discovered: discovered, connect: connect)
        XCTAssertEqual(rows.count, 2)
        XCTAssertTrue(rows.allSatisfy { $0.action == .connect && $0.status == .none && !$0.isRemembered })
    }

    /// Owner feedback item 2's REMEMBERED/FORGET row: must stay visible
    /// (and forgettable) even when a scan happens not to be running, or
    /// hasn't found it — a real radio stops advertising once connected,
    /// so "was it ever re-scanned" is not a fact worth gating FORGET on.
    func testRememberedPeripheralShowsAsRememberedEvenWhenNotInTheCurrentScan() {
        let store = InMemorySettingsStore()
        store.setString("11111111-1111-1111-1111-111111111111", .lastPeripheralID)
        let connect = ConnectViewModel(client: StubMeshtasticClient(), store: store)

        let rows = RadioListBuilder.rows(discovered: [], connect: connect)

        XCTAssertEqual(rows.count, 1)
        XCTAssertEqual(rows[0].status, .remembered)
        XCTAssertTrue(rows[0].isRemembered)
        XCTAssertEqual(rows[0].action, .connect)
    }

    func testActiveRowMergesTheScanNameAndRSSIWhenTheRememberedPeripheralIsInRange() {
        let store = InMemorySettingsStore()
        store.setString("11111111-1111-1111-1111-111111111111", .lastPeripheralID)
        let connect = ConnectViewModel(client: StubMeshtasticClient(), store: store)
        let discovered = [peripheral("11111111-1111-1111-1111-111111111111", name: "Meshtastic_e7d4", rssi: -52)]

        let rows = RadioListBuilder.rows(discovered: discovered, connect: connect)

        XCTAssertEqual(rows.count, 1, "the remembered radio's own scan sighting must not double as a second row")
        XCTAssertEqual(rows[0].title, "Meshtastic_e7d4")
        XCTAssertEqual(rows[0].rssiDbm, -52)
    }

    func testConnectedRowShowsConnectedChipAndOffersDisconnect() {
        let store = InMemorySettingsStore()
        store.setString("11111111-1111-1111-1111-111111111111", .lastPeripheralID)
        let client = StubMeshtasticClient()
        client.connectedNodeNum = 0x02e5_e3d4
        let connect = ConnectViewModel(client: client, store: store)
        connect.noteSelectedPeripheral(name: "Meshtastic_e7d4", rssiDbm: -56)
        connect.apply(.ready)
        connect.apply(snapshot(num: 0x02e5_e3d4, longName: "Firefly 2"))

        let rows = RadioListBuilder.rows(discovered: [], connect: connect)

        XCTAssertEqual(rows.count, 1)
        let row = rows[0]
        XCTAssertEqual(row.status, .connected)
        XCTAssertTrue(row.status.isHighlighted)
        XCTAssertEqual(row.action, .disconnect)
        XCTAssertEqual(row.title, "Firefly 2", "the node's own long name leads once want_config has it")
        XCTAssertEqual(row.subtitle, "Meshtastic_e7d4 · !02e5e3d4")
    }

    /// The specific regression the owner's UX complaint named: DISCONNECT
    /// (an abort) must stay reachable on the active row through the
    /// WHOLE connecting/handshaking window, matching
    /// `ConnectViewModel.isDisconnectable` — not just once `.ready`.
    func testConnectingRowShowsConnectingChipAndStillOffersDisconnect() {
        let store = InMemorySettingsStore()
        store.setString("11111111-1111-1111-1111-111111111111", .lastPeripheralID)
        let connect = ConnectViewModel(client: StubMeshtasticClient(), store: store)
        connect.noteSelectedPeripheral(name: "Meshtastic_e7d4", rssiDbm: -56)
        connect.apply(.handshaking)

        let rows = RadioListBuilder.rows(discovered: [], connect: connect)

        XCTAssertEqual(rows.count, 1)
        XCTAssertEqual(rows[0].status, .connecting)
        XCTAssertTrue(rows[0].status.isHighlighted)
        XCTAssertEqual(rows[0].action, .disconnect, "an abort must stay reachable mid-handshake")
    }

    /// Owner feedback item 2's per-row gating, the other half: every
    /// OTHER row's CONNECT is unavailable while the app is busy with a
    /// different radio — this app only ever talks to one at a time.
    func testOtherRowsAreUnavailableWhileTheActiveRowIsBusy() {
        let store = InMemorySettingsStore()
        store.setString("11111111-1111-1111-1111-111111111111", .lastPeripheralID)
        let connect = ConnectViewModel(client: StubMeshtasticClient(), store: store)
        connect.apply(.handshaking)
        let discovered = [peripheral("22222222-2222-2222-2222-222222222222", name: "Meshtastic_06b0", rssi: -70)]

        let rows = RadioListBuilder.rows(discovered: discovered, connect: connect)

        XCTAssertEqual(rows.count, 2)
        let other = rows.first { $0.id == "22222222-2222-2222-2222-222222222222" }
        XCTAssertEqual(other?.action, .unavailable)
        XCTAssertEqual(other?.status, RadioListRow.Status.none, "'.none' here — never Optional.none; the row itself is very much present")
    }

    /// The acceptance criterion named directly: demo mode (the iOS
    /// Simulator) has NO scanner at all (`StubPeripheralDiscovery`) and
    /// never persists a remembered peripheral (`DemoRunner.start()`
    /// calls `client.connect()` directly, never through the picker) —
    /// yet once it reaches `.ready` there must still be a connected row,
    /// or "which radio am I on?" goes right back to unanswered.
    func testDemoModeStyleConnectShowsAConnectedRowWithNoScanAndNoRememberedID() {
        let client = StubMeshtasticClient()
        client.connectedNodeNum = 0x0000_1001
        let connect = ConnectViewModel(client: client) // no store — nothing ever remembered
        connect.apply(.ready)

        let rows = RadioListBuilder.rows(discovered: [], connect: connect)

        XCTAssertEqual(rows.count, 1, "demo mode must still show a connected row")
        XCTAssertEqual(rows[0].status, .connected)
        XCTAssertEqual(rows[0].title, "!00001001", "no BLE name and no NodeInfo — the node id is all that's known")
        XCTAssertFalse(rows[0].isRemembered, "demo mode never persists a peripheral id")
    }

    func testFullyDisconnectedWithNothingRememberedShowsNoRowForAPastSession() {
        let connect = ConnectViewModel(client: StubMeshtasticClient())
        connect.apply(.failed("x"))
        connect.apply(.disconnected)
        let rows = RadioListBuilder.rows(discovered: [], connect: connect)
        XCTAssertTrue(rows.isEmpty)
    }
}

// MARK: - CrewSettingsViewModel (M2's "Crew" section in More)

@MainActor
final class CrewSettingsViewModelTests: XCTestCase {
    private func makeController() -> CrewPairingController {
        CrewPairingController(crew: CrewStore(), store: InMemoryCrewPairingStore())
    }

    func testStartsWithNoRowsWhenNobodyIsPaired() {
        let vm = CrewSettingsViewModel(pairing: makeController())
        XCTAssertTrue(vm.rows.isEmpty)
    }

    func testARowUsesTheMeshNameUntilAskedForANickname() {
        let pairing = makeController()
        pairing.crew.setIdentity(nodeID: 1, shortName: "SAM", longName: "Sam")
        pairing.pair(nodeID: 1)

        let vm = CrewSettingsViewModel(pairing: pairing)
        XCTAssertEqual(vm.rows.count, 1)
        XCTAssertEqual(vm.rows[0].meshName, "Sam")
        XCTAssertNil(vm.rows[0].nickname)
        XCTAssertEqual(vm.rows[0].displayName, "Sam")
    }

    /// An unnamed node (the mesh has not reported a name yet) falls
    /// back to the same honest `!nodeid` convention Nearby uses — never
    /// a blank row.
    func testAnUnnamedPairedMemberFallsBackToHexID() {
        let pairing = makeController()
        pairing.pair(nodeID: 0xAB)
        let vm = CrewSettingsViewModel(pairing: pairing)
        XCTAssertEqual(vm.rows[0].displayName, "!000000ab")
    }

    func testRenamePersistsThroughTheControllerAndPrefersTheNickname() {
        let pairing = makeController()
        pairing.crew.setIdentity(nodeID: 1, shortName: "SAM", longName: "Sam")
        pairing.pair(nodeID: 1)
        let vm = CrewSettingsViewModel(pairing: pairing)

        vm.rename(1, to: "Sammy")

        XCTAssertEqual(vm.rows[0].nickname, "Sammy")
        XCTAssertEqual(vm.rows[0].displayName, "Sammy", "a nickname takes priority over the mesh name")
        XCTAssertEqual(pairing.pairedRecords().first?.nickname, "Sammy", "the SAME persisted record, not a local copy")
    }

    func testRemoveDropsTheRowAndUnpairsThroughTheController() {
        let pairing = makeController()
        pairing.pair(nodeID: 1)
        let vm = CrewSettingsViewModel(pairing: pairing)

        vm.remove(1)

        XCTAssertTrue(vm.rows.isEmpty)
        XCTAssertEqual(pairing.crew.member(nodeID: 1, now: FireflyClock.nowMillis())?.paired, false)
    }

    /// `refresh()` picks up a pairing made elsewhere (e.g. Connect's
    /// Nearby section, through the SAME controller) — the Crew section
    /// is never its own disconnected copy of the roster.
    func testRefreshPicksUpAPairingMadeThroughTheSameController() {
        let pairing = makeController()
        let vm = CrewSettingsViewModel(pairing: pairing)
        XCTAssertTrue(vm.rows.isEmpty)

        pairing.pair(nodeID: 1)
        vm.refresh()

        XCTAssertEqual(vm.rows.count, 1)
    }
}
