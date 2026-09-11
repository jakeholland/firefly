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

    func testDistanceBearingTextHonestlyNilWithoutBothFacts() {
        let vm = MapViewModel(crew: CrewStore(now: { 0 }), location: UnavailableLocationProvider(),
                               heading: NoHeadingProvider(), festpackSource: DemoMapFestpackSource())
        let noDistance = CrewMapPin(id: 1, name: "Taylor", colorIndex: 0, initial: "T", latitude: 43.7,
                                     longitude: -121.5, treatment: .live, ageText: "1 MIN", distanceMeters: nil,
                                     bearingDegrees: nil, precisionGridMeters: nil)
        XCTAssertNil(vm.distanceBearingText(for: noDistance, imperial: false))
    }
}
