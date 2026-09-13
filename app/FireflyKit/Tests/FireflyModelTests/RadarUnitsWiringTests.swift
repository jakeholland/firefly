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

// MARK: - Hardening QA pass

/// Two bounded-resource / honest-data properties of `RadarViewModel`
/// that nothing pinned before.
@MainActor
final class RadarHardeningTests: XCTestCase {

    private func makeModel() -> (RadarViewModel, MockRadarComputing) {
        let radar = MockRadarComputing()
        let model = RadarViewModel(radar: radar, heading: NoHeadingProvider(),
                                    location: UnavailableLocationProvider(), find: MockFindSession())
        return (model, radar)
    }

    /// `findReplies` grew for the life of the session: `handlePong`
    /// appended one entry per inbound PONG and only `startFind()` ever
    /// cleared it, so on a busy mesh — or simply after a FIND session
    /// ended — it accumulated for 12 hours. Bounded drop-oldest now,
    /// the same policy `ThreadViewModel.outbox` and `ff_feed_t`'s own
    /// ring already follow.
    func testFindRepliesIsBoundedAndKeepsTheNewest() {
        let (model, _) = makeModel()
        let total = RadarViewModel.findRepliesCap * 4
        for i in 0..<total {
            model.handlePong(fromNodeID: 42, nonce: UInt32(i), rssiDbm: -70, hasSNR: false, snrDb: 0)
        }
        XCTAssertEqual(model.findReplies.count, RadarViewModel.findRepliesCap,
                       "an unbounded reply list is a 12-hour memory leak on a busy mesh")
        XCTAssertEqual(model.findReplies.last?.id, total,
                       "drop-oldest: the NEWEST reply is the one that must survive")
    }

    /// Units used to be read exactly once, at graph construction, so a
    /// Settings change did not reach Radar until the next launch — while
    /// `MapViewModel` switched immediately, leaving the two screens
    /// disagreeing about units mid-session.
    func testUnitsPreferenceIsRereadOnEveryRecomputeNotOnlyAtConstruction() {
        let (model, radar) = makeModel()
        // A reference box, not a captured `var`: a `@Sendable` closure
        // capturing a mutated local is a Swift 6 warning today and an
        // error under a stricter mode, and this is the same shape
        // `AppGraph` really passes (a closure reading state that lives
        // somewhere else).
        let preference = ImperialPreferenceBox()
        model.imperialResolver = { preference.value }

        model.observe()
        defer { model.stopObserving() }
        XCTAssertEqual(radar.lastImperial, false, "the first compute uses the current preference")

        preference.value = true
        model.cycleSelection() // any interaction that recomputes
        XCTAssertEqual(radar.lastImperial, true,
                       "a Units change made mid-session must reach Radar without a relaunch")
    }
}

/// A `Sendable` box for the units preference the resolver closure reads
/// — see `testUnitsPreferenceIsRereadOnEveryRecomputeNotOnlyAtConstruction`.
/// Locked rather than `nonisolated(unsafe)` because the closure really
/// is `@Sendable` and could be called from anywhere.
private final class ImperialPreferenceBox: @unchecked Sendable {
    private let lock = NSLock()
    private var storage = false
    var value: Bool {
        get { lock.lock(); defer { lock.unlock() }; return storage }
        set { lock.lock(); storage = newValue; lock.unlock() }
    }
}
