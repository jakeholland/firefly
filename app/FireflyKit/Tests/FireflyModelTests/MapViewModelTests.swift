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

/// A `MapFestpackSource` test double whose `festpackUpdates()` stream is
/// driven entirely by `push(_:)` calls made AFTER `observe()` has already
/// subscribed — the exact shape of the real race
/// (`AlmanacFestpackProviderTests`' fetches, `FestivalPickerViewModel
/// .select(_:)`'s switches) this file's own new tests below prove
/// `MapViewModel` now survives. `currentFestpack()` is never called by
/// `MapViewModel` any more (`observe()`'s own doc comment) — it stays
/// implemented here only because `MapFestpackSource` still requires it.
private final class ControllableMapFestpackSource: MapFestpackSource, @unchecked Sendable {
    private let lock = NSLock()
    private var continuation: AsyncStream<MapFestpack?>.Continuation?

    func currentFestpack() async -> MapFestpack? { nil }

    func festpackUpdates() -> AsyncStream<MapFestpack?> {
        AsyncStream { continuation in
            lock.lock()
            self.continuation = continuation
            lock.unlock()
        }
    }

    /// Delivered on a detached `Task` with a short delay — never
    /// synchronously from the calling test — so a test can prove
    /// `MapViewModel` picks this up whenever it actually arrives,
    /// rather than merely because it happened to already be buffered
    /// before `observe()` subscribed.
    func push(_ pack: MapFestpack?, afterMillis: UInt64 = 0) {
        lock.lock()
        let continuation = self.continuation
        lock.unlock()
        guard let continuation else { return }
        if afterMillis == 0 {
            continuation.yield(pack)
        } else {
            Task.detached {
                try? await Task.sleep(nanoseconds: afterMillis * 1_000_000)
                continuation.yield(pack)
            }
        }
    }

    func finish() {
        lock.lock()
        let continuation = self.continuation
        lock.unlock()
        continuation?.finish()
    }
}

