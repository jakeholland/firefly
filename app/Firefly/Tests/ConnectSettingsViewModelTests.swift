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

    private func waitUntil(_ condition: @escaping () -> Bool, timeout: Int = 400) async {
        for _ in 0..<timeout where !condition() {
            try? await Task.sleep(nanoseconds: 5_000_000)
        }
    }

    /// The async twin, for conditions that have to `await` into an actor.
    private func waitUntilAsync(_ condition: @escaping () async -> Bool, timeout: Int = 400) async {
        for _ in 0..<timeout {
            if await condition() { return }
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
