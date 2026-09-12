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
        // Lost Lands 2026's own fixture: four stages have exactly one
        // known map point each (crater/prehistoric/wompy-woods/subsidia)
        // — untraced stubs, so `polygon` stays empty, but `centre` must
        // round-trip back to real WGS84 coordinates close to the pack's
        // own venue (never (0, 0), never the venue itself standing in
        // for a stage's own position).
        let placedIDs: Set<String> = ["crater", "prehistoric", "wompy-woods", "subsidia"]
        let placed = mapped.stages.filter { placedIDs.contains($0.id) }
        XCTAssertEqual(placed.count, 4)
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
        let real = try loadLostLands()
        let mapped = try XCTUnwrap(FestpackProvidingMapAdapter.map(real))
        // "raptor-alley"/"grove" have no map feature at all; "forest"
        // has one with an explicit null polygon ("Forest Stage
        // (unplaced)") — all three must come through with NO centre,
        // never a fabricated (0, 0) or venue-anchored placeholder.
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
}

/// A minimal `FestpackProviding` test double — `currentFestpack()`'s own
/// delegation is what these two tests prove, not any provider's real
/// fetch/cache behavior (`AlmanacFestpackProviderTests` already owns
/// that).
private actor StubFestpackProviding: FestpackProviding {
    private let pack: Festpack?
    init(pack: Festpack?) { self.pack = pack }
    func current() async -> Festpack? { pack }
    func sourceState() async -> FestpackSourceState { pack == nil ? .none : .fresh }
    nonisolated func festpackUpdates() -> AsyncStream<Festpack> {
        AsyncStream { $0.finish() }
    }
    func refresh() async {}
}