private func samplePack(name: String) -> MapFestpack {
    MapFestpack(meta: MapFestpackMeta(name: name, venue: FestpackLatLon(latitude: 43.7, longitude: -121.5)),
                stages: [], features: [], schedule: [])
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

// MARK: - Hardening QA pass

/// `refreshPins()` rebuilt and REASSIGNED `pins` unconditionally, and it
/// runs at 1 Hz for as long as the Map tab is open — so a crew standing
/// still still handed SwiftUI a brand-new array once a second, for the
/// twelve hours this app is supposed to survive on a battery. What that
/// costs downstream is the whole `Map { }` body: every pin, every
/// festpack polygon, the accuracy circle, and on the Field map a full
/// `ff_map`/`ff_geo` reprojection plus a `Canvas` redraw.
///
/// **Measured, not assumed** (the repo's own proxy-check rule): on the
/// toolchain this builds with today (Swift 6.3.3), `@Observable`
/// ALREADY suppresses an equal-valued assignment to an `Equatable`
/// property, so a `withObservationTracking` test passes with or without
/// the guard — a textbook proxy. That suppression is a property of the
/// Observation RUNTIME, not of this code, and this package's deployment
/// floor is iOS 17 / macOS 14, whose Observation predates it. So the
/// test below measures the thing this code actually controls and every
/// runtime shares: whether the array was rebuilt at all.
@MainActor
final class MapPinChurnTests: XCTestCase {

    private func makeModel(crew: CrewStore) -> MapViewModel {
        MapViewModel(crew: crew, location: UnavailableLocationProvider(),
                     heading: NoHeadingProvider(), festpackSource: DemoMapFestpackSource(),
                     connectivity: FixedConnectivity(.online))
    }

    private func standingStillCrew() -> CrewStore {
        let crew = CrewStore(now: { 60_000 })
        _ = crew.upsert(nodeID: 7)
        _ = crew.setPaired(nodeID: 7, paired: true)
        _ = crew.setIdentity(nodeID: 7, shortName: "TAY", longName: "Taylor")
        crew.onHeard(nodeID: 7, rxTimeMs: 60_000, direct: true)
        crew.onPosition(nodeID: 7, latitude: 43.7, longitude: -121.5, rxTimeMs: 60_000)
        return crew
    }

    /// Array storage identity: holding `before` keeps the old buffer
    /// alive, so a reassignment is guaranteed to land on a DIFFERENT
    /// allocation. Same address ⇒ `pins` was never written.
    private func storageIdentity(_ pins: [CrewMapPin]) -> UnsafeRawPointer? {
        pins.withUnsafeBufferPointer { UnsafeRawPointer($0.baseAddress) }
    }

    func testARefreshThatChangesNothingDoesNotRebuildThePinArray() {
        let model = makeModel(crew: standingStillCrew())
        model.refreshPins(now: 60_000)
        let before = model.pins
        XCTAssertFalse(before.isEmpty, "the fixture must actually produce a pin to be worth anything")
        let beforeStorage = storageIdentity(before)

        model.refreshPins(now: 60_000)

        XCTAssertEqual(model.pins, before, "same inputs, same pins")
        XCTAssertEqual(storageIdentity(model.pins), beforeStorage,
                       "a 1 Hz rebuild of an unchanged map is 43,200 needless invalidations over a festival day")
    }

    /// The other half, so the guard above cannot be satisfied by simply
    /// never updating `pins` at all: a refresh whose result genuinely
    /// differs must still publish it. Driven by advancing the clock far
    /// enough to move the member into a different age bucket — the same
    /// path the live 1 Hz loop takes.
    func testARefreshThatChangesSomethingStillPublishes() {
        let model = makeModel(crew: standingStillCrew())
        model.refreshPins(now: 60_000)
        let before = model.pins

        model.refreshPins(now: 60_000 + 10 * 60_000)

        XCTAssertNotEqual(model.pins, before, "ten minutes later this member is not as fresh")
        XCTAssertNotEqual(storageIdentity(model.pins), storageIdentity(before),
                          "a map that really changed must still be republished")
    }
}

// MARK: - "app: Map subscribes to festpack updates" (2026-09-13)

/// The Field forever-spinner / stale-festival-switch fix: `observe()`
/// used to read `festpackSource.currentFestpack()` exactly once. These
/// tests prove the replacement — a live subscription to
/// `festpackSource.festpackUpdates()` — actually behaves like one: a
/// late-arriving pack is still picked up, a festival switch reaches
/// `festpack`, a provider clear honestly nils it back out, and
/// `stopObserving()` genuinely cancels the subscription rather than
/// merely stopping the periodic loop.
@MainActor
final class MapViewModelFestpackObservationTests: XCTestCase {
    /// Never a fixed sleep or a fixed-iteration poll budget (the house
    /// rule `ConnectSettingsViewModelTests.swift`'s own `eventually`
    /// documents) — polls until `condition` is true or `timeout`
    /// genuinely elapses.
    private func eventually(_ description: String = "condition", timeout: TimeInterval = 5,
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

    private func makeModel(_ source: ControllableMapFestpackSource) -> MapViewModel {
        MapViewModel(crew: CrewStore(now: { 0 }), location: UnavailableLocationProvider(),
                     heading: NoHeadingProvider(), festpackSource: source,
                     connectivity: FixedConnectivity(.online))
    }

    /// The exact real-world race `AlmanacFestpackProvider`'s eager cache/
    /// bundle load and `AppGraph.start()`'s detached `refreshIfNeeded()`
    /// task create: the pack is not ready the instant `observe()` runs,
    /// only shortly after. The OLD one-shot `currentFestpack()` read
    /// would have captured `nil` here and never looked again —
    /// `FieldMapView`'s forever spinner. Mutation check: reverting
    /// `observe()` to the old one-shot read fails this test outright
    /// (`festpack` never becomes non-nil).
    func testFestpackBecomingAvailableAfterObserveStillReachesTheViewModel() async {
        let source = ControllableMapFestpackSource()
        let vm = makeModel(source)
        vm.observe()
        XCTAssertNil(vm.festpack, "nothing has loaded yet — must stay honestly nil, never a placeholder")

        source.push(samplePack(name: "Lost Lands"), afterMillis: 200)

        await eventually("festpack to load 200ms after observe()", timeout: 2) { vm.festpack != nil }
        XCTAssertEqual(vm.festpack?.meta.name, "Lost Lands")
    }

    /// A Settings festival-picker switch: the provider moves from one
    /// real pack straight to another, with Map never having called
    /// `observe()` again. Mirrors `LineupViewModel`'s own stream-driven
    /// update for the identical `FestpackProviding` seam.
    func testFestivalSwitchReplacesTheFestpackWithoutARestart() async {
        let source = ControllableMapFestpackSource()
        let vm = makeModel(source)
        vm.observe()

        source.push(samplePack(name: "Lost Lands"))
        await eventually("first festival to load") { vm.festpack?.meta.name == "Lost Lands" }

        source.push(samplePack(name: "Wakaan"))
        await eventually("switched festival to load") { vm.festpack?.meta.name == "Wakaan" }
    }

    /// `CurrentValueEventHub.clearCurrent()`'s honest counterpart
    /// (`AlmanacFestpackProvider.reloadIfFestivalChanged()`'s own
    /// `hub.yield(nil)`, `FestpackProvidingMapAdapterTests` proves the
    /// adapter half of this): a `nil` element must clear `festpack` back
    /// to nil, not leave the previous festival showing. This is the
    /// view-model half of "Field shows the honest empty state" —
    /// `fieldMapProjection(radiusPx:marginPx:)` already returns `nil`
    /// whenever `festpack` is `nil` (its own `guard let festpack else`),
    /// which is what `FieldMapView` renders its honest text for.
    func testProviderClearingItsPackNilsOutTheFestpack() async {
        let source = ControllableMapFestpackSource()
        let vm = makeModel(source)
        vm.observe()

        source.push(samplePack(name: "Lost Lands"))
        await eventually("pack to load") { vm.festpack != nil }

        source.push(nil)
        await eventually("cleared pack to reach the view model") { vm.festpack == nil }
        XCTAssertNil(vm.fieldMapProjection(radiusPx: 100, marginPx: 16),
                     "no pack means no honest projection to draw")
    }

    /// The leak half, same measured convention as
    /// `testPinRefreshLoopStopsTickingOnceStopObservingIsCalled` above:
    /// a value pushed AFTER `stopObserving()` must never reach
    /// `festpack` — proof the subscribing `Task` was actually cancelled,
    /// not merely that the view model stopped asking for new values on
    /// its own.
    func testStopObservingCancelsTheFestpackSubscription() async {
        let source = ControllableMapFestpackSource()
        let vm = makeModel(source)
        vm.observe()

        source.push(samplePack(name: "Lost Lands"))
        await eventually("initial pack to load") { vm.festpack != nil }

        vm.stopObserving()
        source.push(samplePack(name: "Wakaan"))
        // No fixed-length proof of a negative is airtight, but this
        // window is generous next to the 5ms poll `eventually` itself
        // uses, and the same convention already accepted for the
        // pin-refresh-loop leak test above.
        try? await Task.sleep(nanoseconds: 200_000_000)
        XCTAssertEqual(vm.festpack?.meta.name, "Lost Lands",
                       "a value pushed after stopObserving() must never reach a cancelled subscription")
    }
}
