//
//  FestpackProvidingMapAdapterTests.swift — Map tab slice:
//  `FestpackProvidingMapAdapter.map(_:)` proved against a REAL pack —
//  `firmware/assets/field/lost-lands-2026.festpack.json`, the actual
//  bundled fallback pack `DemoFestpackProvider`'s live sibling ships
//  (richer, partially-placed geometry — unlike `FestpackParserTests`'
//  own smaller `firmware/festpack/tests/fixtures/` unit-test fixture,
//  which places nothing at all) — parsed through the real
//  `FestpackParser`/`fp_parse`, not a hand-built fixture of our own that
//  could quietly disagree with what the real parser actually produces.
//
import FireflyCore
@testable import FireflyModel
import XCTest

final class FestpackProvidingMapAdapterTests: XCTestCase {
    /// .../app/FireflyKit/Tests/FireflyModelTests/<this file>
    private var repoRoot: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent() // FireflyModelTests
            .deletingLastPathComponent() // Tests
            .deletingLastPathComponent() // FireflyKit
            .deletingLastPathComponent() // app
            .deletingLastPathComponent() // <repo root>
    }

    private func loadLostLands() throws -> Festpack {
        let url = repoRoot.appending(path: "firmware/assets/field/lost-lands-2026.festpack.json")
        let data = try Data(contentsOf: url)
        switch FestpackParser.parse(data) {
        case .success(let pack): return pack
        case .failure(let error): throw error
        }
    }

    func testRealPackHasAKnownOriginAndSevenStages() throws {
        // A precondition on the fixture itself, not the adapter — if
        // this ever stops being true, the adapter tests below would be
        // proving something other than what their names claim.
        let real = try loadLostLands()
        XCTAssertTrue(real.originKnown)
        XCTAssertEqual(real.stages.count, 7)
    }

    func testMapsAllSevenStagesWithColoursPresent() throws {
        let real = try loadLostLands()
        let mapped = try XCTUnwrap(FestpackProvidingMapAdapter.map(real))
        XCTAssertEqual(mapped.stages.count, 7)
        // Every real stage carries its own pack color (`fp_stage_t
        // .color_rgb`) — never 0/omitted, unlike centre/polygon below,
        // which are honestly missing for an unplaced stage.
        XCTAssertTrue(mapped.stages.allSatisfy { $0.colorHex != 0 })
    }

    func testPlacedStagesGetARealCentreRecoveredFromTheEastNorthMeters() throws {
        let real = try loadLostLands()
        let mapped = try XCTUnwrap(FestpackProvidingMapAdapter.map(real))
        // Lost Lands 2026's own fixture (2026-09-16 refresh): all seven
        // stages now have exactly one known map point each — six are
        // satellite-view pins, The Grove an approximate campground point
        // — untraced stubs, so `polygon` stays empty, but `centre` must
        // round-trip back to real WGS84 coordinates close to the pack's
        // own venue (never (0, 0), never the venue itself standing in
        // for a stage's own position).
        let placedIDs: Set<String> = ["crater", "prehistoric", "wompy-woods", "subsidia",
                                      "forest", "raptor-alley", "grove"]
        let placed = mapped.stages.filter { placedIDs.contains($0.id) }
        XCTAssertEqual(placed.count, 7)
        for stage in placed {
            let centre = try XCTUnwrap(stage.centre, "\(stage.id) should have a recovered centre")
            XCTAssertTrue(stage.polygon.isEmpty, "\(stage.id) is an untraced (1-point) stub, never a polygon")
            // Legend Valley venue: (39.9387, -82.4027) — every stage
            // should land within a fraction of a degree of it, proving
            // this is a real unprojection and not the venue itself
            // leaking through as every stage's centre.
            XCTAssertEqual(centre.latitude, 39.94, accuracy: 0.05)
            XCTAssertEqual(centre.longitude, -82.40, accuracy: 0.05)
        }
    }

    func testUnplacedStagesGetNoFabricatedCentre() throws {
        // Until the 2026-09-16 pack refresh this ran against the real
        // pack, where "raptor-alley"/"grove" had no map feature and
        // "forest" had one with an explicit null polygon. Every real
        // stage is placed now, so the unplaced contract is proved on a
        // pack of our own with the same three shapes the real one used
        // to have: no feature at all, and a feature whose polygon is
        // null. Both must come through with NO centre — never a
        // fabricated (0, 0) or venue-anchored placeholder.
        let json = """
        {"festpack":"0.1",
         "festival":{"name":"Unplaced Fest","year":2026,"start":"2026-09-18","end":"2026-09-20",
                     "venue":{"name":"Legend Valley","lat":39.9387,"lon":-82.4027,"approximate":true}},
         "stages":[{"id":"forest","name":"Forest Stage","color":"#ffc66b"},
                   {"id":"raptor-alley","name":"Raptor Alley","color":"#ffc66b"},
                   {"id":"grove","name":"The Grove","color":"#ffc66b"}],
         "schedule":[],
         "map":{"features":[{"kind":"stage","stage":"forest","label":"Forest Stage (unplaced)","polygon":null}],
                "landmarks":[]}}
        """
        let pack: Festpack
        switch FestpackParser.parse(Data(json.utf8)) {
        case .success(let p): pack = p
        case .failure(let error): throw error
        }
        let mapped = try XCTUnwrap(FestpackProvidingMapAdapter.map(pack))
        let unplacedIDs: Set<String> = ["raptor-alley", "grove", "forest"]
        let unplaced = mapped.stages.filter { unplacedIDs.contains($0.id) }
        XCTAssertEqual(unplaced.count, 3)
        for stage in unplaced {
            XCTAssertNil(stage.centre, "\(stage.id) has no known geometry in this pack — centre must stay nil")
            XCTAssertTrue(stage.polygon.isEmpty)
        }
    }

    func testFeaturesCarryNonEmptyPolygonsWhereTheRealPackTracedOne() throws {
        let real = try loadLostLands()
        let mapped = try XCTUnwrap(FestpackProvidingMapAdapter.map(real))
        // The real pack's own "Venue extent (approx.)" feature is a
        // 9-point traced polygon — proves the adapter carries a REAL,
        // multi-point polygon through `MapBridge.unproject` end to end,
        // not just single-point stubs.
        let venueExtent = try XCTUnwrap(mapped.features.first { $0.label == "Venue extent (approx.)" })
        XCTAssertEqual(venueExtent.polygon.count, 9)
        XCTAssertFalse(venueExtent.polygon.isEmpty)
    }

    func testMetaNameAndVenueComeFromTheRealPack() throws {
        let real = try loadLostLands()
        let mapped = try XCTUnwrap(FestpackProvidingMapAdapter.map(real))
        XCTAssertEqual(mapped.meta.name, "Lost Lands")
        XCTAssertEqual(mapped.meta.venue.latitude, 39.9387, accuracy: 0.0001)
        XCTAssertEqual(mapped.meta.venue.longitude, -82.4027, accuracy: 0.0001)
    }

    func testCurrentFestpackReturnsNilWhenTheRealProviderHasNoPackYet() async {
        let adapter = FestpackProvidingMapAdapter(provider: StubFestpackProviding(pack: nil))
        let mapped = await adapter.currentFestpack()
        XCTAssertNil(mapped)
    }

    func testCurrentFestpackDelegatesToTheRealProviderWhenAPackIsLoaded() async throws {
        let real = try loadLostLands()
        let adapter = FestpackProvidingMapAdapter(provider: StubFestpackProviding(pack: real))
        let mapped = await adapter.currentFestpack()
        XCTAssertEqual(mapped?.stages.count, 7)
    }

    // MARK: - festpackUpdates() ("app: Map subscribes to festpack updates", 2026-09-13)

    /// The mapping half: each real `Festpack` the provider publishes
    /// must come through as `map(_:)` would produce it, one element per
    /// upstream yield.
    func testFestpackUpdatesMapsEachRealPackThroughMap() async throws {
        let real = try loadLostLands()
        let provider = PushableFestpackProviding()
        let adapter = FestpackProvidingMapAdapter(provider: provider)
        var iterator = adapter.festpackUpdates().makeAsyncIterator()

        provider.push(real)
        let firstElement = await iterator.next()
        let first = try XCTUnwrap(firstElement)
        XCTAssertEqual(first, FestpackProvidingMapAdapter.map(real))
    }

    /// The honest-clear half: a `nil` from the real provider (a festival
    /// switch landing on nothing cached, `AlmanacFestpackProvider
    /// .reloadIfFestivalChanged()`'s own `hub.yield(nil)`) must come
    /// through as `nil` here too — never silently dropped, never the
    /// previous pack held over.
    func testFestpackUpdatesForwardsAnHonestNilWhenTheProviderClears() async throws {
        let real = try loadLostLands()
        let provider = PushableFestpackProviding()
        let adapter = FestpackProvidingMapAdapter(provider: provider)
        var iterator = adapter.festpackUpdates().makeAsyncIterator()

        provider.push(real)
        let loadedElement = await iterator.next()
        let loaded = try XCTUnwrap(loadedElement)
        XCTAssertNotNil(loaded)

        provider.push(nil)
        let cleared = await iterator.next()
        XCTAssertEqual(cleared, .some(nil), "a provider clear must forward as an honest nil element, not be dropped")
    }

    /// A `Festpack` that parses fine but has no known origin
    /// (`originKnown == false`) must map to `nil` on the stream too —
    /// same rule `currentFestpack()`/`map(_:)` already enforce for a
    /// one-shot read.
    func testFestpackUpdatesMapsAnUnknownOriginPackToNil() async throws {
        let noOriginJSON = Data("""
        {"festpack":"0.1","festival":{"name":"No Origin Fest","year":2027,"start":"2027-07-01","end":"2027-07-02"},
         "stages":[],"schedule":[]}
        """.utf8)
        guard case .success(let noOrigin) = FestpackParser.parse(noOriginJSON) else {
            return XCTFail("fixture must parse")
        }
        XCTAssertFalse(noOrigin.originKnown, "precondition: this fixture must have no known origin")

        let provider = PushableFestpackProviding()
        let adapter = FestpackProvidingMapAdapter(provider: provider)
        var iterator = adapter.festpackUpdates().makeAsyncIterator()

        provider.push(noOrigin)
        let mapped = await iterator.next()
        XCTAssertEqual(mapped, .some(nil))
    }
}

