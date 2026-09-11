//
//  FestpackScheduleTests.swift — `FestpackSchedule` (the `ff_sched`
//  bridge) exercised against the real Lost Lands pack, at the same two
//  fixed fake clocks `test_sched.c`'s own S07 integration cases use:
//  Fri 2026-09-18 23:30 (day_doy=Friday, now_min=1410, Excision live)
//  and Sat 2026-09-19 00:30 (day_doy=Friday still, now_min=1470,
//  Excision over, Sippy live) — the concrete proof that pre- and
//  post-midnight sets share one festival night and sort correctly
//  against each other.
//
import FireflyModel
import XCTest

final class FestpackScheduleTests: XCTestCase {
    private var repoRoot: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
    }

    private func loadLostLands() throws -> Festpack {
        let url = repoRoot.appending(path: "firmware/festpack/tests/fixtures/lost-lands-2026.festpack.json")
        let data = try Data(contentsOf: url)
        guard case .success(let pack) = FestpackParser.parse(data) else {
            throw XCTSkip("fixture failed to parse")
        }
        return pack
    }

    func testFriday2330ExcisionLiveWithHonestFortyMinutesLeft() throws {
        let pack = try loadLostLands()
        let friday = pack.startDayOfYear // 261
        let rows = FestpackSchedule.nowPlaying(in: pack, night: friday, nowMinute: 23 * 60 + 30)

        XCTAssertEqual(rows.count, 5)
        XCTAssertTrue(rows.contains { $0.set.artist == "The Resistance" })
        XCTAssertTrue(rows.contains { $0.set.artist == "Shlump" })
        XCTAssertFalse(rows.contains { $0.set.artist == "Sigma" }) // Raptor Alley, not until 00:00

        let excision = try XCTUnwrap(rows.first { $0.set.artist == "Excision" })
        XCTAssertEqual(excision.set.startMinute, 22 * 60 + 10)
        XCTAssertEqual(excision.set.endMinute, 24 * 60 + 10)
        XCTAssertEqual(excision.minutesLeft, 40)
        XCTAssertTrue(excision.percentValid)
    }

    func testSaturday0030AfterMidnightSippyLiveExcisionOver() throws {
        let pack = try loadLostLands()
        let friday = pack.startDayOfYear
        let rows = FestpackSchedule.nowPlaying(in: pack, night: friday, nowMinute: 30 + 1440)

        XCTAssertEqual(rows.count, 5)
        XCTAssertFalse(rows.contains { $0.set.artist == "Excision" }) // real end_day-dated 00:10 end has passed
        XCTAssertFalse(rows.contains { $0.set.artist == "The Resistance" }) // handed to Sippy at 00:15

        let sippy = try XCTUnwrap(rows.first { $0.set.artist == "Sippy" })
        XCTAssertEqual(sippy.set.startMinute, 24 * 60 + 15) // 1455

        // The ordering claim, stated directly: Sippy's after-midnight
        // start sorts AFTER the pre-midnight set it followed on the same
        // stage/night, in the day lineup ordering.
        let dayLineup = FestpackSchedule.daySets(in: pack, night: friday)
        let resistanceIndex = try XCTUnwrap(dayLineup.firstIndex { $0.artist == "The Resistance" })
        let sippyIndex = try XCTUnwrap(dayLineup.firstIndex { $0.artist == "Sippy" })
        XCTAssertLessThan(resistanceIndex, sippyIndex)
    }

    func testDaySetsIncludesEveryFridayNightSetInAscendingOrder() throws {
        let pack = try loadLostLands()
        let friday = pack.startDayOfYear
        let lineup = FestpackSchedule.daySets(in: pack, night: friday)
        // Friday's night is all 64 of its sets (S05's own integration
        // test asserts the identical count on the C side).
        XCTAssertEqual(lineup.count, 64)
        let knownStarts = lineup.compactMap(\.startMinute)
        XCTAssertEqual(knownStarts, knownStarts.sorted())
    }

    func testNextStarredFindsTheEarliestUnstartedStarredSet() throws {
        let pack = try loadLostLands()
        let friday = pack.startDayOfYear
        // Excision (22:10) and Sippy (00:15/1455) both starred; asking
        // at 21:00 (1260) should surface Excision first.
        let starred: Set<String> = ["Excision", "Sippy"]
        let next = FestpackSchedule.nextStarred(in: pack, night: friday, nowMinute: 21 * 60, starred: starred)
        XCTAssertEqual(next?.set.artist, "Excision")
        XCTAssertEqual(next?.minutesUntil, 22 * 60 + 10 - 21 * 60)
    }

    func testNoStarredSetsReturnsNil() throws {
        let pack = try loadLostLands()
        let next = FestpackSchedule.nextStarred(in: pack, night: pack.startDayOfYear, nowMinute: 0, starred: [])
        XCTAssertNil(next)
    }

    func testDayIsAllTBDIsFalseForTheRealPack() throws {
        // Lost Lands 2026 has real published set times — never TBD.
        let pack = try loadLostLands()
        XCTAssertFalse(FestpackSchedule.dayIsAllTBD(in: pack, night: pack.startDayOfYear))
    }
}
