//
//  MapViewModelTests.swift — Map tab slice: selected-card text
//  (source + age) and the offline chip states, per this slice's own
//  test plan.
//
@testable import FireflyModel
import XCTest

private final class FixedConnectivity: NetworkConnectivityObserving, @unchecked Sendable {
    let state: MapConnectivity
    init(_ state: MapConnectivity) { self.state = state }
    func connectivityUpdates() -> AsyncStream<MapConnectivity> {
        AsyncStream { continuation in
            continuation.yield(state)
            continuation.finish()
        }
    }
}

@MainActor
final class MapViewModelTests: XCTestCase {
    private func pin(treatment: CrewMapPinTreatment, ageText: String = "3 MIN",
                      precisionGridMeters: Float? = nil) -> CrewMapPin {
        CrewMapPin(id: 1, name: "Taylor", colorIndex: 0, initial: "T", latitude: 43.7, longitude: -121.5,
                   treatment: treatment, ageText: ageText, distanceMeters: 120, bearingDegrees: 45,
                   precisionGridMeters: precisionGridMeters)
    }

    func testSelectedCardTextForLive() {
        let (source, age) = MapViewModel.selectedCardText(for: pin(treatment: .live, ageText: "12 SEC"))
        XCTAssertEqual(source, "LIVE GPS")
        XCTAssertEqual(age, "12 SEC")
    }

    func testSelectedCardTextForStale() {
        let (source, age) = MapViewModel.selectedCardText(for: pin(treatment: .staleRing, ageText: "~6 MIN"))
        XCTAssertEqual(source, "LAST KNOWN")
        XCTAssertEqual(age, "~6 MIN")
    }

    func testSelectedCardTextForLost() {
        let (source, _) = MapViewModel.selectedCardText(for: pin(treatment: .lostRing))
        XCTAssertEqual(source, "LAST KNOWN · LOST")
    }

    func testSelectedCardTextForAsserted() {
        let (source, age) = MapViewModel.selectedCardText(for: pin(treatment: .asserted, ageText: "ASSERTED"))
        XCTAssertEqual(source, "ASSERTED POSITION")
        XCTAssertEqual(age, "ASSERTED")
    }

    func testSelectedCardTextForImpreciseIncludesGrid() {
        let (source, _) = MapViewModel.selectedCardText(for: pin(treatment: .imprecise, precisionGridMeters: 110.4))
        XCTAssertEqual(source, "LOW PRECISION · ~110 m")
    }

    func testOfflineChipHiddenWhenOnline() {
        let vm = MapViewModel(crew: CrewStore(now: { 0 }), location: UnavailableLocationProvider(),
                               heading: NoHeadingProvider(), festpackSource: DemoMapFestpackSource(),
                               connectivity: FixedConnectivity(.online))
        vm.observe()
        XCTAssertNil(vm.offlineChipText)
    }

    func testOfflineChipShowsHonestTextWhenOffline() {
        let vm = MapViewModel(crew: CrewStore(now: { 0 }), location: UnavailableLocationProvider(),
                               heading: NoHeadingProvider(), festpackSource: DemoMapFestpackSource(),
                               connectivity: FixedConnectivity(.offline))
        let expectation = expectation(description: "connectivity observed")
        vm.observe()
        Task { @MainActor in
            // Let the AsyncStream's single `.offline` value land.
            try? await Task.sleep(nanoseconds: 50_000_000)
            expectation.fulfill()
        }
        wait(for: [expectation], timeout: 1.0)
        XCTAssertEqual(vm.offlineChipText, "OFFLINE — GPS map needs data")
        XCTAssertFalse(vm.offlineChipText?.contains("MB") ?? false,
                        "must never claim a fabricated cached-tile size")
    }

    // PR #283 review, BLOCKING 3: `AppGraph.makeMapViewModel()` used to
    // call `model.observe()` eagerly at graph construction (app
    // launch), so the 1 Hz `pinRefreshLoop` — plus its location/heading/
    // connectivity subscriptions — ran for the ENTIRE app session,
    // regardless of which tab was on screen, failing the "stops when
    // not visible (battery)" requirement outright. The fix moves
    // `observe()`/`stopObserving()` to `MapTabView`'s own `.onAppear`/
    // `.onDisappear`. This test proves `stopObserving()` actually halts
    // the periodic loop itself — not just the location/heading/
    // connectivity streams — via `refreshTickCount` (a test seam,
    // `MapViewModel`'s own doc comment on it): `ageText` alone can't
    // tell a live loop from a stopped one within a short test window
    // (`ff_fmt_age` deliberately reads "now" for a full minute).
    func testPinRefreshLoopStopsTickingOnceStopObservingIsCalled() async {
        let vm = MapViewModel(crew: CrewStore(now: { 0 }), location: UnavailableLocationProvider(),
                               heading: NoHeadingProvider(), festpackSource: DemoMapFestpackSource(),
                               connectivity: FixedConnectivity(.online))
        vm.observe()
        // >= 2 periodic ticks (1 Hz), on top of the one synchronous
        // `refreshPins()` call `observe()` itself makes.
        try? await Task.sleep(nanoseconds: 2_300_000_000)
        let ticksWhileObserving = vm.refreshTickCount
        XCTAssertGreaterThan(ticksWhileObserving, 1, "the periodic loop must keep ticking while the tab is visible")

        vm.stopObserving()
        try? await Task.sleep(nanoseconds: 2_300_000_000) // long enough for 2 more ticks, if it still ran
        XCTAssertEqual(vm.refreshTickCount, ticksWhileObserving,
                        "the 1Hz recompute loop must not tick once the Map tab is no longer visible (battery)")
    }

    func testDistanceBearingTextHonestlyNilWithoutBothFacts() {
        let vm = MapViewModel(crew: CrewStore(now: { 0 }), location: UnavailableLocationProvider(),
                               heading: NoHeadingProvider(), festpackSource: DemoMapFestpackSource())
        let noDistance = CrewMapPin(id: 1, name: "Taylor", colorIndex: 0, initial: "T", latitude: 43.7,
                                     longitude: -121.5, treatment: .live, ageText: "1 MIN", distanceMeters: nil,
                                     bearingDegrees: nil, precisionGridMeters: nil)
        XCTAssertNil(vm.distanceBearingText(for: noDistance, imperial: false))
    }

    // PR #283 review, SHOULD-FIX 4: `imperial` used to be `private`, so
    // `GPSMapView.selectedCard` had no way to read the real Units
    // setting and hardcoded `false` (always metric). Now public, and
    // reflects whatever the injected resolver currently returns.
    func testImperialExposesTheResolvedUnitsSetting() {
        let vm = MapViewModel(crew: CrewStore(now: { 0 }), location: UnavailableLocationProvider(),
                               heading: NoHeadingProvider(), festpackSource: DemoMapFestpackSource(),
                               imperial: { true })
        XCTAssertTrue(vm.imperial)
    }
}
