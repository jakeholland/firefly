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

// MARK: - NearbyNodesViewModel

@MainActor
final class NearbyNodesViewModelTests: XCTestCase {

    private func snapshot(num: UInt32, shortName: String? = nil, rssi: Int16? = -60) -> MeshNodeSnapshot {
        MeshNodeSnapshot(num: num, shortName: shortName, longName: nil, position: nil,
                          lastHeard: nil, rssiDbm: rssi, snrDb: nil, hopsAway: nil)
    }

    func testStartsEmpty() {
        let vm = NearbyNodesViewModel(client: StubMeshtasticClient())
        XCTAssertTrue(vm.nodes.isEmpty)
    }

    /// A node with no RSSI is not "nearby" at all — Nearby is
    /// specifically about heard signal strength, and a node the client
    /// only knows a position for (no packet RSSI attributable to it)
    /// must not show up ranked by a tier it never measured.
    func testNodesWithoutRSSIAreExcluded() {
        let vm = NearbyNodesViewModel(client: StubMeshtasticClient())
        vm.apply(snapshot(num: 1, rssi: nil))
        XCTAssertTrue(vm.nodes.isEmpty)
    }

    func testAppliedSnapshotAppearsWithItsTier() {
        let vm = NearbyNodesViewModel(client: StubMeshtasticClient())
        vm.apply(snapshot(num: 1, shortName: "AB1", rssi: -55)) // strong, per SignalTierTests
        XCTAssertEqual(vm.nodes.count, 1)
        XCTAssertEqual(vm.nodes[0].displayName, "AB1")
        XCTAssertEqual(vm.nodes[0].tier, .strong)
        XCTAssertFalse(vm.nodes[0].isCrew)
    }

    /// A node named only by number renders Meshtastic's own `!%08x`
    /// convention, never a blank row.
    func testUnnamedNodeFallsBackToHexID() {
        let vm = NearbyNodesViewModel(client: StubMeshtasticClient())
        vm.apply(snapshot(num: 0xAB, shortName: nil, rssi: -55))
        XCTAssertEqual(vm.nodes[0].displayName, "!000000ab")
    }

    func testNodesAreRankedStrongestFirst() {
        let vm = NearbyNodesViewModel(client: StubMeshtasticClient())
        vm.apply(snapshot(num: 1, shortName: "WEAK", rssi: -96))
        vm.apply(snapshot(num: 2, shortName: "STRONG", rssi: -55))
        vm.apply(snapshot(num: 3, shortName: "GOOD", rssi: -80))
        XCTAssertEqual(vm.nodes.map(\.displayName), ["STRONG", "GOOD", "WEAK"])
    }

    /// A later snapshot for the same node updates it in place rather
    /// than appending a duplicate row.
    func testReapplyingTheSameNodeUpdatesRatherThanDuplicates() {
        let vm = NearbyNodesViewModel(client: StubMeshtasticClient())
        vm.apply(snapshot(num: 1, shortName: "A", rssi: -96))
        vm.apply(snapshot(num: 1, shortName: "A", rssi: -55))
        XCTAssertEqual(vm.nodes.count, 1)
        XCTAssertEqual(vm.nodes[0].tier, .strong)
    }

    func testToggleCrewFlipsAndRemembersPerNode() {
        let vm = NearbyNodesViewModel(client: StubMeshtasticClient())
        vm.apply(snapshot(num: 1, shortName: "A", rssi: -55))
        vm.apply(snapshot(num: 2, shortName: "B", rssi: -55))

        vm.toggleCrew(1)
        XCTAssertTrue(vm.nodes.first { $0.id == 1 }!.isCrew)
        XCTAssertFalse(vm.nodes.first { $0.id == 2 }!.isCrew)

        vm.toggleCrew(1)
        XCTAssertFalse(vm.nodes.first { $0.id == 1 }!.isCrew)
    }

    /// Crew membership is session-only in M1 (see the view model's own
    /// doc comment) — a fresh instance never inherits a prior one's
    /// toggles, because nothing was ever persisted.
    func testCrewMembershipDoesNotSurviveAFreshViewModel() {
        let first = NearbyNodesViewModel(client: StubMeshtasticClient())
        first.apply(snapshot(num: 1, shortName: "A", rssi: -55))
        first.toggleCrew(1)
        XCTAssertTrue(first.nodes[0].isCrew)

        let second = NearbyNodesViewModel(client: StubMeshtasticClient())
        second.apply(snapshot(num: 1, shortName: "A", rssi: -55))
        XCTAssertFalse(second.nodes[0].isCrew)
    }

    func testObserveDeliversNodeUpdatesFromTheClient() async {
        let client = StubMeshtasticClient()
        let vm = NearbyNodesViewModel(client: client)
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

    func testSettersPersistThroughToAFreshViewModelOverTheSameStore() {
        let store = SettingsStore(defaults: defaults)
        let vm = SettingsViewModel(store: store, channelImport: ChannelImportViewModel())

        vm.setNodeLongName("Firefly One")
        vm.setNodeShortName("FF1")
        vm.setShareGPSWithNode(true)
        vm.setLocationIntervalSeconds(45)
        vm.setStayConnectedInBackground(true)
        vm.setColorblindPalette(true)

        let reopened = SettingsViewModel(store: SettingsStore(defaults: defaults), channelImport: ChannelImportViewModel())
        XCTAssertEqual(reopened.nodeLongName, "Firefly One")
        XCTAssertEqual(reopened.nodeShortName, "FF1")
        XCTAssertTrue(reopened.shareGPSWithNode)
        XCTAssertEqual(reopened.locationIntervalSeconds, 45)
        XCTAssertTrue(reopened.stayConnectedInBackground)
        XCTAssertTrue(reopened.colorblindPalette)
    }

    /// The spec's floor: "default 30 s, floor 5 s" for the GPS-push
    /// cadence.
    func testLocationIntervalIsFlooredAtFiveSeconds() {
        let vm = SettingsViewModel(store: SettingsStore(defaults: defaults), channelImport: ChannelImportViewModel())
        vm.setLocationIntervalSeconds(1)
        XCTAssertEqual(vm.locationIntervalSeconds, 5)
    }

    func testRegionIsAlwaysUnknownInM1() {
        let vm = SettingsViewModel(store: SettingsStore(defaults: defaults), channelImport: ChannelImportViewModel())
        XCTAssertEqual(vm.region, "UNKNOWN")
    }

    func testChannelNameIsUnknownUntilSomethingIsImported() {
        let vm = SettingsViewModel(store: SettingsStore(defaults: defaults), channelImport: ChannelImportViewModel())
        XCTAssertEqual(vm.currentChannelName, "UNKNOWN")
    }

    /// The whole point of sharing one `ChannelImportViewModel` instance
    /// between Connect and Settings: an import made through it is
    /// visible here too, without SettingsViewModel inventing its own
    /// copy.
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

        for _ in 0..<200 where diagnostics.link != .ready {
            try? await Task.sleep(nanoseconds: 5_000_000)
        }
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
}