/// A minimal `FestpackProviding` test double — `currentFestpack()`'s own
/// delegation is what these two tests prove, not any provider's real
/// fetch/cache behavior (`AlmanacFestpackProviderTests` already owns
/// that).
private actor StubFestpackProviding: FestpackProviding {
    private let pack: Festpack?
    init(pack: Festpack?) { self.pack = pack }
    func current() async -> Festpack? { pack }
    func sourceState() async -> FestpackSourceState { pack == nil ? .none : .fetched() }
    nonisolated func festpackUpdates() -> AsyncStream<Festpack?> {
        AsyncStream { $0.finish() }
    }
    func refresh() async {}
    func refreshIfNeeded() async {}
}

/// A `FestpackProviding` test double whose `festpackUpdates()` stream is
/// driven by explicit `push(_:)` calls, including `nil` — proving
/// `FestpackProvidingMapAdapter.festpackUpdates()` forwards exactly what
/// the real provider publishes, `nil` included, rather than only ever
/// relaying a non-optional pack.
private final class PushableFestpackProviding: FestpackProviding, @unchecked Sendable {
    private let lock = NSLock()
    private var continuation: AsyncStream<Festpack?>.Continuation?
    private var latest: Festpack?

    /// A plain, non-`async` accessor — `NSLock.lock()`/`unlock()` are
    /// unavailable directly inside an `async` function body on this
    /// toolchain, so the locking itself has to happen in a synchronous
    /// helper that `current()` (below) merely calls, never `await`s.
    private func readLatest() -> Festpack? {
        lock.lock()
        defer { lock.unlock() }
        return latest
    }

    func current() async -> Festpack? { readLatest() }
    func sourceState() async -> FestpackSourceState { .none }
    func festpackUpdates() -> AsyncStream<Festpack?> {
        AsyncStream { continuation in
            lock.lock()
            self.continuation = continuation
            lock.unlock()
        }
    }
    func refresh() async {}
    func refreshIfNeeded() async {}

    func push(_ pack: Festpack?) {
        lock.lock()
        latest = pack
        let continuation = self.continuation
        lock.unlock()
        continuation?.yield(pack)
    }
}
