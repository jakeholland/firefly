//
//  RadarUnitsWiringTests.swift — `AppGraph.makeRadarViewModel` reads the
//  REAL resolved units preference (M2), not the hard-`false` PR #265's
//  review flagged. New file, not an addition to `AppGraphTests.swift`
//  (not shared infra, but several other M2 slices land alongside this
//  one this sprint — the same reasoning `UnitsPreferenceTests.swift`'s
//  header gives).
//
//  No radio, no CoreBluetooth: `StubMeshtasticClient`/
//  `UnavailableLocationProvider` are the same honest doubles
//  `AppDependencies.stub()` itself uses.
//
import FireflyMesh
import FireflyModel
import XCTest

@MainActor
final class RadarUnitsWiringTests: XCTestCase {
    private func dependencies(store: any FireflyExtraSettingsStoring) -> AppDependencies {
        AppDependencies(client: StubMeshtasticClient(), location: UnavailableLocationProvider(),
                         heading: NoHeadingProvider(), store: store)
    }

    func testRadarViewModelIsMetricWhenTheStoredPreferenceIsMetric() {
        let store = InMemorySettingsStore()
        store.setUnitsPreference(.metric)
        let graph = AppGraph(dependencies: dependencies(store: store))
        XCTAssertFalse(graph.makeRadarViewModel().imperial)
    }

    func testRadarViewModelIsImperialWhenTheStoredPreferenceIsImperial() {
        let store = InMemorySettingsStore()
        store.setUnitsPreference(.imperial)
        let graph = AppGraph(dependencies: dependencies(store: store))
        XCTAssertTrue(graph.makeRadarViewModel().imperial)
    }

    /// A fresh graph (`InMemorySettingsStore()`'s own default, nothing
    /// ever written) resolves `.system` against THIS process's real
    /// locale — mirrors what a first launch actually does, unlike the
    /// explicit-locale tests in `UnitsPreferenceTests`, which test
    /// `UnitsPreference.resolvedImperial(locale:)` directly.
    func testRadarViewModelDefaultsToTheProcessLocalesSystemResolution() {
        let store = InMemorySettingsStore()
        let graph = AppGraph(dependencies: dependencies(store: store))
        XCTAssertEqual(graph.makeRadarViewModel().imperial, Locale.current.resolvesImperialForTesting())
    }
}

private extension Locale {
    /// Exactly `UnitsPreference.system.resolvedImperial(locale:)`'s own
    /// rule, re-stated here rather than imported, so this one assertion
    /// does not silently pass by calling the very function it is
    /// supposed to be checking against the real process locale.
    func resolvesImperialForTesting() -> Bool {
        switch measurementSystem {
        case .us, .uk: return true
        default: return false
        }
    }
}
