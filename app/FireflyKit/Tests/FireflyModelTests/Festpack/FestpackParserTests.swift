//
//  FestpackParserTests.swift — `FestpackParser.parse` round-tripped
//  against the real, vendored Lost Lands 2026 fixture
//  (firmware/festpack/tests/fixtures/lost-lands-2026.festpack.json),
//  the same file `test_festpack.c`/`test_sched.c` exercise on the C
//  side. This is the app-side twin of those C tests, not a duplicate
//  parser: every number here is read back off `Festpack`, decoded
//  entirely through `fp_parse`.
//
import FireflyCore
import FireflyModel
import XCTest

final class FestpackParserTests: XCTestCase {
    /// .../app/FireflyKit/Tests/FireflyModelTests/Festpack/<this file>
    private var repoRoot: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent() // Festpack
            .deletingLastPathComponent() // FireflyModelTests
            .deletingLastPathComponent() // Tests
            .deletingLastPathComponent() // FireflyKit
            .deletingLastPathComponent() // app
            .deletingLastPathComponent() // <repo root>
    }

    private func fixture(_ name: String) throws -> Data {
        let url = repoRoot.appending(path: "firmware/festpack/tests/fixtures/\(name)")
        return try Data(contentsOf: url)
    }

    private func loadLostLands() throws -> Festpack {
        let data = try fixture("lost-lands-2026.festpack.json")
        switch FestpackParser.parse(data) {
        case .success(let pack): return pack
        case .failure(let error): throw error
        }
    }

    // MARK: - AC1: the real pack, 222 sets / 55 after-midnight

    func testLostLandsRoundTripHas222SetsAnd7Stages() throws {
        let pack = try loadLostLands()
        XCTAssertEqual(pack.name, "Lost Lands")
        XCTAssertEqual(pack.year, 2026)
        XCTAssertEqual(pack.sets.count, 222)
        XCTAssertEqual(pack.stages.count, 7)
        XCTAssertEqual(pack.startDayOfYear, 261) // 2026-09-18
        XCTAssertEqual(pack.endDayOfYear, 263)   // 2026-09-20
        XCTAssertEqual(pack.dayCount, 3)
    }

    func testLostLands55SetsAreAfterMidnight() throws {
        let pack = try loadLostLands()
        // "After midnight" = start folded to >= 1440 in the night's own
        // minute space (S05's 2026-09-09 amendment) — the exact
        // condition test_festpack.c's own 55-count assertion uses.
        let afterMidnight = pack.sets.filter { ($0.startMinute ?? 0) >= 1440 }
        XCTAssertEqual(afterMidnight.count, 55)
    }

    func testLostLandsAfterMidnightSetsFoldOntoFridayNight() throws {
        let pack = try loadLostLands()
        func set(_ artist: String) -> FestpackScheduleSet? { pack.sets.first { $0.artist == artist } }

        let resistance = try XCTUnwrap(set("The Resistance"))
        let sippy = try XCTUnwrap(set("Sippy"))
        let oliverse = try XCTUnwrap(set("Oliverse"))

        XCTAssertEqual(resistance.nightDayOfYear, 261)
        XCTAssertEqual(resistance.startMinute, 22 * 60 + 45)

        XCTAssertEqual(sippy.nightDayOfYear, 261) // Friday night, not Saturday's 262
        XCTAssertEqual(sippy.startMinute, 24 * 60 + 15) // 00:15 folded to 1455
        XCTAssertNil(sippy.endMinute)
        XCTAssertGreaterThan(sippy.startMinute!, resistance.startMinute!)

        XCTAssertEqual(oliverse.nightDayOfYear, 261)
        XCTAssertEqual(oliverse.startMinute, 27 * 60) // 03:00 folded to 1620
    }

    func testLostLandsExcisionHasARealEndDayDatedEnd() throws {
        let pack = try loadLostLands()
        let excision = try XCTUnwrap(pack.sets.first { $0.artist == "Excision" })
        XCTAssertEqual(excision.nightDayOfYear, 261)
        XCTAssertEqual(excision.startMinute, 22 * 60 + 10)
        XCTAssertEqual(excision.endMinute, 24 * 60 + 10) // 00:10 next morning, end_day-dated -> 1450
    }

    func testLostLandsMetaUpdatedSourcesAndCompleteFlags() throws {
        let pack = try loadLostLands()
        XCTAssertTrue(pack.meta.present)
        XCTAssertEqual(pack.meta.updated, "2026-09-09")
        XCTAssertEqual(pack.meta.sources.count, 4)
        XCTAssertEqual(pack.meta.sources.first, "https://www.lostlandsfestival.com/")
        XCTAssertEqual(pack.meta.completeLineup, "full")
        XCTAssertEqual(pack.meta.completeSetTimes, "full")
        XCTAssertEqual(pack.meta.completeMap, "none")
    }

    func testLostLandsHasNoUTCOffsetAndDefaultsHonestlyAssumed() throws {
        let pack = try loadLostLands()
        XCTAssertEqual(pack.utcOffsetMinutes, -240)
        XCTAssertTrue(pack.utcOffsetAssumed)
    }

    // MARK: - Error paths

    func testWrongVersionFails() {
        let json = Data(#"{"festpack":"9.9"}"#.utf8)
        switch FestpackParser.parse(json) {
        case .failure(.wrongVersion): break
        default: XCTFail("expected .wrongVersion")
        }
    }

    func testMalformedJSONFails() {
        let json = Data("not json at all".utf8)
        switch FestpackParser.parse(json) {
        case .failure(.malformedJSON): break
        default: XCTFail("expected .malformedJSON")
        }
    }

    func testMinimalPackWithNoMetaReadsAsNotPresent() throws {
        let data = try fixture("minimal.festpack.json")
        guard case .success(let pack) = FestpackParser.parse(data) else {
            return XCTFail("expected minimal.festpack.json to parse")
        }
        XCTAssertFalse(pack.meta.present)
        XCTAssertNil(pack.meta.updated)
        XCTAssertTrue(pack.meta.sources.isEmpty)
    }
}
